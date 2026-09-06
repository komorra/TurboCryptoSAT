// Command line configuration shared by the solver, the UI and the benchmark runner.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tcs {

struct Options {
    std::string cnfPath;         // instance to solve
    std::string benchmarkDir;    // set when running in benchmark mode
    std::string solutionPath;    // override for the solution file

    // Input variable range, 1-based and inclusive; empty means auto-detect.
    int inputFrom = 0;
    int inputTo = 0;
    bool inputsGiven = false;

    int sigLen = 1024;           // 64-bit lanes per variable (1024 -> 65536 samples)
    int initk = 6;               // literals taken from the current assignment per probe;
                                 // halved on every restart
    int mink = 10;               // minimum surviving sample words for a signature verdict
    int probeVars = 1;           // variables probed at once (2^probeVars combinations)

    std::vector<int> outputs;    // DIMACS literals pinning the target valuation
    bool outputsGiven = false;

    int threads = 0;             // 0 -> hardware concurrency
    int attempts = 5;            // restarts before giving up
    double timeout = 0.0;        // seconds, 0 = unlimited
    uint64_t seed = 0;
    bool seedGiven = false;

    bool ui = true;
    bool quiet = false;
    bool verbose = false;
    bool keepSamples = false;    // reuse the sample population across restarts
    long long stallLimit = -1;   // -1 -> 1000 barren rounds
    int sampleRounds = 12;       // retry rounds while building the samples

    // What the solver does when the probes plateau: a bounded CDCL search over
    // the same formula, with everything committed so far pinned at level 0. The
    // budget is per phase and doubles whenever a phase proves nothing.
    bool cdcl = true;
    uint64_t cdclConflicts = 10000;  // 0 -> bounded only by --timeout

    // Tuning mode: search for the parameters that suit an instance family,
    // validated against a known solution. See tune.h.
    std::string tunePath;             // instance, or a directory of them
    std::string tuneSolution;         // explicit solution file for a single instance
    std::string tunePreset = "balanced";
    double tuneTrialTimeout = 30.0;   // seconds allowed per trial
    double tuneBudget = 0.0;          // seconds for the whole search, 0 = unlimited
    int tuneSeeds = 3;                // runs per setting; results are seed-noisy
};

}  // namespace tcs
