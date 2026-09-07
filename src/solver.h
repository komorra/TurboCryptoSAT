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

#include "cdcl.h"
#include "gf2.h"
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
    OracleMismatch,  // tuning only: an assignment contradicted the known solution
    Error,
};

const char* toString(SolveStatus s);

// What preparing the base assignment concluded. UNSAT and "you passed nonsense"
// both stop the run, but only one of them is a statement about the formula, and
// reporting a bad argument as a mathematical result is how an automated caller
// ends up trusting it.
enum class PrepareResult {
    Ok,
    Unsat,         // the formula plus the requested valuation has no model
    InvalidInput,  // the request itself does not make sense
};

struct SolveStats {
    uint64_t probes = 0;
    uint64_t productiveProbes = 0;
    uint64_t rejectedResults = 0;
    uint64_t signatureVerdicts = 0;
    uint64_t signatureBails = 0;
    uint64_t resamples = 0;
    uint64_t cdclPhases = 0;      // bounded CDCL runs started at a plateau
    uint64_t cdclConflicts = 0;   // conflicts they analysed
    uint64_t cdclImplied = 0;     // literals they proved and handed back
    uint64_t cdclLearned = 0;     // clauses currently in the CDCL database
    uint32_t attempt = 1;
    uint32_t restarts = 0;
    double sampleSeconds = 0.0;
    double solveSeconds = 0.0;
    uint64_t validSamples = 0;
    uint64_t totalSamples = 0;
    uint64_t focusBits = 0;      // target bits the sample population must reproduce
    uint64_t focusLanes = 0;     // lanes that reproduce them
    uint64_t focusRounds = 0;    // redraws it dispatched getting there
    uint64_t signatureBytes = 0;
    uint64_t unusedVars = 0;     // variables no clause mentions
    int oracleVar = -1;          // 0-based variable that contradicted the oracle
    uint64_t assignedVars = 0;   // variables assigned when the run stopped
    uint64_t gf2Equations = 0;   // XOR constraints recovered from the clauses
    uint64_t gf2Vars = 0;        // variables they mention
    uint64_t gf2Runs = 0;        // elimination passes
    uint64_t gf2Units = 0;       // literals they proved
    uint64_t gf2Equivs = 0;      // equivalences found at the root
    uint64_t gf2Clauses = 0;     // ...of which propagation could not reach
    uint64_t gf2Conflicts = 0;   // passes that refuted the assignment
    double gf2Seconds = 0.0;     // time spent eliminating
    uint64_t gates = 0;          // gates recovered from the clauses
    uint64_t unexplained = 0;    // clauses no recovered gate accounts for
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

    // Tuning support. `oracle` holds one entry per variable, 1 or -1 for a known
    // solution and 0 for "no opinion"; it must outlive the solver. With one set,
    // the first literal committed against it ends the run as OracleMismatch
    // instead of letting it wander on toward a timeout - which is what makes a
    // parameter sweep cheap, since a bad setting is refuted in a fraction of a
    // second rather than burning the whole per-trial budget.
    //
    // It is a check, never a hint: nothing in the search reads it to decide
    // anything, so a tuned parameter set means the same thing on an instance
    // whose solution nobody has.
    void setOracle(const std::vector<int8_t>* oracle) { oracle_ = oracle; }

    // Live view used by the UI.
    const Propagator& master() const { return master_; }
    const SolveStats& stats() const { return stats_; }
    const std::vector<Var>& inputVars() const { return inputVars_; }
    const GateNetwork& gateNetwork() const { return gateNet_; }
    // Clauses the search actually runs on: the file's, plus whatever the root
    // GF(2) pass derived. The UI needs this as its denominator - clause indices
    // below `cnf.clauseCount()` still mean the same clauses, the derived ones
    // are appended after them.
    size_t searchClauses() const { return search_ ? search_->clauseCount() : 0; }
    size_t targetLitCount() const { return targetLits_.size(); }

private:
    friend struct SolverTestAccess;
    struct Worker;

    void buildSampleCnf();
    void detectInputs();
    PrepareResult prepareBase(std::string& error);
    bool buildSignatures(std::string& error);
    // Worker threads to use: --threads, or the hardware count when it is unset.
    // Both the sample generator and the probe pool ask here, so neither can
    // silently fall back to one thread.
    int workerThreads() const;
    bool attempt(SolveStatus& status);
    bool applyLiterals(const std::vector<Lit>& lits, bool& conflict);
    void collectSortedInit();
    bool runProbe(Worker& w);
    void signatureOutcome(Worker& w, const std::vector<Lit>& forced, std::vector<Lit>& out);
    static void intersectLits(Worker& w, std::vector<Lit>& dst, const std::vector<Lit>& other);
    // Bounded CDCL phase run in place of a guess when the probes plateau.
    // `progress` says whether it moved the assignment; the return value is
    // false only when the attempt is over (refuted, interrupted, out of time).
    bool runCdclPhase(SolveStatus& status, bool& progress);
    // One elimination pass over the current assignment. Applies whatever units
    // it proves; false means the attempt is over (refuted, or the oracle
    // tripped). `progress` says whether the assignment moved.
    bool runGf2(SolveStatus& status, bool& progress);
    // Recovers the parity constraints, reduces them once against the base
    // assignment, and folds the equivalences that come out into `augmented_`.
    PrepareResult prepareGf2(std::string& error);
    const Cnf& searchCnf() const { return *search_; }
    bool verify() const;
    // Scans master_'s trail from `from` for a literal the oracle contradicts.
    bool oracleBroken(size_t from);
    void tick(const char* phase);

    const Cnf& cnf_;
    // The formula the sample population is drawn from: the same clauses without
    // the unit clauses, since those either pin the target outputs or are handed
    // to the generator as fixed literals.
    Cnf sampleCnf_;
    // The formula the search actually runs on: the original clauses plus the
    // binary clauses the root GF(2) pass derived. Those are consequences of the
    // formula, so a model of this is a model of `cnf_` - which is what verify()
    // still checks against. The sample population is built from `cnf_` instead,
    // so the recovered gate network stays complete and the fast sampler stays
    // available.
    Cnf augmented_;
    const Cnf* search_ = nullptr;  // &cnf_, or &augmented_ once GF(2) added to it
    Gf2System gf2_;
    Gf2Result gf2Res_;
    std::vector<Lit> gf2New_;      // scratch: units master_ does not have yet
    size_t gf2LastAssigned_ = 0;   // master_ size at the last elimination pass
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

    // Kept alive across phases within one attempt so its learned clauses carry
    // over; dropped on a restart, since they are implied by the assignment the
    // restart retracts.
    std::unique_ptr<Cdcl> cdcl_;
    size_t cdclFedFromMaster_ = 0;  // how far into master_'s trail was pinned
    size_t cdclRootSeen_ = 0;       // how far into its level 0 trail was read
    uint64_t cdclBudget_ = 0;       // conflicts allowed in the next phase
    std::vector<Lit> cdclNew_;

    std::unique_ptr<ThreadPool> pool_;
    std::vector<std::unique_ptr<Worker>> workers_;

    const std::vector<int8_t>* oracle_ = nullptr;
    bool oracleTripped_ = false;
    // Why an attempt gave up with SolveStatus::Error. Set deep in the round
    // loop, where there is no SolveResult to write to yet.
    std::string errorMessage_;

    SolveStats stats_;
    int effectiveInitk_ = 0;
    double lastSampleSeconds_ = 0.0;
    uint64_t startNs_ = 0;
    uint64_t lastTickNs_ = 0;
    ProgressFn progress_;
};

}  // namespace tcs
