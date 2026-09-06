#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>

namespace tcs {
namespace {

// Dark gray, then red through orange and yellow into green.
const int kRamp[] = {238, 52, 88, 130, 166, 178, 148, 112, 40, 46};
const char kChars[] = {'.', '.', ':', ':', '-', '-', '+', '+', '*', '#'};

std::string fg(int color) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "\x1b[38;5;%dm", color);
    return buf;
}

const char* kReset = "\x1b[0m";
const char* kDim = "\x1b[38;5;244m";
const char* kLabel = "\x1b[38;5;110m";
const char* kValue = "\x1b[38;5;255m";
const char* kAccent = "\x1b[38;5;214m";

void appendBar(std::string& out, double frac, int width, int color) {
    if (width < 3) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    const int filled = static_cast<int>(frac * (width - 2) + 0.5);
    out += kDim;
    out += '[';
    out += fg(color);
    for (int i = 0; i < width - 2; ++i) out += (i < filled ? '=' : ' ');
    out += kDim;
    out += ']';
    out += kReset;
}

std::string padTo(const std::string& visible, int width) {
    std::string s = visible;
    if (static_cast<int>(s.size()) < width) s.append(static_cast<size_t>(width) - s.size(), ' ');
    return s;
}

std::string field(const char* label, const std::string& value, int width) {
    std::string s = " ";
    s += kLabel;
    s += padTo(label, 13);
    s += kValue;
    s += value;
    s += kReset;
    // Padding is computed on the visible text only.
    const int visible = 1 + 13 + static_cast<int>(value.size());
    if (visible < width) s.append(static_cast<size_t>(width - visible), ' ');
    return s;
}

}  // namespace

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
    enableAnsi();
    std::fputs("\x1b[?25l\x1b[2J\x1b[H", stdout);
    std::fflush(stdout);
    started_ = true;
}

void Ui::end() {
    if (!started_) return;
    std::fputs("\x1b[?25h\x1b[0m\n", stdout);
    std::fflush(stdout);
    started_ = false;
}

void Ui::drawMap(const UiModel& m, int width, int height) {
    map_.clear();
    if (width < 4 || height < 1) return;
    const size_t cells = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t nc = order_.size();
    if (nc == 0) return;

    map_.reserve(static_cast<size_t>(height));
    std::string row;
    for (int r = 0; r < height; ++r) {
        row.clear();
        row.reserve(static_cast<size_t>(width) * 12);
        int lastColor = -1;
        for (int c = 0; c < width; ++c) {
            const size_t cellIndex = static_cast<size_t>(r) * static_cast<size_t>(width) + static_cast<size_t>(c);
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
            const int color = kRamp[level];
            if (color != lastColor) {
                row += fg(color);
                lastColor = color;
            }
            row += (total == 0 ? ' ' : kChars[level]);
        }
        row += kReset;
        map_.push_back(row);
    }
}

