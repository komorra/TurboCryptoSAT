#include "ui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>

namespace tcs {
namespace {

// Dark gray, then red through orange and yellow into green.
const int kRamp[] = {238, 52, 88, 130, 166, 178, 148, 112, 40, 46};
const char kChars[] = {'.', '.', ':', ':', '-', '-', '+', '+', '*', '#'};

const char* const kReset = "\x1b[0m";
const char* const kDim = "\x1b[38;5;244m";
const char* const kLabel = "\x1b[38;5;110m";
const char* const kValue = "\x1b[38;5;255m";
const char* const kAccent = "\x1b[38;5;214m";
const char* const kHeader = "\x1b[48;5;24m\x1b[38;5;231m";

std::string fgSeq(int color) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "\x1b[38;5;%dm", color);
    return buf;
}

// The map redraws thousands of cells per frame, so the ten escapes it can
// possibly need are built once instead of on every cell.
const std::array<std::string, 10>& rampFg() {
    static const std::array<std::string, 10> t = [] {
        std::array<std::string, 10> a;
        for (int i = 0; i < 10; ++i) a[static_cast<size_t>(i)] = fgSeq(kRamp[i]);
        return a;
    }();
    return t;
}

}  // namespace

void Ui::Line::put(const char* s, int budget) {
    while (*s && budget > 0) {
        text += *s++;
        ++visible;
        --budget;
    }
}

void Ui::prepare(const Cnf& cnf) {
    const size_t nc = cnf.clauseCount();
    order_.resize(nc);
    std::iota(order_.begin(), order_.end(), 0u);
    std::vector<uint32_t> key(nc);
    for (size_t c = 0; c < nc; ++c) {
        uint32_t minVar = 0xFFFFFFFFu;
        const Lit* b = cnf.clauseBegin(c);
        for (uint32_t i = 0, e = cnf.clauseLen(c); i < e; ++i) {
            minVar = std::min<uint32_t>(minVar, static_cast<uint32_t>(b[i] >> 1));
        }
        key[c] = minVar;
    }
    std::sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) {
        const uint32_t la = cnf.clauseLen(a), lb = cnf.clauseLen(b);
        if (la != lb) return la < lb;
        if (key[a] != key[b]) return key[a] < key[b];
        return a < b;
    });
}

void Ui::begin() {
    if (!enabled_ || started_) return;
    tty_ = stdoutIsTty();
    // TCS_FORCE_UI=tty drives the incremental renderer into a redirected
    // stream, which is the only way to exercise that path from a test.
    if (const char* force = std::getenv("TCS_FORCE_UI")) {
        if (std::strcmp(force, "tty") == 0) tty_ = true;
    }
    if (!enableAnsi() && stdoutIsTty()) {
        // A console without VT support would only ever show raw escapes.
        enabled_ = false;
        return;
    }
    if (tty_) {
        // Alternate buffer, hidden cursor, autowrap off: the dashboard gets a
        // private screen that the shell's scrollback never sees. It comes up
        // blank, so the first frame's own clear is the only one needed.
        std::fputs("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[H", stdout);
    } else {
        std::fputs("\x1b[2J\x1b[H", stdout);
    }
    std::fflush(stdout);
    started_ = true;
}

void Ui::end() {
    if (!started_) return;
    if (tty_) {
        std::fputs("\x1b[?7h\x1b[?25h\x1b[0m\x1b[?1049l", stdout);
    } else {
        std::fputs("\x1b[0m\n", stdout);
    }
    std::fflush(stdout);
    started_ = false;
    lastCols_ = lastRows_ = -1;
    prev_.clear();
}

void Ui::buildMap(const UiModel& m, int width, int height) {
    map_.clear();
    if (width < 4 || height < 1) return;
    const size_t cells = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t nc = order_.size();
    if (nc == 0) return;

    const std::array<std::string, 10>& ramp = rampFg();
    map_.resize(static_cast<size_t>(height));
    for (int r = 0; r < height; ++r) {
        Line& row = map_[static_cast<size_t>(r)];
        row.text.reserve(static_cast<size_t>(width) + 64);
        int lastLevel = -1;
        for (int c = 0; c < width; ++c) {
            const size_t cellIndex =
                static_cast<size_t>(r) * static_cast<size_t>(width) + static_cast<size_t>(c);
            const size_t from = cellIndex * nc / cells;
            const size_t to = (cellIndex + 1) * nc / cells;
            size_t total = 0, sat = 0;
            for (size_t i = from; i < to; ++i) {
                ++total;
                if (m.prop->clauseSatisfied(order_[i])) ++sat;
            }
            int level;
            if (total == 0) {
                level = 0;
            } else {
                const double frac = static_cast<double>(sat) / static_cast<double>(total);
                level = static_cast<int>(frac * 9.0 + 0.5);
                if (level < 0) level = 0;
                if (level > 9) level = 9;
            }
            if (level != lastLevel) {
                row.esc(ramp[static_cast<size_t>(level)]);
                lastLevel = level;
            }
            row.put(total == 0 ? ' ' : kChars[level]);
        }
        row.esc(kReset);
    }
}

