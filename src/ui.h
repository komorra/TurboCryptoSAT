// Colored ASCII dashboard: a stable map of clause satisfaction on the left and
// a live status panel on the right, both sized to the current terminal.
//
// Rendering rules that keep legacy consoles (cmd.exe / conhost) happy:
//   * the dashboard lives in the alternate screen buffer, so it never touches
//     the shell's scrollback and the wheel cannot scroll it out of place;
//   * every row is padded to exactly the terminal width and placed with an
//     absolute cursor move, so nothing ever wraps and nothing ever scrolls;
//   * only rows whose bytes changed since the previous frame are rewritten,
//     which is what removes the flicker.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cnf.h"
#include "platform.h"
#include "propagator.h"

namespace tcs {

struct UiModel {
    const Cnf* cnf = nullptr;
    const Propagator* prop = nullptr;
    const char* phase = "solving";

    int attempt = 1;
    int attempts = 5;
    int threads = 1;

    int assignedVars = 0;
    int numVars = 0;
    size_t satClauses = 0;
    size_t numClauses = 0;

    double elapsed = 0.0;
    double eta = -1.0;
    double varsPerSec = 0.0;

    uint64_t probes = 0;
    uint64_t productive = 0;
    uint64_t rejected = 0;
    uint64_t guesses = 0;
    uint64_t resamples = 0;
    uint32_t restarts = 0;

    int sigLen = 0;
    int initk = 0;
    int mink = 0;
    uint64_t validSamples = 0;
    uint64_t totalSamples = 0;
    uint64_t sigBytes = 0;
    int inputVarCount = 0;
    uint64_t gates = 0;            // recovered gates, 0 when the fast path is off
    bool gateSampling = false;

    ResourceSnapshot res;
};

class Ui {
public:
    // Computes the fixed clause display order. Clauses are ranked by length and
    // then by their smallest variable, so a cell always shows the same clauses
    // for the whole run.
    void prepare(const Cnf& cnf);

    void begin();
    void end();
    void render(const UiModel& m);
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

private:
    // One rendered row: the escape-laden bytes plus the number of columns they
    // actually occupy. Escapes are appended without touching the column count,
    // which is what lets a row be padded to an exact width.
    struct Line {
        std::string text;
        int visible = 0;

        void esc(const char* e) { text += e; }
        void esc(const std::string& e) { text += e; }
        void put(char c) { text += c; ++visible; }
        void put(const char* s, int budget);
        void put(const std::string& s, int budget) { put(s.c_str(), budget); }
        void pad(int width) {
            if (visible < width) {
                text.append(static_cast<size_t>(width - visible), ' ');
                visible = width;
            }
        }
    };

    void buildMap(const UiModel& m, int width, int height);
    void buildStatus(const UiModel& m, int width);
    void buildFrame(const UiModel& m, int cols, int rows, int mapW);

    bool enabled_ = true;
    bool started_ = false;
    bool tty_ = false;
    int lastCols_ = -1;
    int lastRows_ = -1;
    std::vector<uint32_t> order_;    // display rank -> clause index
    std::vector<Line> map_;          // left pane rows
    std::vector<Line> status_;       // right pane rows
    std::vector<Line> lines_;        // the whole frame, one entry per row
    std::vector<std::string> prev_;  // bytes drawn last frame, for the diff
    std::string out_;                // one write per frame
};

}  // namespace tcs