void Ui::statusLines(const UiModel& m, int width, std::vector<std::string>& out) {
    char buf[256];
    out.clear();

    out.push_back(std::string(" ") + kAccent + "STATUS" + kReset);
    out.push_back("");

    std::snprintf(buf, sizeof(buf), "%d / %d", m.attempt, m.attempts);
    out.push_back(field("attempt", buf, width));

    const double varFrac = m.numVars ? static_cast<double>(m.assignedVars) / m.numVars : 0.0;
    std::snprintf(buf, sizeof(buf), "%d / %d  (%.1f%%)", m.assignedVars, m.numVars, varFrac * 100.0);
    out.push_back(field("variables", buf, width));
    {
        std::string bar = " ";
        appendBar(bar, varFrac, std::min(width - 2, 40), 40);
        out.push_back(bar);
    }

    const double clFrac = m.numClauses ? static_cast<double>(m.satClauses) / static_cast<double>(m.numClauses) : 0.0;
    std::snprintf(buf, sizeof(buf), "%llu / %llu  (%.1f%%)",
                  static_cast<unsigned long long>(m.satClauses),
                  static_cast<unsigned long long>(m.numClauses), clFrac * 100.0);
    out.push_back(field("clauses sat", buf, width));
    {
        std::string bar = " ";
        appendBar(bar, clFrac, std::min(width - 2, 40), 214);
        out.push_back(bar);
    }
    out.push_back("");

    out.push_back(field("elapsed", formatDuration(m.elapsed), width));
    out.push_back(field("eta", m.eta >= 0 ? formatDuration(m.eta) : std::string("--:--:--"), width));
    std::snprintf(buf, sizeof(buf), "%.1f vars/s", m.varsPerSec);
    out.push_back(field("rate", buf, width));
    out.push_back("");

    std::snprintf(buf, sizeof(buf), "%llu (ok %llu, rej %llu)",
                  static_cast<unsigned long long>(m.probes),
                  static_cast<unsigned long long>(m.productive),
                  static_cast<unsigned long long>(m.rejected));
    out.push_back(field("probes", buf, width));
    std::snprintf(buf, sizeof(buf), "%llu    restarts %u",
                  static_cast<unsigned long long>(m.guesses), m.restarts);
    out.push_back(field("guesses", buf, width));
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(m.resamples));
    out.push_back(field("resamples", buf, width));
    out.push_back("");

    std::snprintf(buf, sizeof(buf), "%llu / %llu lanes",
                  static_cast<unsigned long long>(m.validSamples),
                  static_cast<unsigned long long>(m.totalSamples));
    out.push_back(field("samples", buf, width));
    std::snprintf(buf, sizeof(buf), "sigLen %d  initk %d  mink %d", m.sigLen, m.initk, m.mink);
    out.push_back(field("tuning", buf, width));
    std::snprintf(buf, sizeof(buf), "%d", m.inputVarCount);
    out.push_back(field("input vars", buf, width));
    out.push_back("");

    std::snprintf(buf, sizeof(buf), "%d", m.threads);
    out.push_back(field("threads", buf, width));
    std::snprintf(buf, sizeof(buf), "%.0f %%", m.res.cpuPercent);
    out.push_back(field("cpu", buf, width));
    std::string mem = formatBytes(m.res.rssBytes);
    if (m.res.totalRamBytes) mem += " / " + formatBytes(m.res.totalRamBytes);
    out.push_back(field("memory", mem, width));
    std::snprintf(buf, sizeof(buf), "%s", formatBytes(m.sigBytes).c_str());
    out.push_back(field("signatures", buf, width));
}

void Ui::render(const UiModel& m) {
    if (!enabled_) return;
    begin();

    const TermSize ts = terminalSize();
    if (ts.cols != lastCols_ || ts.rows != lastRows_) {
        lastCols_ = ts.cols;
        lastRows_ = ts.rows;
        std::fputs("\x1b[2J", stdout);
    }

    const int rows = ts.rows;
    const int cols = ts.cols;
    const int bodyRows = std::max(1, rows - 3);

    int mapW = cols * 3 / 5;
    if (mapW < 16) mapW = std::max(0, cols - 34);
    int statusW = cols - mapW - 1;
    if (statusW < 30) {
        statusW = std::min(cols, 30);
        mapW = std::max(0, cols - statusW - 1);
    }

    drawMap(m, mapW, bodyRows);
    std::vector<std::string> status;
    statusLines(m, statusW, status);

    frame_.clear();
    frame_ += "\x1b[H";

    char head[256];
    std::snprintf(head, sizeof(head), " TurboCryptoSAT  |  %s  |  %s ", m.phase,
                  m.cnf ? "signature propagation" : "");
    frame_ += "\x1b[48;5;24m\x1b[38;5;231m";
    frame_ += padTo(head, cols);
    frame_ += kReset;
    frame_ += "\x1b[K\n";

    for (int r = 0; r < bodyRows; ++r) {
        if (r < static_cast<int>(map_.size())) {
            frame_ += map_[static_cast<size_t>(r)];
        } else if (mapW > 0) {
            frame_.append(static_cast<size_t>(mapW), ' ');
        }
        if (mapW > 0) {  // a console too narrow for the map shows status only
            frame_ += kDim;
            frame_ += " |";
            frame_ += kReset;
        }
        if (r < static_cast<int>(status.size())) {
            frame_ += status[static_cast<size_t>(r)];
        }
        frame_ += "\x1b[K\n";
    }

    frame_ += kDim;
    frame_ += " clause map: sorted by length then first variable; ";
    frame_ += fg(46);
    frame_ += "#";
    frame_ += kDim;
    frame_ += " satisfied  ";
    frame_ += fg(238);
    frame_ += ".";
    frame_ += kDim;
    frame_ += " open";
    frame_ += kReset;
    frame_ += "\x1b[K\n";
    frame_ += kDim;
    frame_ += " press Ctrl+C or ESC ESC to abort";
    frame_ += kReset;
    frame_ += "\x1b[K";

    std::fwrite(frame_.data(), 1, frame_.size(), stdout);
    std::fflush(stdout);
}

}  // namespace tcs
