// Signature guided in-place SAT solving.
//
// The solver never searches. It repeatedly picks an unassigned variable, looks
// ahead into both of its polarities, and keeps only what both branches agree
// on. The agreement is computed twice: once by plain unit propagation and once
// through the sample population, which exposes implications that propagation
// alone cannot see. Because nothing is ever undone, a statistically wrong
// verdict can paint the instance into a corner - that is what the restart
// attempts are for.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cnf.h"
#include "gates.h"
#include "options.h"
#include "propagator.h"
#include "rng.h"
#include "signatures.h"
#include "threadpool.h"

namespace tcs {

enum class SolveStatus {
    Solved,
    Unsatisfiable,   // proven by propagation alone
    Exhausted,       // every attempt ran into a conflict
    Interrupted,
    Timeout,
    Error,
};

const char* toString(SolveStatus s);

struct SolveStats {
    uint64_t probes = 0;
    uint64_t productiveProbes = 0;
    uint64_t rejectedResults = 0;
    uint64_t signatureVerdicts = 0;
    uint64_t signatureBails = 0;
    uint64_t guesses = 0;
    uint64_t resamples = 0;
    uint32_t attempt = 1;
    uint32_t restarts = 0;
    double sampleSeconds = 0.0;
    double solveSeconds = 0.0;
    uint64_t validSamples = 0;
    uint64_t totalSamples = 0;
    uint64_t signatureBytes = 0;
    uint64_t gates = 0;          // gates recovered from the clauses
    bool gateSampling = false;   // samples produced by executing them
};

struct SolveResult {
    SolveStatus status = SolveStatus::Error;
    std::vector<int8_t> assignment;  // per variable: 1 true, -1 false
    SolveStats stats;
    std::string message;
};

class Solver {
public:
    Solver(const Cnf& cnf, const Options& opt);
    ~Solver();

    // Called roughly ten times per second and once per phase transition.
    using ProgressFn = std::function<void(const char* phase)>;
    void setProgressCallback(ProgressFn fn) { progress_ = std::move(fn); }

    SolveResult solve();

    // Live view used by the UI.
    const Propagator& master() const { return master_; }
    const SolveStats& stats() const { return stats_; }
    const std::vector<Var>& inputVars() const { return inputVars_; }
    const GateNetwork& gateNetwork() const { return gateNet_; }
    size_t targetLitCount() const { return targetLits_.size(); }

private:
    struct Worker;

    void buildSampleCnf();
    void detectInputs();
    bool prepareBase(std::string& error);
    bool buildSignatures(std::string& error);
    bool attempt(SolveStatus& status);
    bool applyLiterals(const std::vector<Lit>& lits, bool& conflict);
    void collectSortedInit();
    bool runProbe(Worker& w);
    void signatureOutcome(Worker& w, const std::vector<Lit>& forced, std::vector<Lit>& out);
    static void intersectLits(Worker& w, std::vector<Lit>& dst, const std::vector<Lit>& other);
    bool guessVariable();
    bool verify() const;
    void tick(const char* phase);

    const Cnf& cnf_;
    // The formula the sample population is drawn from: the same clauses without
    // the unit clauses, since those either pin the target outputs or are handed
    // to the generator as fixed literals.
    Cnf sampleCnf_;
    // The circuit read back out of `sampleCnf_`, when there was one. A complete
    // network is what the sample generator runs instead of propagating.
    GateNetwork gateNet_;
    Options opt_;
    Propagator master_;
    Signatures sig_;
    Rng rng_;

    std::vector<Lit> unitLits_;    // unit clauses present in the file
    std::vector<Lit> targetLits_;  // output valuation the solver must honour
    std::vector<Lit> sampleFixed_; // units that constrain the sample population
    std::vector<Var> inputVars_;
    std::vector<Lit> sortedInit_;  // assigned literals in variable order

    std::unique_ptr<ThreadPool> pool_;
    std::vector<std::unique_ptr<Worker>> workers_;

    SolveStats stats_;
    int effectiveInitk_ = 0;
    double lastSampleSeconds_ = 0.0;
    uint64_t startNs_ = 0;
    uint64_t lastTickNs_ = 0;
    ProgressFn progress_;
};

}  // namespace tcs
