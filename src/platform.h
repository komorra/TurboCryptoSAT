// Cross-platform helpers: terminal geometry, ANSI setup, resource usage and
// interrupt handling (Ctrl+C or double ESC).
#pragma once

#include <cstdint>
#include <string>

namespace tcs {

struct TermSize {
    int cols = 80;
    int rows = 25;
};

// Current console geometry; falls back to 80x25 when it cannot be queried.
TermSize terminalSize();

// True when stdout is attached to an interactive console.
bool stdoutIsTty();

// Enables ANSI/VT escape sequence processing (no-op outside Windows).
void enableAnsi();

struct ResourceSnapshot {
    double cpuPercent = 0.0;   // summed over all cores, 100% == one saturated core
    uint64_t rssBytes = 0;
    uint64_t totalRamBytes = 0;
};

// Samples process CPU time and resident memory. Keeps the previous sample so
// that cpuPercent is a delta over the interval between two calls.
class ResourceMonitor {
public:
    ResourceMonitor();
    ResourceSnapshot sample();

private:
    uint64_t prevCpuNs_ = 0;
    uint64_t prevWallNs_ = 0;
    unsigned cores_ = 1;
};

// Installs the SIGINT/SIGTERM handler and (when stdin is a tty) a background
// watcher that treats two ESC presses within one second as an abort request.
void installInterruptHandlers();
void shutdownInterruptHandlers();
bool interruptRequested();
void requestInterrupt();

// Wall clock in nanoseconds since an arbitrary epoch.
uint64_t nowNs();

std::string formatDuration(double seconds);
std::string formatBytes(uint64_t bytes);

}  // namespace tcs
