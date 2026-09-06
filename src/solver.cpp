#include "solver.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "platform.h"

namespace tcs {

const char* toString(SolveStatus s) {
    switch (s) {
        case SolveStatus::Solved: return "SOLVED";
        case SolveStatus::Unsatisfiable: return "UNSAT";
        case SolveStatus::Exhausted: return "EXHAUSTED";
        case SolveStatus::Interrupted: return "INTERRUPTED";
        case SolveStatus::Timeout: return "TIMEOUT";
        case SolveStatus::OracleMismatch: return "WRONG";
        case SolveStatus::Error: return "ERROR";
    }
    return "?";
}

struct Solver::Worker {
    Propagator prop;
    Rng rng;
    size_t synced = 0;
    size_t base = 0;

    std::vector<Lit> forcedBase;
    std::vector<Lit> forced;
    std::vector<Lit> sigOut;
    std::vector<Lit> branchLits;
    std::vector<Lit> sound;
    std::vector<Lit> stat;
    std::vector<Lit> result;
    std::vector<Lit> combo;
    std::vector<Var> probeVars;
    std::vector<int> aliveMasks;

    std::vector<int32_t> stamp;
    std::vector<int8_t> stampPol;
    int32_t stampCounter = 0;

    std::vector<uint64_t> keep;
    std::vector<uint32_t> keepIdx;
    std::vector<uint64_t> keepVal;
    std::vector<Var> candidates;

