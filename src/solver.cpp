#include "solver_worker.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <thread>

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

Solver::Solver(const Cnf& cnf, const Options& opt)
    : cnf_(cnf), search_(&cnf), opt_(opt), rng_(opt.seed) {}

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
// the sampler: variables are walked and every one that is still undetermined is
// decided, the rest falling out by propagation. What remains is a dependency
// set - for a Tseitin encoded circuit exactly its inputs - and letting the bit
// parallel generator assign the whole set at once saves it one full propagation
// sweep per input.
//
// The order that pass walks in decides what it finds. Index order alone adopts
// whichever variable of a dependency it meets first, so a gate output numbered
// ahead of its own inputs is taken for an input and the real ones then fall out
// of it by propagation - a driving set the same size but not the circuit's.
// When the gate network was recovered its free variables are the answer by
// construction, so they go first and the index scan only fills in whatever the
// residual clauses leave undetermined.
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

    auto adopt = [&](Var v) {
        if (probe.assigned(v)) return;
        inputVars_.push_back(v);
        const size_t m = probe.mark();
        const bool pick = rng_.coin();
        if (probe.enqueue(mkLit(v, pick)) && probe.propagate()) return;
        probe.undoTo(m);
        if (probe.enqueue(mkLit(v, !pick)) && probe.propagate()) return;
        // Both polarities fail here; the lane will be retried by the generator.
        probe.undoTo(m);
        probe.enqueue(mkLit(v, pick));
        probe.propagate();
    };

    if (gateNet_.complete()) {
        for (Var v : gateNet_.freeVars) adopt(v);
    }
    for (Var v = 0; v < sampleCnf_.numVars; ++v) adopt(v);
}

int Solver::workerThreads() const {
    int n = opt_.threads > 0 ? opt_.threads
                             : static_cast<int>(std::thread::hardware_concurrency());
    if (n <= 0) n = 1;
    return n;
}

PrepareResult Solver::prepareBase(std::string& error) {
    if (cnf_.hasEmptyClause) {
        error = "the formula contains an empty clause";
        return PrepareResult::Unsat;
    }
    master_.attach(searchCnf());

    targetLits_.clear();
    sampleFixed_.clear();
    if (opt_.outputsGiven) {
        for (int d : opt_.outputs) {
            const int64_t av = d < 0 ? -static_cast<int64_t>(d) : d;
            if (av < 1 || av > cnf_.numVars) {
                error = "output literal out of range: " + std::to_string(d) +
                        " (the formula has " + std::to_string(cnf_.numVars) + " variables)";
                return PrepareResult::InvalidInput;
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
            return PrepareResult::Unsat;
        }
    }
    for (Lit l : targetLits_) {
        if (!master_.enqueue(l)) {
            error = "the requested output valuation contradicts the formula";
            return PrepareResult::Unsat;
        }
    }
    // Variables that occur in no clause at all. DIMACS files that declare fewer
    // variables than their largest index leave gaps like this, and the loader
    // widens numVars to whatever it sees, so the gaps become variables. Any
    // value satisfies the formula, but the solver still has to assign every one
    // of them before it can call the instance done - and left to the probe loop
    // that happens one statistical accident at a time. Pin them here instead:
    // they land in the base trail, so a restart keeps them.
    stats_.unusedVars = 0;
    // Pin unused variables only after the requested outputs have been applied.
    // An absent variable is free in the CNF, but may still have an explicit target.
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
            if (!master_.assigned(v)) master_.enqueue(mkLit(v, negate));
            ++stats_.unusedVars;
        }
    }

    if (!master_.propagate()) {
        error = "propagating the unit clauses and the output valuation conflicts";
        return PrepareResult::Unsat;
    }
    return PrepareResult::Ok;
}