void Ui::buildStatus(const UiModel& m, int width) {
    status_.clear();
    if (width < 8) return;

    // The label column shrinks with the pane so a narrow console still shows a
    // useful amount of each value, and below the width where the roomy formats
    // fit the values switch to compact ones rather than being clipped.
    const int labelW = std::max(6, std::min(13, width / 2 - 2));
    const bool roomy = width >= 44;
    auto fmt = [&](const char* wide, const char* tight) { return roomy ? wide : tight; };
    char buf[256];

    auto blank = [&]() { status_.emplace_back(); };

    auto field = [&](const char* label, const char* value) {
        Line l;
        l.put(' ');
        l.esc(kLabel);
        l.put(label, std::min(labelW, width - l.visible));
        l.pad(1 + labelW);
        l.esc(kValue);
        l.put(value, width - l.visible);
        l.esc(kReset);
        status_.push_back(std::move(l));
    };

    auto bar = [&](double frac, int color) {
        Line l;
        l.put(' ');
        const int w = std::min(width - 2, 40);
        if (w >= 3) {
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            const int inner = w - 2;
            const int filled = static_cast<int>(frac * inner + 0.5);
            l.esc(kDim);
            l.put('[');
            l.esc(fgSeq(color));
            for (int i = 0; i < inner; ++i) l.put(i < filled ? '=' : ' ');
            l.esc(kDim);
            l.put(']');
            l.esc(kReset);
        }
        status_.push_back(std::move(l));
    };

    {
        Line l;
        l.put(' ');
        l.esc(kAccent);
        l.put("STATUS", width - 1);
        l.esc(kReset);
        status_.push_back(std::move(l));
    }
    blank();

    std::snprintf(buf, sizeof(buf), "%d / %d", m.attempt, m.attempts);
    field("attempt", buf);

    const double varFrac = m.numVars ? static_cast<double>(m.assignedVars) / m.numVars : 0.0;
    std::snprintf(buf, sizeof(buf), fmt("%d / %d  (%.1f%%)", "%d/%d %.0f%%"),
                  m.assignedVars, m.numVars, varFrac * 100.0);
    field("variables", buf);
    bar(varFrac, 40);

    const double clFrac =
        m.numClauses ? static_cast<double>(m.satClauses) / static_cast<double>(m.numClauses) : 0.0;
    std::snprintf(buf, sizeof(buf), fmt("%llu / %llu  (%.1f%%)", "%llu/%llu %.0f%%"),
                  static_cast<unsigned long long>(m.satClauses),
                  static_cast<unsigned long long>(m.numClauses), clFrac * 100.0);
    field("clauses sat", buf);
    bar(clFrac, 214);
    blank();

    field("elapsed", formatDuration(m.elapsed).c_str());
    field("eta", m.eta >= 0 ? formatDuration(m.eta).c_str() : "--:--:--");
    std::snprintf(buf, sizeof(buf), fmt("%.1f vars/s", "%.1f v/s"), m.varsPerSec);
    field("rate", buf);
    blank();

    std::snprintf(buf, sizeof(buf), fmt("%llu (ok %llu, rej %llu)", "%llu ok%llu rj%llu"),
                  static_cast<unsigned long long>(m.probes),
                  static_cast<unsigned long long>(m.productive),
                  static_cast<unsigned long long>(m.rejected));
    field("probes", buf);
    std::snprintf(buf, sizeof(buf), fmt("%llu    restarts %u", "%llu  rst %u"),
                  static_cast<unsigned long long>(m.guesses), m.restarts);
    field("guesses", buf);
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(m.resamples));
    field("resamples", buf);
    blank();

    std::snprintf(buf, sizeof(buf), fmt("%llu / %llu lanes", "%llu/%llu"),
                  static_cast<unsigned long long>(m.validSamples),
                  static_cast<unsigned long long>(m.totalSamples));
    field("samples", buf);
    std::snprintf(buf, sizeof(buf), fmt("sigLen %d  initk %d  mink %d", "%d/%d/%d"),
                  m.sigLen, m.initk, m.mink);
    field("tuning", buf);
    std::snprintf(buf, sizeof(buf), "%d", m.inputVarCount);
    field("input vars", buf);
    blank();

    std::snprintf(buf, sizeof(buf), "%d", m.threads);
    field("threads", buf);
    std::snprintf(buf, sizeof(buf), "%.0f %%", m.res.cpuPercent);
    field("cpu", buf);
    std::string mem = formatBytes(m.res.rssBytes);
    if (roomy && m.res.totalRamBytes) mem += " / " + formatBytes(m.res.totalRamBytes);
    field("memory", mem.c_str());
    field("signatures", formatBytes(m.sigBytes).c_str());
}