    bool hardConflict = false;
    uint64_t sigVerdicts = 0;
    uint64_t sigBails = 0;
};

Solver::Solver(const Cnf& cnf, const Options& opt)
    : cnf_(cnf), opt_(opt), rng_(opt.seed) {}

Solver::~Solver() = default;

void Solver::tick(const char* phase) {
    if (!progress_) return;
    const uint64_t now = nowNs();
    if (now - lastTickNs_ < 80ull * 1000ull * 1000ull) return;
    lastTickNs_ = now;
    progress_(phase);
}

// Picks the variables that drive the sample population.
//
// A user supplied range wins. Otherwise a single scalar pass plays the role of
// the sampler: variables are walked in index order and every one that is still
// undetermined is decided, the rest falling out by propagation. What remains is
// a dependency set - for a Tseitin encoded circuit exactly its inputs - and
// letting the bit parallel generator assign the whole set at once saves it one
// full propagation sweep per input.
void Solver::detectInputs() {
    inputVars_.clear();
    if (opt_.inputsGiven) {
        const int lo = std::max(1, opt_.inputFrom);
        const int hi = std::min(cnf_.numVars, opt_.inputTo);
        inputVars_.reserve(static_cast<size_t>(std::max(0, hi - lo + 1)));
        for (int v = lo; v <= hi; ++v) inputVars_.push_back(v - 1);
        return;
    }

    Propagator probe;
    probe.attach(sampleCnf_);
    for (Lit l : sampleFixed_) probe.enqueue(l);
    probe.propagate();

    for (Var v = 0; v < sampleCnf_.numVars; ++v) {
        if (probe.assigned(v)) continue;
        inputVars_.push_back(v);
        const size_t m = probe.mark();
        const bool pick = rng_.coin();
        if (probe.enqueue(mkLit(v, pick)) && probe.propagate()) continue;
        probe.undoTo(m);
        if (probe.enqueue(mkLit(v, !pick)) && probe.propagate()) continue;
        // Both polarities fail here; the lane will be retried by the generator.
        probe.undoTo(m);
        probe.enqueue(mkLit(v, pick));
        probe.propagate();
    }
}

bool Solver::prepareBase(std::string& error) {
    if (cnf_.hasEmptyClause) {
        error = "the formula contains an empty clause";
        return false;
    }
    master_.attach(cnf_);

    // Variables that occur in no clause at all. DIMACS files that declare fewer
    // variables than their largest index leave gaps like this, and the loader
    // widens numVars to whatever it sees, so the gaps become variables. Any
    // value satisfies the formula, but the solver still has to assign every one
    // of them before it can call the instance done - and left to the probe loop
    // that happens one statistical accident at a time. Pin them here instead:
    // they land in the base trail, so a restart keeps them.
    stats_.unusedVars = 0;
    for (Var v = 0; v < cnf_.numVars; ++v) {
        const size_t p = static_cast<size_t>(v) * 2u;
        if (cnf_.occStart[p] == cnf_.occStart[p + 2]) {
            // Any value works, but under a tuning oracle it has to be *that*
            // value: pinning the opposite would abort the trial on the spot for
            // a variable no clause even mentions.
            bool negate = false;
            if (oracle_ && static_cast<size_t>(v) < oracle_->size()) {
                negate = (*oracle_)[static_cast<size_t>(v)] < 0;
            }
            master_.enqueue(mkLit(v, negate));
            ++stats_.unusedVars;
        }
    }

    targetLits_.clear();
    sampleFixed_.clear();
    if (opt_.outputsGiven) {
        for (int d : opt_.outputs) {
            const int av = d < 0 ? -d : d;
            if (av < 1 || av > cnf_.numVars) {
                error = "output literal out of range: " + std::to_string(d);
                return false;
            }
            targetLits_.push_back(dimacsToLit(d));
        }
        // Unit clauses stay structural constants for the sample population.
        sampleFixed_ = unitLits_;
    } else {
        // Without an explicit valuation the unit clauses are the target: they
        // are what pins the outputs of the encoded circuit.
        targetLits_ = unitLits_;
    }

    for (Lit l : unitLits_) {
        if (!master_.enqueue(l)) {
            error = "unit clauses contradict each other";
            return false;
        }
    }
    for (Lit l : targetLits_) {
        if (!master_.enqueue(l)) {
            error = "the requested output valuation contradicts the formula";
            return false;
        }
    }
    if (!master_.propagate()) {
        error = "propagating the unit clauses and the output valuation conflicts";
        return false;
    }
    return true;
}

void Solver::buildSampleCnf() {
    sampleCnf_ = Cnf();
    sampleCnf_.numVars = cnf_.numVars;
    sampleCnf_.hasEmptyClause = cnf_.hasEmptyClause;
    sampleCnf_.lits.reserve(cnf_.lits.size());
    sampleCnf_.start.reserve(cnf_.start.size());
    sampleCnf_.start.push_back(0);
    const size_t nc = cnf_.clauseCount();
    for (size_t c = 0; c < nc; ++c) {
        const uint32_t len = cnf_.clauseLen(c);
        if (len < 2) continue;
        const Lit* b = cnf_.clauseBegin(c);
        sampleCnf_.lits.insert(sampleCnf_.lits.end(), b, b + len);
        sampleCnf_.start.push_back(static_cast<uint32_t>(sampleCnf_.lits.size()));
        if (len > sampleCnf_.maxClauseLen) sampleCnf_.maxClauseLen = len;
    }
    sampleCnf_.buildOccurrences();
}

bool Solver::buildSignatures(std::string& error) {
    // The table needs numVars * sigLen words, and twice that while the assigned
    // mask is still alive - executing a recovered circuit needs no such mask,
    // since it leaves nothing undecided. Say so up front rather than dying in
    // the allocator.
    {
        ResourceMonitor rm;
        const ResourceSnapshot snap = rm.sample();
        const uint64_t tables = gateNet_.complete() ? 1ull : 2ull;
        const uint64_t need = tables * static_cast<uint64_t>(cnf_.numVars) *
                              static_cast<uint64_t>(opt_.sigLen) * 8ull;
        if (snap.totalRamBytes && need > snap.totalRamBytes) {
            error = "the sample table would need " + formatBytes(need) + " but the machine has " +
                    formatBytes(snap.totalRamBytes) + "; lower --siglen";
            return false;
        }
    }

    SignatureConfig cfg;
    cfg.words = opt_.sigLen;
    cfg.seed = rng_.next();
    cfg.threads = opt_.threads;
    cfg.maxRounds = opt_.sampleRounds;
    cfg.gates = &gateNet_;
    cfg.cancelled = [this] {
        if (interruptRequested()) return true;
        return opt_.timeout > 0.0 &&
               static_cast<double>(nowNs() - startNs_) * 1e-9 > opt_.timeout;
    };
    const uint64_t t0 = nowNs();
    if (!sig_.generate(sampleCnf_, sampleFixed_, inputVars_, cfg, error)) return false;
    lastSampleSeconds_ = static_cast<double>(nowNs() - t0) * 1e-9;
    stats_.sampleSeconds += lastSampleSeconds_;
    stats_.validSamples = sig_.validSamples();
    stats_.totalSamples = sig_.sampleCount();
    stats_.signatureBytes = sig_.memoryBytes();
    stats_.gateSampling = sig_.gateSampling();
    // Reported even when the fast path was not taken: a network that explains
    // almost everything means one unmatched pattern, which is worth seeing.
    stats_.gates = gateNet_.gates.size();
    stats_.unexplained = gateNet_.residualClauses;
    return true;
}

// dst := dst intersect other, comparing literals (variable and polarity). Uses
// a per-worker stamp array so it stays linear and allocation free.
void Solver::intersectLits(Worker& w, std::vector<Lit>& dst, const std::vector<Lit>& other) {
    if (dst.empty()) return;
    ++w.stampCounter;
    for (Lit l : other) {
        const size_t v = static_cast<size_t>(l >> 1);
        w.stamp[v] = w.stampCounter;
        w.stampPol[v] = static_cast<int8_t>(l & 1);
    }
    size_t keep = 0;
    for (Lit l : dst) {
        const size_t v = static_cast<size_t>(l >> 1);
        if (w.stamp[v] == w.stampCounter && w.stampPol[v] == static_cast<int8_t>(l & 1)) {
            dst[keep++] = l;
        }
    }
    dst.resize(keep);
}

void Solver::collectSortedInit() {
    sortedInit_.clear();
    const std::vector<int8_t>& val = master_.values();
    sortedInit_.reserve(master_.assignedCount());
    for (int v = 0; v < cnf_.numVars; ++v) {
        const int8_t x = val[static_cast<size_t>(v)];
        if (x != 0) sortedInit_.push_back(mkLit(v, x < 0));
    }
}

void Solver::signatureOutcome(Worker& w, const std::vector<Lit>& forced, std::vector<Lit>& out) {
    const int words = sig_.words();
    const uint64_t* valid = sig_.validMask();

    w.keep.resize(static_cast<size_t>(words));
    std::memcpy(w.keep.data(), valid, static_cast<size_t>(words) * sizeof(uint64_t));

    for (Lit l : forced) {
        const uint64_t* s = sig_.var(l >> 1);
        if (l & 1) {
            for (int i = 0; i < words; ++i) w.keep[static_cast<size_t>(i)] &= ~s[i];
        } else {
            for (int i = 0; i < words; ++i) w.keep[static_cast<size_t>(i)] &= s[i];
        }
    }

    w.keepIdx.clear();
    w.keepVal.clear();
    for (int i = 0; i < words; ++i) {
        const uint64_t k = w.keep[static_cast<size_t>(i)];
        if (k) {
            w.keepIdx.push_back(static_cast<uint32_t>(i));
            w.keepVal.push_back(k);
        }
    }

    // Too few surviving samples: any verdict would be noise, so fall back to
    // what plain propagation already knows.
    if (static_cast<int>(w.keepIdx.size()) <= opt_.mink) {
        ++w.sigBails;
        out.assign(forced.begin(), forced.end());
        return;
    }

    out.clear();
    const size_t nk = w.keepIdx.size();
    const uint32_t* kidx = w.keepIdx.data();
    const uint64_t* kval = w.keepVal.data();
    const int nvars = cnf_.numVars;

    // Narrow the field with the transposed lane words first. Being constant on
    // a subset of the surviving lanes is a necessary condition, so this only
    // discards variables that the full test would have discarded too - but it
    // reads memory sequentially instead of jumping through the whole table.
    w.candidates.clear();
    int prefiltered = -1;
    for (int pw = 0; pw < sig_.probeWordCount(); ++pw) {
        const uint64_t k = w.keep[static_cast<size_t>(pw)];
        if (!k) continue;
        const uint64_t* col = sig_.probeWord(pw);
        if (prefiltered < 0) {
            for (int v = 0; v < nvars; ++v) {
                if (w.prop.assigned(v)) continue;
                const uint64_t sv = col[v] & k;
                if (sv == k || sv == 0) w.candidates.push_back(v);
            }
            prefiltered = 0;
        } else {
            size_t keep = 0;
            for (Var v : w.candidates) {
                const uint64_t sv = col[v] & k;
                if (sv == k || sv == 0) w.candidates[keep++] = v;
            }
            w.candidates.resize(keep);
        }
        if (w.candidates.size() < 64) break;
    }

    if (prefiltered < 0) {
        // No surviving lane in the transposed prefix; fall back to a full scan.
        w.candidates.clear();
        for (int v = 0; v < nvars; ++v) {
            if (!w.prop.assigned(v)) w.candidates.push_back(v);
        }
    }

    for (Var v : w.candidates) {
        const uint64_t* s = sig_.var(v);
        bool allTrue = true;
        bool allFalse = true;
        for (size_t i = 0; i < nk; ++i) {
            const uint64_t k = kval[i];
            const uint64_t sv = s[kidx[i]];
            if ((sv & k) != k) allTrue = false;
            if (sv & k) allFalse = false;
            if (!allTrue && !allFalse) break;
        }
        if (allTrue) {
            out.push_back(mkLit(v, false));
        } else if (allFalse) {
            out.push_back(mkLit(v, true));
        }
    }
    w.sigVerdicts += out.size();
}

bool Solver::runProbe(Worker& w) {
    // Catch up with everything the master has committed since the last round.
    const std::vector<Lit>& mt = master_.trail();
    for (size_t i = w.synced; i < mt.size(); ++i) {
        if (!w.prop.enqueue(mt[i])) {
            w.hardConflict = true;
            return false;
        }
    }
    w.synced = mt.size();
    if (!w.prop.propagate()) {
        w.hardConflict = true;
        return false;
    }
    w.base = w.prop.mark();
    w.result.clear();
    w.hardConflict = false;

    const int nvars = cnf_.numVars;
    const int k = std::max(1, std::min(opt_.probeVars, 16));

    // Pick k variables: scan forward from a random offset and take the first
    // unassigned ones. This is deliberately not a uniform draw over the
    // unassigned set - it favours variables that sit just past a run of already
    // assigned ones, which is where propagation has the most to work with.
    w.probeVars.clear();
    const Var start = static_cast<Var>(w.rng.below(static_cast<uint32_t>(nvars)));
    for (int i = 0; i < nvars && static_cast<int>(w.probeVars.size()) < k; ++i) {
        const Var v = (start + i) % nvars;
        if (!w.prop.assigned(v)) w.probeVars.push_back(v);
    }
    if (w.probeVars.empty()) return false;

    const int nc = static_cast<int>(w.probeVars.size());
    const int combos = 1 << nc;
    const int rot = static_cast<int>(w.rng.below(static_cast<uint32_t>(combos)));

    // Stage one: plain propagation on every branch. Two things fall out of it
    // for free, and both are sound rather than statistical. A branch that
    // conflicts is refuted outright; and whatever every surviving branch
    // propagates in common holds no matter which branch is the real one - the
    // dilemma rule. The closures are needed anyway to filter the branches, so
    // intersecting them costs nothing.
    bool firstSound = true;
    w.sound.clear();
    w.aliveMasks.clear();
    std::vector<Lit>& combo = w.combo;
    for (int ci = 0; ci < combos; ++ci) {
        const int mask = ci ^ rot;
        combo.clear();
        for (int b = 0; b < nc; ++b) {
            combo.push_back(mkLit(w.probeVars[static_cast<size_t>(b)], (mask >> b) & 1));
        }
        bool ok = true;
        for (Lit l : combo) {
            if (!w.prop.enqueue(l)) { ok = false; break; }
        }
        if (ok) ok = w.prop.propagate();
        if (ok) {
            w.aliveMasks.push_back(mask);
            const std::vector<Lit>& tr = w.prop.trail();
            w.branchLits.assign(tr.begin() + static_cast<std::ptrdiff_t>(w.base), tr.end());
            if (firstSound) {
                w.sound = w.branchLits;
                firstSound = false;
            } else {
                intersectLits(w, w.sound, w.branchLits);
            }
        }
        w.prop.undoTo(w.base);
    }

    if (w.aliveMasks.empty()) {
        w.hardConflict = true;  // every polarity refuted: the assignment is dead
        return false;
    }

    // The sound half of the verdict, whatever the samples say below. With one
    // surviving combination that is the failed literal rule and w.sound is its
    // whole closure; with several it is the dilemma rule. The single survivor
    // case deliberately falls through to the statistical layer as well: the
    // reference implementation runs SigOutcome on the branch that lived through
    // the other one's conflict, and that is where the filter window is at its
    // most informative, since it now holds a literal known to be forced.
    w.result = w.sound;

    // A random window of the current assignment, taken in variable order so the
    // forced literals stay topologically close to each other. It is drawn once
    // per probe: both branches have to filter the sample population the same
    // way, otherwise their outcomes are not comparable and the intersection
    // below is meaningless.
    w.forcedBase.clear();
    if (!sortedInit_.empty() && effectiveInitk_ > 0) {
        const size_t n = sortedInit_.size();
        const size_t startAt = w.rng.below(static_cast<uint32_t>(n));
        const size_t take = std::min<size_t>(static_cast<size_t>(effectiveInitk_), n);
        for (size_t i = 0; i < take; ++i) {
            w.forcedBase.push_back(sortedInit_[(startAt + i) % n]);
        }
    }

    // Stage two: the statistical layer. Each surviving branch has the sample
    // population filtered by the same window of assigned literals plus its own
    // polarity; whatever every branch then agrees on is a candidate implication.
    bool first = true;
    for (size_t ai = 0; ai < w.aliveMasks.size() && !(!first && w.stat.empty()); ++ai) {
        const int mask = w.aliveMasks[ai];
        combo.clear();
        for (int b = 0; b < nc; ++b) {
            combo.push_back(mkLit(w.probeVars[static_cast<size_t>(b)], (mask >> b) & 1));
        }

        w.forced = w.forcedBase;
        for (Lit l : combo) w.forced.push_back(l);

        signatureOutcome(w, w.forced, w.sigOut);

        bool ok = true;
        for (Lit l : w.sigOut) {
            if (!w.prop.enqueue(l)) { ok = false; break; }
        }
        if (ok) ok = w.prop.propagate();
        if (!ok) {
            w.prop.undoTo(w.base);
            continue;
        }
        const std::vector<Lit>& tr = w.prop.trail();
        w.branchLits.assign(tr.begin() + static_cast<std::ptrdiff_t>(w.base), tr.end());
        w.prop.undoTo(w.base);

        if (first) {
            w.stat = w.branchLits;
            first = false;
        } else {
            intersectLits(w, w.stat, w.branchLits);
        }
    }
    if (!first) {
        w.result.insert(w.result.end(), w.stat.begin(), w.stat.end());
    }
    return !w.result.empty();
}

// The answer to a plateau, in place of a guess the solver could not take back.
//
// A conventional CDCL search runs over the same formula with everything the
// signature loop has committed pinned as level 0 units. Whatever it then puts
// on its own level 0 trail is a learned unit: it follows from the formula and
// the pinned literals alone, so handing it back is inference, not a bet. The
// phase is bounded by a conflict budget - when it expires with nothing proven
// the budget doubles and the probes carry on, and because the learned clauses
// stay, the next phase resumes where this one stopped.
bool Solver::runCdclPhase(SolveStatus& status, bool& progress) {
    progress = false;
    if (!opt_.cdcl) return true;

    if (!cdcl_) {
        cdcl_.reset(new Cdcl());
        if (!cdcl_->attach(cnf_, rng_.next())) {
            status = SolveStatus::Exhausted;
            return false;
        }
        cdclFedFromMaster_ = 0;
        cdclRootSeen_ = cdcl_->rootTrail().size();
        cdclBudget_ = opt_.cdclConflicts;
    }

    // Pin everything the signature loop has committed since the last phase.
    const std::vector<Lit>& mt = master_.trail();
    for (; cdclFedFromMaster_ < mt.size(); ++cdclFedFromMaster_) {
        if (!cdcl_->addRoot(mt[cdclFedFromMaster_])) {
            status = SolveStatus::Exhausted;  // the assignment is refuted
            return false;
        }
    }
    cdclRootSeen_ = std::max(cdclRootSeen_, cdcl_->rootTrail().size());

    ++stats_.cdclPhases;
    tick("cdcl");
    const uint64_t conflictsBefore = cdcl_->conflicts();
    auto cancelled = [this] {
        if (interruptRequested()) return true;
        return opt_.timeout > 0.0 &&
               static_cast<double>(nowNs() - startNs_) * 1e-9 > opt_.timeout;
    };
    const CdclResult r = cdcl_->run(cdclBudget_, cancelled);
    stats_.cdclConflicts += cdcl_->conflicts() - conflictsBefore;
    stats_.cdclLearned = cdcl_->learnedClauses();

    switch (r) {
        case CdclResult::Implied: {
            const std::vector<Lit>& rt = cdcl_->rootTrail();
            cdclNew_.assign(rt.begin() + static_cast<std::ptrdiff_t>(cdclRootSeen_), rt.end());
            cdclRootSeen_ = rt.size();
            stats_.cdclImplied += cdclNew_.size();
            bool conflict = false;
            progress = applyLiterals(cdclNew_, conflict);
            if (conflict) {
                status = SolveStatus::Exhausted;
                return false;
            }
            return true;
        }
        case CdclResult::Solved: {
            // The search finished the instance outright. Its model extends the
            // pinned assignment, so it drops straight into master_.
            const std::vector<int8_t>& m = cdcl_->model();
            for (Var v = 0; v < cnf_.numVars; ++v) {
                if (!master_.enqueue(mkLit(v, m[static_cast<size_t>(v)] < 0))) {
                    status = SolveStatus::Exhausted;
                    return false;
                }
            }
            if (!master_.propagate()) {
                status = SolveStatus::Exhausted;
                return false;
            }
            if (oracleBroken(0)) {
                // A different satisfying assignment, not a wrong one - but the
                // tuning run is measuring progress towards *this* solution, so
                // it stops here either way.
                status = SolveStatus::OracleMismatch;
                return false;
            }
            progress = true;
            return true;
        }
        case CdclResult::RootConflict:
            status = SolveStatus::Exhausted;
            return false;
        case CdclResult::Aborted:
            status = interruptRequested() ? SolveStatus::Interrupted : SolveStatus::Timeout;
            return false;
        case CdclResult::Budget:
        default:
            cdclBudget_ *= 2;
            return true;
    }
}

bool Solver::applyLiterals(const std::vector<Lit>& lits, bool& conflict) {
    conflict = false;
    if (lits.empty()) return false;
    const size_t m = master_.mark();
    bool ok = true;
    for (Lit l : lits) {
        if (!master_.enqueue(l)) { ok = false; break; }
    }
    if (ok) ok = master_.propagate();
    if (!ok) {
        master_.undoTo(m);
        conflict = true;
        return false;
    }
    if (oracleBroken(m)) {
        oracleTripped_ = true;
        return false;
    }
    return master_.mark() > m;
}

bool Solver::oracleBroken(size_t from) {
    if (!oracle_) return false;
    const std::vector<Lit>& tr = master_.trail();
    const std::vector<int8_t>& o = *oracle_;
    for (size_t i = from; i < tr.size(); ++i) {
        const Var v = litVar(tr[i]);
        if (static_cast<size_t>(v) >= o.size()) continue;
        const int8_t want = o[static_cast<size_t>(v)];
        if (want == 0) continue;
        const int8_t got = litSign(tr[i]) ? static_cast<int8_t>(-1) : static_cast<int8_t>(1);
        if (got != want) {
            stats_.oracleVar = v;
            return true;
        }
    }
    return false;
}

bool Solver::verify() const {
    const std::vector<int8_t>& val = master_.values();
    const size_t nc = cnf_.clauseCount();
    for (size_t c = 0; c < nc; ++c) {
        bool sat = false;
        const Lit* b = cnf_.clauseBegin(c);
        const uint32_t len = cnf_.clauseLen(c);
        for (uint32_t i = 0; i < len; ++i) {
            const int8_t x = val[static_cast<size_t>(b[i] >> 1)];
            if ((b[i] & 1) ? x < 0 : x > 0) { sat = true; break; }
        }
        if (!sat) return false;
    }
    return true;
}

bool Solver::attempt(SolveStatus& status) {
    const int nvars = cnf_.numVars;
    long long stallLimit = opt_.stallLimit;
    if (stallLimit < 0) stallLimit = 1000;
    long long stall = 0;
    int resamplesSinceProgress = 0;
    const int kResampleAttempts = 2;

    // The "direct" pass of the reference implementation: ask the samples what
    // the current assignment alone already determines.
    {
        Worker& w = *workers_[0];
        w.synced = 0;
        w.prop.undoTo(0);
        for (Lit l : master_.trail()) w.prop.enqueue(l);
        w.prop.propagate();
        w.synced = master_.trail().size();
        w.base = w.prop.mark();
        collectSortedInit();
        signatureOutcome(w, sortedInit_, w.sigOut);
        bool conflict = false;
        applyLiterals(w.sigOut, conflict);
    }

    while (static_cast<int>(master_.assignedCount()) < nvars) {
        if (oracleTripped_) { status = SolveStatus::OracleMismatch; return false; }
        if (interruptRequested()) { status = SolveStatus::Interrupted; return false; }
        if (opt_.timeout > 0.0 &&
            static_cast<double>(nowNs() - startNs_) * 1e-9 > opt_.timeout) {
            status = SolveStatus::Timeout;
            return false;
        }

        collectSortedInit();

        const int n = pool_->size();
        pool_->run([&](int i) {
            Worker& w = *workers_[static_cast<size_t>(i)];
            runProbe(w);
        });

        bool hard = false;
        bool grew = false;
        for (int i = 0; i < n; ++i) {
            Worker& w = *workers_[static_cast<size_t>(i)];
            ++stats_.probes;
            stats_.signatureVerdicts += w.sigVerdicts;
            stats_.signatureBails += w.sigBails;
            w.sigVerdicts = 0;
            w.sigBails = 0;
            if (w.hardConflict) { hard = true; continue; }
            bool conflict = false;
            if (applyLiterals(w.result, conflict)) {
                grew = true;
                ++stats_.productiveProbes;
            } else if (conflict) {
                ++stats_.rejectedResults;
            }
        }

        if (oracleTripped_) { status = SolveStatus::OracleMismatch; return false; }

        if (hard) {
            status = SolveStatus::Exhausted;
            return false;
        }

        if (grew) {
            stall = 0;
            resamplesSinceProgress = 0;
        } else if (++stall > stallLimit) {
            stall = 0;
            // A plateau means the probes have stopped finding agreement. Fresh
            // randomness is the cheapest response, so redraw the sample
            // population first and only fall through to a CDCL phase once a new
            // population has failed to help either.
            // ...but only while redrawing stays cheap relative to the run.
            // On a large instance a population costs seconds to build, and
            // spending the whole budget on it would starve the probes.
            const double elapsed = static_cast<double>(nowNs() - startNs_) * 1e-9;
            // A population can only be redrawn while it stays a small share of
            // the run, and never so late that it would overshoot the deadline:
            // sampling is only interruptible between its retry rounds.
            const bool sampleBudgetLeft =
                stats_.sampleSeconds < 0.15 * elapsed &&
                (opt_.timeout <= 0.0 || opt_.timeout - elapsed > 2.0 * lastSampleSeconds_);
            if (!opt_.keepSamples && sampleBudgetLeft &&
                resamplesSinceProgress < kResampleAttempts) {
                ++resamplesSinceProgress;
                ++stats_.resamples;
                tick("resampling");
                std::string err;
                if (buildSignatures(err)) {
                    for (auto& w : workers_) w->keep.resize(static_cast<size_t>(sig_.words()));
                    continue;
                }
            }
            bool cdclProgress = false;
            if (!runCdclPhase(status, cdclProgress)) return false;
            if (cdclProgress) resamplesSinceProgress = 0;
        }

        tick("solving");
    }

    if (!verify()) {
        status = SolveStatus::Exhausted;
        return false;
    }
    status = SolveStatus::Solved;
    return true;
}

SolveResult Solver::solve() {
    SolveResult res;
    startNs_ = nowNs();
    lastTickNs_ = 0;

    if (!opt_.seedGiven) {
        rng_.reseed(nowNs() ^ 0xA5A5A5A5DEADBEEFull);
    }

    std::string error;
    std::vector<Lit> units;
    unitLits_.clear();
    for (size_t c = 0; c < cnf_.clauseCount(); ++c) {
        if (cnf_.clauseLen(c) == 1) unitLits_.push_back(cnf_.clauseBegin(c)[0]);
    }

    if (!prepareBase(error)) {
        res.status = SolveStatus::Unsatisfiable;
        res.message = error;
        return res;
    }
    buildSampleCnf();
    // Reading the gates back is linear in the formula and pays for itself many
    // times over: a circuit is executed once per population instead of being
    // propagated, which is what makes a redraw cheap enough to lean on.
    extractGates(sampleCnf_, gateNet_);
    detectInputs();
    const size_t baseTrail = master_.mark();

    tick("sampling");
    if (!buildSignatures(error)) {
        res.status = SolveStatus::Error;
        res.message = error;
        return res;
    }

    int threads = opt_.threads > 0 ? opt_.threads
                                   : static_cast<int>(std::thread::hardware_concurrency());
    if (threads <= 0) threads = 1;
    pool_.reset(new ThreadPool(threads));
    workers_.clear();
    workers_.reserve(static_cast<size_t>(pool_->size()));
    for (int i = 0; i < pool_->size(); ++i) {
        std::unique_ptr<Worker> w(new Worker());
        w->prop.attach(cnf_);
        w->rng.reseed(rng_.next());
        w->stamp.assign(static_cast<size_t>(cnf_.numVars), 0);
        w->stampPol.assign(static_cast<size_t>(cnf_.numVars), 0);
        w->keep.resize(static_cast<size_t>(sig_.words()));
        workers_.push_back(std::move(w));
    }

    if (oracleBroken(0)) {
        res.status = SolveStatus::OracleMismatch;
        res.message = "the unit clauses already contradict the reference solution";
        stats_.assignedVars = master_.assignedCount();
        res.stats = stats_;
        return res;
    }

    SolveStatus status = SolveStatus::Exhausted;
    const uint64_t solveStart = nowNs();

    for (int a = 1; a <= std::max(1, opt_.attempts); ++a) {
        stats_.attempt = static_cast<uint32_t>(a);

        // How many assigned literals each probe filters the samples with is the
        // one parameter with no safe default: a wide filter leaves too many
        // samples for anything to look constant, a narrow one leaves so few
        // that merely biased variables pass for implied ones and poison the
        // assignment. Each restart halves it, so the attempts double as a
        // search over that trade-off, from aggressive to conservative.
        effectiveInitk_ = std::max(1, opt_.initk >> (a - 1));

        master_.undoTo(baseTrail);
        for (auto& w : workers_) {
            w->prop.undoTo(0);
            w->synced = 0;
        }
        // Learned clauses are implied by the assignment this restart retracts,
        // so they go with it.
        if (cdcl_) {
            cdcl_->reset();
            cdclFedFromMaster_ = 0;
            cdclRootSeen_ = cdcl_->rootTrail().size();
            cdclBudget_ = opt_.cdclConflicts;
        }

        if (a > 1 && !opt_.keepSamples) {
            tick("resampling");
            if (!buildSignatures(error)) {
                res.status = SolveStatus::Error;
                res.message = error;
                return res;
            }
            for (auto& w : workers_) w->keep.resize(static_cast<size_t>(sig_.words()));
        }

        if (attempt(status)) break;
        if (status == SolveStatus::Interrupted || status == SolveStatus::Timeout ||
            status == SolveStatus::OracleMismatch) {
            break;  // tuning: a contradicted literal ends the trial, not just the attempt
        }
        ++stats_.restarts;
    }

    stats_.solveSeconds = static_cast<double>(nowNs() - solveStart) * 1e-9;
    stats_.assignedVars = master_.assignedCount();
    res.status = status;
    res.stats = stats_;
    if (status == SolveStatus::Solved) {
        res.assignment.assign(master_.values().begin(), master_.values().end());
        for (auto& v : res.assignment) {
            if (v == 0) v = 1;  // free variables: any value works
        }
    }
    return res;
}

}  // namespace tcs
