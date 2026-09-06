// Colored ASCII dashboard: a stable map of clause satisfaction on the left and
// a live status panel on the right, both sized to the current terminal.
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
    void drawMap(const UiModel& m, int width, int height);
    void statusLines(const UiModel& m, int width, std::vector<std::string>& out);

    bool enabled_ = true;
    bool started_ = false;
    int lastCols_ = -1;
    int lastRows_ = -1;
    std::vector<uint32_t> order_;   // display rank -> clause index
    std::vector<std::string> map_;  // rendered map rows
    std::string frame_;
};

}  // namespace tcs