void Ui::buildFrame(const UiModel& m, int cols, int rows, int mapW) {
    lines_.assign(static_cast<size_t>(rows), Line());

    {
        Line& l = lines_[0];
        l.esc(kHeader);
        char head[256];
        std::snprintf(head, sizeof(head), " TurboCryptoSAT  |  %s  |  %s ", m.phase,
                      m.cnf ? "signature propagation" : "");
        l.put(head, cols);
        l.pad(cols);
        l.esc(kReset);
    }

    const int bodyRows = rows - 3;
    for (int r = 0; r < bodyRows; ++r) {
        Line& l = lines_[static_cast<size_t>(r + 1)];
        if (mapW > 0) {
            if (r < static_cast<int>(map_.size())) {
                const Line& src = map_[static_cast<size_t>(r)];
                l.text += src.text;
                l.visible += src.visible;
            }
            l.pad(mapW);
            l.esc(kDim);
            l.put(' ');
            l.put('|');
            l.esc(kReset);
        }
        if (r < static_cast<int>(status_.size())) {
            const Line& src = status_[static_cast<size_t>(r)];
            l.text += src.text;
            l.visible += src.visible;
        }
        l.pad(cols);
    }

    {
        Line& l = lines_[static_cast<size_t>(rows - 2)];
        l.esc(kDim);
        l.put(" clause map: sorted by length then first variable; ", cols - l.visible);
        l.esc(rampFg()[9]);
        l.put("#", cols - l.visible);
        l.esc(kDim);
        l.put(" satisfied  ", cols - l.visible);
        l.esc(rampFg()[0]);
        l.put(".", cols - l.visible);
        l.esc(kDim);
        l.put(" open", cols - l.visible);
        l.esc(kReset);
        l.pad(cols);
    }
    {
        Line& l = lines_[static_cast<size_t>(rows - 1)];
        l.esc(kDim);
        l.put(" press Ctrl+C or ESC ESC to abort", cols);
        l.esc(kReset);
        l.pad(cols);
    }
}

void Ui::render(const UiModel& m) {
    if (!enabled_) return;
    begin();
    if (!enabled_) return;

    const TermSize ts = terminalSize();
    const int cols = ts.cols;
    const int rows = ts.rows;
    const bool resized = (cols != lastCols_ || rows != lastRows_);
    if (resized) {
        lastCols_ = cols;
        lastRows_ = rows;
        prev_.assign(static_cast<size_t>(rows), std::string());
    }

    // Map, a one column gutter with a rule, then the status pane. The three
    // must add up to exactly cols or the rows wrap and the console scrolls.
    int statusW = 46;                              // enough for the roomy formats
    if (statusW > cols * 45 / 100) statusW = cols * 45 / 100;  // never over the map
    if (statusW < 30) statusW = std::min(cols, 30);
    int mapW = cols - statusW - 2;
    if (mapW < 12) {  // too narrow for a useful map: status only
        mapW = 0;
        statusW = cols;
    }

    const int bodyRows = std::max(1, rows - 3);
    buildMap(m, mapW, bodyRows);
    buildStatus(m, statusW);
    buildFrame(m, cols, rows, mapW);

    out_.clear();
    if (resized) out_ += "\x1b[2J";

    if (!tty_) {
        // Redirected output has no cursor to address, so the frame is written
        // whole, with newlines, which keeps screen captures readable.
        out_ += "\x1b[H";
        for (size_t r = 0; r < lines_.size(); ++r) {
            out_ += lines_[r].text;
            if (r + 1 < lines_.size()) out_ += '\n';
        }
    } else {
        char cup[16];
        for (int r = 0; r < rows; ++r) {
            const std::string& want = lines_[static_cast<size_t>(r)].text;
            if (!resized && prev_[static_cast<size_t>(r)] == want) continue;
            std::snprintf(cup, sizeof(cup), "\x1b[%d;1H", r + 1);
            out_ += cup;
            out_ += want;
            prev_[static_cast<size_t>(r)] = want;
        }
        if (out_.empty()) return;  // nothing moved, leave the screen untouched
        out_ += "\x1b[H";
    }

    std::fwrite(out_.data(), 1, out_.size(), stdout);
    std::fflush(stdout);
}

}  // namespace tcs
