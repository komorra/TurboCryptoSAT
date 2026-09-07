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
    // Minimum surviving samples for a signature verdict, counted in SAMPLES -
    // individual lanes, the popcount of the surviving lane words - and never in
    // 64-bit words. 640 is ten full lane words, which is what the tuning grid
    // behind this default actually measured; ten *samples* is no threshold at
    // all, since on a formula with 24k variables a dozen lanes leave a couple of
    // dozen variables looking constant by chance alone.
    int mink = 640;
    int probeVars = 1;           // variables probed at once (2^probeVars combinations)

    // How many bits of the target valuation the sample population has to
    // reproduce. 0 leaves the samples free executions of the circuit, which is
    // what the filtering step assumes; anything higher narrows the population
    // to the neighbourhood of the solution by redrawing lanes that miss those
    // bits, at a cost of roughly 2^focusBits redraws per lane.
    int focusBits = 8;

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

    // Linear reasoning over the parity constraints recovered from the clauses.
    // The root pass contributes derived binary clauses to the formula the search
    // runs on; later passes contribute proven units. See gf2.h.
    bool gf2 = true;
    int gf2Interval = 16;            // new assignments between elimination passes

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