// Linear reasoning over the parity constraints, run once against the base
// assignment before anything else starts.
//
// Two kinds of thing come out, and they are used differently. A row that
// reduces to a single variable is a proven literal and goes straight onto the
// trail. A row that reduces to two is an equivalence, and those are folded into
// the formula the search runs on as a pair of binary clauses each - which is
// the point of doing this at all. They are consequences of the original
// clauses, so a model of the augmented formula is a model of the file, and
// `verify()` still checks against the file either way.
//
// The sample population is deliberately left reading `cnf_`: the derived
// clauses are not part of any gate, so adding them to `sampleCnf_` would leave
// residual clauses behind and cost the fast sampler for nothing.
PrepareResult Solver::prepareGf2(std::string& error) {
    if (!opt_.gf2) return PrepareResult::Ok;

    gf2_.build(cnf_);
    stats_.gf2Equations = gf2_.equationCount();
    stats_.gf2Vars = gf2_.varCount();
    if (gf2_.empty()) return PrepareResult::Ok;

    const uint64_t t0 = nowNs();
    ++stats_.gf2Runs;
    gf2_.solve(master_.values(), gf2Res_);
    stats_.gf2Seconds += static_cast<double>(nowNs() - t0) * 1e-9;
    gf2LastAssigned_ = master_.assignedCount();

    if (gf2Res_.conflict) {
        ++stats_.gf2Conflicts;
        error = "the parity constraints contradict the unit clauses";
        return PrepareResult::Unsat;
    }

    stats_.gf2Equivs = gf2Res_.equivA.size();
    if (!gf2Res_.equivA.empty()) {
        // `a` and `b` stand or fall together: a -> b and b -> a, one binary
        // clause each.
        //
        // Most of them are not worth writing down. On a Tseitin encoded round
        // function the parities are already cut into three variable gates whose
        // intermediates propagation can reach anyway, so the overwhelming
        // majority of what elimination derives is something unit propagation
        // would have produced on its own - 712 of 715 on a 17 round SHA-256
        // preimage. Adding those costs every probe a bigger formula to
        // propagate over and buys nothing, so each clause is checked against
        // propagation first and only the ones it cannot reach are kept.
        Propagator probe;
        probe.attach(cnf_);
        for (Lit l : master_.trail()) probe.enqueue(l);
        probe.propagate();
        const size_t probeBase = probe.mark();
        // A clause (x | y) is redundant when propagating ~x already yields y,
        // or propagating ~y already yields x. Propagation is not closed under
        // contraposition, so both directions have to be tried.
        auto reachable = [&](Lit x, Lit y) {
            bool got = true;  // a refuted premise makes the clause vacuous here
            if (probe.enqueue(litNeg(x)) && probe.propagate()) got = probe.litValue(y) == 1;
            probe.undoTo(probeBase);
            if (got) return true;
            got = true;
            if (probe.enqueue(litNeg(y)) && probe.propagate()) got = probe.litValue(x) == 1;
            probe.undoTo(probeBase);
            return got;
        };

        std::vector<Lit> keep;
        for (size_t i = 0; i < gf2Res_.equivA.size(); ++i) {
            const Lit a = gf2Res_.equivA[i], b = gf2Res_.equivB[i];
            if (!reachable(litNeg(a), b)) { keep.push_back(litNeg(a)); keep.push_back(b); }
            if (!reachable(a, litNeg(b))) { keep.push_back(a); keep.push_back(litNeg(b)); }
        }
        stats_.gf2Clauses = keep.size() / 2;

        if (!keep.empty()) {
            augmented_ = cnf_;
            for (size_t i = 0; i < keep.size(); i += 2) {
                augmented_.lits.push_back(keep[i]);
                augmented_.lits.push_back(keep[i + 1]);
                augmented_.start.push_back(static_cast<uint32_t>(augmented_.lits.size()));
            }
            if (augmented_.maxClauseLen < 2) augmented_.maxClauseLen = 2;
            augmented_.buildOccurrences();

            // Re-attach and replay. The trail is already the propagation
            // closure of everything pinned so far, so pushing it back literal
            // by literal restores exactly the same state - and then propagates
            // further, since the formula now says more.
            const std::vector<Lit> saved = master_.trail();
            search_ = &augmented_;
            master_.attach(searchCnf());
            for (Lit l : saved) {
                if (!master_.enqueue(l)) {
                    error = "the derived parity clauses contradict the unit clauses";
                    return PrepareResult::Unsat;
                }
            }
            if (!master_.propagate()) {
                error = "propagating the derived parity clauses conflicts";
                return PrepareResult::Unsat;
            }
        }
    }

    for (Lit l : gf2Res_.units) {
        if (master_.litValue(l) == 1) continue;
        ++stats_.gf2Units;
        if (!master_.enqueue(l)) {
            error = "the parity constraints contradict the unit clauses";
            return PrepareResult::Unsat;
        }
    }
    if (!master_.propagate()) {
        error = "propagating the proven parity literals conflicts";
        return PrepareResult::Unsat;
    }
    return PrepareResult::Ok;
}

