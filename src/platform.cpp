#include "platform.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <conio.h>
#  include <io.h>
#else
#  include <unistd.h>
#  include <termios.h>
#  include <sys/ioctl.h>
#  include <sys/select.h>
#  include <sys/sysinfo.h>
#endif

namespace tcs {
namespace {

std::atomic<bool> g_interrupt{false};
std::atomic<bool> g_watcherRun{false};
std::thread g_watcher;

#if !defined(_WIN32)
termios g_savedTermios;
bool g_termiosSaved = false;
#endif

extern "C" void signalHandler(int) {
    g_interrupt.store(true, std::memory_order_relaxed);
}

// Polls the keyboard and flags an abort after two ESC presses within a second.
void keyWatcherLoop() {
    uint64_t lastEsc = 0;
    while (g_watcherRun.load(std::memory_order_relaxed)) {
        int ch = -1;
#if defined(_WIN32)
        if (_kbhit()) {
            ch = _getch();
        }
#else
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        timeval tv{0, 50 * 1000};
        if (::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            unsigned char c = 0;
            if (::read(STDIN_FILENO, &c, 1) == 1) {
                ch = c;
            }
        }
#endif
        if (ch == 27) {
            const uint64_t now = nowNs();
            if (lastEsc != 0 && now - lastEsc < 1000ull * 1000ull * 1000ull) {
                g_interrupt.store(true, std::memory_order_relaxed);
                return;
            }
            lastEsc = now;
        } else if (ch == 3 || ch == 'q' || ch == 'Q') {
            g_interrupt.store(true, std::memory_order_relaxed);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
}

// Total CPU time this process has burned so far, kernel plus user.
uint64_t processCpuNs() {
#if defined(_WIN32)
    FILETIME creation, exitT, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitT, &kernel, &user)) return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 100ull;  // FILETIME ticks are 100ns
#else
    FILE* f = std::fopen("/proc/self/stat", "r");
    if (!f) return 0;
    // utime/stime are fields 14 and 15, counted after the parenthesised comm.
    char buf[4096];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    char* p = std::strrchr(buf, ')');
    if (!p || !p[1]) return 0;
    int field = 2;
    unsigned long utime = 0, stime = 0;
    char* save = nullptr;
    char* tok = strtok_r(p + 2, " ", &save);
    while (tok) {
        ++field;
        if (field == 14) utime = std::strtoul(tok, nullptr, 10);
        if (field == 15) { stime = std::strtoul(tok, nullptr, 10); break; }
        tok = strtok_r(nullptr, " ", &save);
    }
    const long hz = ::sysconf(_SC_CLK_TCK) > 0 ? ::sysconf(_SC_CLK_TCK) : 100;
    return static_cast<uint64_t>(utime + stime) * (1000000000ull / static_cast<uint64_t>(hz));
#endif
}

}  // namespace

uint64_t nowNs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

TermSize terminalSize() {
    TermSize ts;
    // TCS_TERM_SIZE=<cols>x<rows> pins the geometry, which is how the dashboard
    // layout gets checked at sizes the test machine's console cannot take.
    if (const char* forced = std::getenv("TCS_TERM_SIZE")) {
        int c = 0, r = 0;
        if (std::sscanf(forced, "%dx%d", &c, &r) == 2 && c > 0 && r > 0) {
            ts.cols = c;
            ts.rows = r;
            if (ts.cols < 20) ts.cols = 20;
            if (ts.rows < 8) ts.rows = 8;
            return ts;
        }
    }
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
        ts.cols = info.srWindow.Right - info.srWindow.Left + 1;
        ts.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    winsize ws;
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ts.cols = ws.ws_col;
        ts.rows = ws.ws_row;
    }
#endif
    if (ts.cols < 20) ts.cols = 20;
    if (ts.rows < 8) ts.rows = 8;
    return ts;
}

bool stdoutIsTty() {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

bool enableAnsi() {
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;  // redirected, or not a console
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    return true;
#endif
}

ResourceMonitor::ResourceMonitor() {
    cores_ = std::thread::hardware_concurrency();
    if (cores_ == 0) cores_ = 1;
    prevWallNs_ = nowNs();
    // Seeding from the real counter keeps the first sample from charging
    // the whole of process start-up to one short interval.
    prevCpuNs_ = processCpuNs();
}

ResourceSnapshot ResourceMonitor::sample() {
    ResourceSnapshot s;
    const uint64_t cpuNs = processCpuNs();
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        s.rssBytes = pmc.WorkingSetSize;
    }
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.totalRamBytes = ms.ullTotalPhys;
    }
#else
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long total = 0, resident = 0;
        if (std::fscanf(f, "%lu %lu", &total, &resident) == 2) {
            s.rssBytes = static_cast<uint64_t>(resident) *
                         static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
        }
        std::fclose(f);
    }
    struct sysinfo si;
    if (::sysinfo(&si) == 0) {
        s.totalRamBytes = static_cast<uint64_t>(si.totalram) * si.mem_unit;
    }
#endif
    const uint64_t wall = nowNs();
    s.cores = cores_;
    if (wall > prevWallNs_ && cpuNs >= prevCpuNs_) {
        const double dt = static_cast<double>(wall - prevWallNs_);
        s.coresBusy = static_cast<double>(cpuNs - prevCpuNs_) / dt;
        if (s.coresBusy > static_cast<double>(cores_)) s.coresBusy = cores_;
        s.cpuPercent = 100.0 * s.coresBusy / static_cast<double>(cores_);
    }
    prevWallNs_ = wall;
    prevCpuNs_ = cpuNs;
    return s;
}

void installInterruptHandlers() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    bool stdinTty;
#if defined(_WIN32)
    stdinTty = _isatty(_fileno(stdin)) != 0;
#else
    stdinTty = ::isatty(STDIN_FILENO) != 0;
    if (stdinTty && ::tcgetattr(STDIN_FILENO, &g_savedTermios) == 0) {
        g_termiosSaved = true;
        termios raw = g_savedTermios;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
#endif
    if (stdinTty) {
        g_watcherRun.store(true, std::memory_order_relaxed);
        g_watcher = std::thread(keyWatcherLoop);
    }
}

void shutdownInterruptHandlers() {
    g_watcherRun.store(false, std::memory_order_relaxed);
    if (g_watcher.joinable()) {
        // The watcher may sit in a blocking read; detaching avoids stalling exit.
        g_watcher.detach();
    }
#if !defined(_WIN32)
    if (g_termiosSaved) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTermios);
        g_termiosSaved = false;
    }
#endif
}

bool interruptRequested() { return g_interrupt.load(std::memory_order_relaxed); }
void requestInterrupt() { g_interrupt.store(true, std::memory_order_relaxed); }

std::string formatDuration(double seconds) {
    if (seconds < 0 || seconds > 359999.0) return "--:--:--";
    const int total = static_cast<int>(seconds);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    return buf;
}

std::string formatBytes(uint64_t bytes) {
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), v < 10.0 ? "%.1f %s" : "%.0f %s", v, kUnits[u]);
    return buf;
}

}  // namespace tcs