// One elimination pass over the assignment as it now stands.
//
// Every literal it returns follows from the formula and that assignment, the
// same standing as anything the CDCL phase hands back - so this contributes no
// bets. It is cheap enough (single-digit milliseconds even on the SHA-256
// instances) to run whenever the assignment has moved a little.
bool Solver::runGf2(SolveStatus& status, bool& progress) {
    progress = false;
    if (!opt_.gf2 || gf2_.empty()) return true;

    const uint64_t t0 = nowNs();
    ++stats_.gf2Runs;
    gf2_.solve(master_.values(), gf2Res_);
    stats_.gf2Seconds += static_cast<double>(nowNs() - t0) * 1e-9;
    gf2LastAssigned_ = master_.assignedCount();

    if (gf2Res_.conflict) {
        ++stats_.gf2Conflicts;
        status = SolveStatus::Exhausted;
        return false;
    }

    gf2New_.clear();
    for (Lit l : gf2Res_.units) {
        if (master_.litValue(l) != 1) gf2New_.push_back(l);
    }
    if (gf2New_.empty()) return true;

    stats_.gf2Units += gf2New_.size();
    bool conflict = false;
    progress = applyLiterals(gf2New_, conflict);
    if (conflict) {
        status = SolveStatus::Exhausted;
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
    cfg.threads = workerThreads();
    cfg.maxRounds = opt_.sampleRounds;
    cfg.gates = &gateNet_;
    // The bits of the target valuation the population has to reproduce, taken
    // as a prefix of it: the target literals come in file order, so a prefix is
    // a stable, reproducible choice rather than a fresh subset per redraw, and
    // the population then keeps the same shape across the whole run.
    //
    // Nothing here is inference. Constraining the samples does not commit the
    // solver to anything - it narrows the population the statistical layer bets
    // from, and every bet is still checked the way it always was.
    {
        const size_t n = std::min(static_cast<size_t>(std::max(0, opt_.focusBits)),
                                  targetLits_.size());
        cfg.focusLits.assign(targetLits_.begin(), targetLits_.begin() + static_cast<long>(n));
        stats_.focusBits = n;
    }
    cfg.focusProgress = [this](uint64_t ok, uint64_t total, uint64_t rounds) {
        stats_.focusLanes = ok;
        stats_.totalSamples = total;
        stats_.focusRounds = rounds;
        tick("focusing");
    };
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
    stats_.focusLanes = sig_.focusLanes();
    stats_.focusRounds = sig_.focusRounds();
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
    if (++w.stampCounter == 0) {
        std::fill(w.stamp.begin(), w.stamp.end(), 0);
        ++w.stampCounter;
    }
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
    if (sig_.validSamples() <= static_cast<uint64_t>(opt_.mink)) {
        ++w.sigBails;
        out.assign(forced.begin(), forced.end());
        return;
    }
    const int words = sig_.words();
    const uint64_t* valid = sig_.validMask();

    w.keep.resize(static_cast<size_t>(words));
    std::memcpy(w.keep.data(), valid, static_cast<size_t>(words) * sizeof(uint64_t));

    for (Lit l : forced) {
        const uint64_t* s = sig_.var(l >> 1);
        uint64_t any = 0;
        if (l & 1) {
            for (int i = 0; i < words; ++i)
                any |= (w.keep[static_cast<size_t>(i)] &= ~s[i]);
        } else {
            for (int i = 0; i < words; ++i)
                any |= (w.keep[static_cast<size_t>(i)] &= s[i]);
        }
        // Filtering is monotone. In the direct pass thousands of assigned
        // literals can remain after the first few already emptied the mask.
        if (!any) {
            ++w.sigBails;
            out.assign(forced.begin(), forced.end());
            return;
        }
    }

    w.keepIdx.clear();
    w.keepVal.clear();
    uint64_t survivingSamples = 0;  // lanes, i.e. samples - never lane words
    for (int i = 0; i < words; ++i) {
        const uint64_t k = w.keep[static_cast<size_t>(i)];
        if (k) {
            w.keepIdx.push_back(static_cast<uint32_t>(i));
            w.keepVal.push_back(k);
            survivingSamples += popcount64(k);
        }
    }

    // Too few surviving samples: any verdict would be noise, so fall back to
    // what plain propagation already knows.
    //
    // `mink` is in SAMPLES. That is the only reading that makes the threshold
    // mean anything, and it is why the default is 640 rather than 10: a
    // variable that is constant across n samples is constant by chance with
    // probability 2^-(n-1), so with 24k variables to test, n = 11 hands back
    // around two dozen invented implications per probe while n = 640 hands
    // back none. Counting lane words instead would put the same number on a
    // wildly different amount of evidence depending on --siglen.
    if (survivingSamples <= static_cast<uint64_t>(opt_.mink)) {
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
    // No filter can increase the population. Avoid repeating both BCP branches
    // and scanning the table when a statistical verdict is impossible.
    if (sig_.validSamples() <= static_cast<uint64_t>(opt_.mink))
        return !w.result.empty();

    // A random window of the current assignment, taken in variable order so the
    // forced literals stay topologically close to each other. It is drawn once
    // per probe: both branches have to filter the sample population the same
    // way, otherwise their outcomes are not comparable and the intersection
    // below is meaningless.
    w.forcedBase.clear();
    if (!sortedInit_.empty() && effectiveInitk_ > 0) {
        const size_t n = sortedInit_.size();
        const size_t startAt = w.rng.below(static_cast<uint32_t>(n));
        const size_t take = std::min<size_t>(static_cast<size_t>(effectiveInitk_), n - startAt);
        for (size_t i = 0; i < take; ++i) {
            w.forcedBase.push_back(sortedInit_[startAt + i]);
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
        // The branch assumptions remain necessary even if the sample scan
        // returned no literals (or excluded an already assigned variable).
        for (Lit l : combo) {
            if (!w.prop.enqueue(l)) { ok = false; break; }
        }
        for (Lit l : w.sigOut) {
            if (!ok) break;
            if (!w.prop.enqueue(l)) { ok = false; break; }
        }
        if (ok) ok = w.prop.propagate();
        if (!ok) {
            w.prop.undoTo(w.base);
            // A statistical conflict does not refute this branch. Keep its
            // plain propagation closure in the intersection instead of silently
            // turning the other branches' guesses into unconditional literals.
            for (Lit l : combo) w.prop.enqueue(l);
            w.prop.propagate();  // this combination survived stage one
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
        if (!cdcl_->attach(searchCnf(), rng_.next())) {
            status = SolveStatus::Exhausted;
            return false;
        }
        cdclFedFromMaster_ = 0;
        cdclRootSeen_ = cdcl_->rootTrail().size();
        cdclBudget_ = opt_.cdclConflicts;
    }

    // Pin everything the signature loop has committed since the last phase.
    //
    // addRoot() propagates, and the learned clauses carried over from earlier
    // phases can turn a freshly pinned literal into further level 0 units. Those
    // are proven the same way anything run() returns is, so they are read off
    // the root trail here and applied - and applying them can extend master_'s
    // trail again, which is why this loops until neither side has anything new.
    for (;;) {
        const std::vector<Lit>& mt = master_.trail();
        if (cdclFedFromMaster_ >= mt.size()) break;
        for (; cdclFedFromMaster_ < mt.size(); ++cdclFedFromMaster_) {
            if (!cdcl_->addRoot(mt[cdclFedFromMaster_])) {
                status = SolveStatus::Exhausted;  // the assignment is refuted
                return false;
            }
        }
        // Most of the root trail delta is the literals just fed in; what is
        // left is what the learned clauses derived from them.
        cdclNew_.clear();
        const std::vector<Lit>& rt = cdcl_->rootTrail();
        for (size_t i = cdclRootSeen_; i < rt.size(); ++i) {
            if (master_.litValue(rt[i]) != 1) cdclNew_.push_back(rt[i]);
        }
        cdclRootSeen_ = rt.size();
        if (cdclNew_.empty()) break;
        stats_.cdclImplied += cdclNew_.size();
        bool conflict = false;
        if (applyLiterals(cdclNew_, conflict)) progress = true;
        if (conflict) {
            status = SolveStatus::Exhausted;
            return false;
        }
        if (oracleTripped_) return true;
    }

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
            // No oracle check here on purpose. This model satisfies the whole
            // formula, so the instance is solved even when it is a different
            // solution from the one a tuning run was handed - the oracle exists
            // to cut short a run that has wandered off, not to reject an answer.
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
    // A restart retracts the trail, so the last pass's cursor means nothing.
    gf2LastAssigned_ = 0;

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

        // The probes have moved the assignment; the parity system may now say
        // something it could not before. Each pass costs milliseconds, so it is
        // run on a stride rather than every round.
        if (opt_.gf2 && !gf2_.empty() &&
            master_.assignedCount() >=
                gf2LastAssigned_ + static_cast<size_t>(std::max(1, opt_.gf2Interval))) {
            bool gf2Progress = false;
            if (!runGf2(status, gf2Progress)) return false;
            if (gf2Progress) grew = true;
        }

        if (grew) {
            stall = 0;
            resamplesSinceProgress = 0;
        } else if (++stall > stallLimit) {
            stall = 0;
            // A plateau is the one moment worth spending an elimination pass on
            // unconditionally: it is the cheapest thing that can still prove
            // something, and it comes before both the redraw and the search.
            {
                bool gf2Progress = false;
                if (!runGf2(status, gf2Progress)) return false;
                if (gf2Progress) {
                    resamplesSinceProgress = 0;
                    tick("solving");
                    continue;
                }
            }
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
                // Not something to shrug off: a failed redraw can leave the
                // tables sized for a population that was never built, and the
                // probes would then read past the end of them.
                if (!buildSignatures(errorMessage_)) {
                    status = SolveStatus::Error;
                    return false;
                }
                for (auto& w : workers_) w->keep.resize(static_cast<size_t>(sig_.words()));
                continue;
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
    errorMessage_.clear();
    // A complete root model needs neither parity extraction nor samples. Check
    // again after GF(2), which can finish the assignment too.
    auto rootSolved = [&] {
        if (master_.assignedCount() != static_cast<size_t>(cnf_.numVars) || !verify())
            return false;
        res.status = SolveStatus::Solved;
        res.assignment = master_.values();
        stats_.assignedVars = master_.assignedCount();
        stats_.solveSeconds = static_cast<double>(nowNs() - startNs_) * 1e-9;
        res.stats = stats_;
        return true;
    };

    if (!opt_.seedGiven) {
        rng_.reseed(nowNs() ^ 0xA5A5A5A5DEADBEEFull);
    }

    std::string error;
    std::vector<Lit> units;
    unitLits_.clear();
    for (size_t c = 0; c < cnf_.clauseCount(); ++c) {
        if (cnf_.clauseLen(c) == 1) unitLits_.push_back(cnf_.clauseBegin(c)[0]);
    }

    switch (prepareBase(error)) {
        case PrepareResult::Ok:
            break;
        case PrepareResult::Unsat:
            res.status = SolveStatus::Unsatisfiable;
            res.message = error;
            return res;
        case PrepareResult::InvalidInput:
            res.status = SolveStatus::Error;
            res.message = error;
            return res;
    }
    if (rootSolved()) return res;
    // Before anything reads the formula: recover the parity constraints and
    // reduce them. This can widen the base assignment and can add clauses to
    // what the search runs on, so it has to happen before master_'s trail is
    // taken as the base and before the workers attach.
    switch (prepareGf2(error)) {
        case PrepareResult::Ok:
            break;
        case PrepareResult::Unsat:
            res.status = SolveStatus::Unsatisfiable;
            res.message = error;
            return res;
        case PrepareResult::InvalidInput:
            res.status = SolveStatus::Error;
            res.message = error;
            return res;
    }

    if (rootSolved()) return res;
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

    pool_.reset(new ThreadPool(workerThreads()));
    workers_.clear();
    workers_.reserve(static_cast<size_t>(pool_->size()));
    for (int i = 0; i < pool_->size(); ++i) {
        std::unique_ptr<Worker> w(new Worker());
        w->prop.attach(searchCnf());
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
        // one parameter with no safe default: a narrow filter leaves too many
        // samples for anything to look constant, a wide one leaves so few
        // that merely biased variables pass for implied ones and poison the
        // assignment. Each restart halves it, so the attempts double as a
        // search over that trade-off, from aggressive to conservative.
        effectiveInitk_ = std::max(1, opt_.initk >> std::min(a - 1, 30));

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
            status == SolveStatus::OracleMismatch || status == SolveStatus::Error) {
            break;  // tuning: a contradicted literal ends the trial, not just the attempt
        }
        ++stats_.restarts;
    }

    stats_.solveSeconds = static_cast<double>(nowNs() - solveStart) * 1e-9;
    stats_.assignedVars = master_.assignedCount();
    res.status = status;
    if (res.message.empty()) res.message = errorMessage_;
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
