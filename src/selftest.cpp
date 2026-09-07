// Property tests for the parts of the solver that have a checkable contract.
//
// The benchmark is the real test suite, but it only ever checks the one thing a
// solved instance proves: that the model satisfies the formula. Everything the
// CDCL phase promises the round loop - that a literal on its level 0 trail is
// implied by the formula and the pinned roots, that a root conflict means the
// roots really are refuted, that the search is back at level 0 whatever run()
// returned - is invisible to it, and a regression there costs the solver
// inference without ever producing a wrong answer.
//
// So: small random formulas, few enough variables to enumerate every model by
// brute force, and each claim checked against that enumeration.
#include "selftest.h"

#include <cstdio>
#include <functional>
#include <vector>

#include "cdcl.h"
#include "cnf.h"
#include "gates.h"
#include "gf2.h"
#include "propagator.h"
#include "rng.h"
#include "signatures.h"
#include "solver_worker.h"

namespace tcs {

// Exercise the actual probe with a population valid for the relaxed formula.
// A sample-derived conflict must reject the branch even when BCP alone accepts it.
struct SolverTestAccess {
    static bool checkUnitOnlyTarget() {
        Cnf cnf;
        cnf.numVars = 4;
        cnf.lits = {mkLit(0, true), mkLit(1, false), mkLit(2, false)};
        cnf.start = {0, 1, 3};
        cnf.buildOccurrences();
        Options opt;
        Solver solver(cnf, opt);
        solver.unitLits_ = {mkLit(0, true)};
        std::string error;
        if (solver.prepareBase(error) != PrepareResult::Ok) return false;
        solver.buildSampleCnf();
        solver.collectSortedInit();
        if (solver.sortedInit_ != solver.unitLits_) return false;
        SignatureConfig cfg;
        cfg.words = 2;
        if (!solver.sig_.generate(solver.sampleCnf_, solver.sampleFixed_, {}, cfg, error)) return false;
        return solver.sig_.validSamples() == 128 && solver.sig_.var(0)[0] == 0 &&
               solver.sig_.var(0)[1] == 0;
    }

    static bool checkNumbering(const Cnf& cnf, uint64_t seed, bool gates) {
        Cnf sparse = cnf;
        sparse.numVars = 3 * cnf.numVars + 5;
        auto map = [](Lit l) { return mkLit(3 * litVar(l) + 2, litSign(l)); };
        for (Lit& l : sparse.lits) l = map(l);
        sparse.buildOccurrences();
        Options opt;
        opt.mink = 3;
        opt.minkWords = (seed & 1) != 0;
        opt.probeDescending = (seed & 2) != 0;
        opt.probeVars = 3;
        Solver denseSolver(cnf, opt), sparseSolver(sparse, opt);
        std::string error;
        if (denseSolver.prepareBase(error) != PrepareResult::Ok ||
            sparseSolver.prepareBase(error) != PrepareResult::Ok) return false;
        GateNetwork denseNet, sparseNet;
        extractGates(cnf, denseNet);
        extractGates(sparse, sparseNet);
        SignatureConfig cfg;
        cfg.words = 12;
        cfg.threads = 1 + static_cast<int>(seed & 1);
        cfg.seed = seed;
        // The last gate is reachable and can take at least one polarity. Use a
        // preliminary population to choose one, without reading a solution.
        Signatures base;
        cfg.gates = gates ? &denseNet : nullptr;
        if (!base.generate(cnf, {}, {}, cfg, error)) return false;
        const Lit target = mkLit(cnf.numVars - 1, !(base.var(cnf.numVars - 1)[0] & 1));
        if (gates) cfg.focusLits = {target};
        if (!denseSolver.sig_.generate(cnf, {}, {}, cfg, error)) return false;
        cfg.gates = gates ? &sparseNet : nullptr;
        if (gates) cfg.focusLits = {map(target)};
        if (!sparseSolver.sig_.generate(sparse, {}, {}, cfg, error)) return false;
        for (int w = 0; w < cfg.words; ++w) {
            if (denseSolver.sig_.validMask()[w] != sparseSolver.sig_.validMask()[w]) return false;
            for (Var v : denseSolver.searchVars_) {
                if (denseSolver.sig_.var(v)[w] != sparseSolver.sig_.var(3 * v + 2)[w])
                    return false;
            }
        }
        for (Solver* s : {&denseSolver, &sparseSolver}) {
            const Lit l = s == &denseSolver ? target : map(target);
            if (!s->master_.enqueue(l) || !s->master_.propagate()) return false;
            s->effectiveInitk_ = 3;
            s->collectSortedInit();
        }
        std::vector<Lit> mapped;
        for (Lit l : denseSolver.sortedInit_) mapped.push_back(map(l));
        if (mapped != sparseSolver.sortedInit_) return false;
        for (int probe = 0; probe < 12; ++probe) {
            Solver::Worker a, b;
            const std::pair<Solver::Worker*, Solver*> jobs[] = {{&a, &denseSolver}, {&b, &sparseSolver}};
            for (auto p : jobs) {
                p.first->prop.attach(p.second->cnf_);
                p.first->rng.reseed(seed + probe);
                p.first->stamp.assign(p.second->cnf_.numVars, 0);
                p.first->stampPol.assign(p.second->cnf_.numVars, 0);
                p.second->runProbe(*p.first);
            }
            if (a.hardConflict != b.hardConflict) return false;
            mapped.clear();
            for (Lit l : a.result) mapped.push_back(map(l));
            if (mapped != b.result) return false;
            if (a.probeVars.size() != b.probeVars.size()) return false;
            for (size_t i = 0; i < a.probeVars.size(); ++i)
                if (3 * a.probeVars[i] + 2 != b.probeVars[i]) return false;
            // Compare the optimized scan with the literal definition of
            // SigOutcome, including both threshold units and all lane words.
            std::vector<uint64_t> keep(cfg.words);
            for (int w = 0; w < cfg.words; ++w) keep[w] = denseSolver.sig_.validMask()[w];
            for (Lit l : a.forced) {
                for (int w = 0; w < cfg.words; ++w) {
                    const uint64_t bits = denseSolver.sig_.var(litVar(l))[w];
                    keep[w] &= litSign(l) ? ~bits : bits;
                }
            }
            uint64_t evidence = 0;
            for (uint64_t bits : keep) evidence += opt.minkWords ? (bits != 0) : popcount64(bits);
            std::vector<Lit> expected, actual;
            if (evidence <= static_cast<uint64_t>(opt.mink)) expected = a.forced;
            else for (Var v : denseSolver.searchVars_) {
                if (a.prop.assigned(v)) continue;
                bool allTrue = true, allFalse = true;
                for (int w = 0; w < cfg.words; ++w) {
                    const uint64_t bits = denseSolver.sig_.var(v)[w] & keep[w];
                    allTrue = allTrue && bits == keep[w];
                    allFalse = allFalse && bits == 0;
                }
                if (allTrue || allFalse) expected.push_back(mkLit(v, allFalse));
            }
            denseSolver.signatureOutcome(a, a.forced, actual);
            if (expected != actual) return false;
        }
        return true;
    }

    static bool check() {
        Cnf cnf;
        cnf.numVars = 3;
        cnf.lits = {mkLit(0, false), mkLit(1, true), mkLit(2, true)};
        cnf.start = {0, 3};
        cnf.buildOccurrences();
        Options opt;
        opt.mink = 0;
        Solver solver(cnf, opt);
        solver.master_.attach(cnf);
        solver.master_.enqueue(mkLit(2, false));
        solver.master_.propagate();
        SignatureConfig cfg;
        cfg.words = 1;
        std::string error;
        if (!solver.sig_.generate(cnf, {mkLit(0, true), mkLit(1, false), mkLit(2, true)},
                                  {}, cfg, error)) return false;
        // Try both branch visitation orders, always probing x.
        int checked = 0;
        for (uint64_t seed = 0; seed < 100; ++seed) {
            Rng rng(seed);
            if (rng.below(3) != 0) continue;
            Solver::Worker w;
            w.prop.attach(cnf);
            w.rng.reseed(seed);
            w.stamp.assign(3, 0);
            w.stampPol.assign(3, 0);
            solver.runProbe(w);
            // The samples reject NOT x, so the remaining branch commits x.
            // This checks the intended statistical contract, not logical entailment.
            if (w.hardConflict || w.result != std::vector<Lit>{mkLit(0, false)}) return false;
            // A wrapped stamp must not match an entry from an ancient pass.
            w.stampCounter = UINT32_MAX;
            w.stamp.assign(3, 1);
            std::vector<Lit> lhs = {mkLit(0, false)};
            Solver::intersectLits(w, lhs, {mkLit(1, false)});
            if (!lhs.empty()) return false;
            ++checked;
        }
        return checked > 0;
    }
};

namespace {

struct Failure {
    const char* what = nullptr;
};

bool checkRegressions(Failure& fail) {
    if (!SolverTestAccess::checkUnitOnlyTarget()) {
        fail.what = "unit-only target was treated as an unused numbering gap";
        return false;
    }
    if (!SolverTestAccess::check()) {
        fail.what = "statistical branch conflict or intersection stamp regression";
        return false;
    }
    Cnf cnf;
    cnf.numVars = 2;
    cnf.lits = {mkLit(0, false)};
    cnf.start = {0, 1};
    cnf.buildOccurrences();
    Options opt;
    opt.outputsGiven = true;
    opt.outputs = {-2};
    opt.seedGiven = true;
    Solver solver(cnf, opt);
    const SolveResult result = solver.solve();
    if (result.status != SolveStatus::Solved || result.assignment[1] != -1 ||
        result.stats.signatureBytes || result.stats.sampleSeconds || result.stats.gf2Runs) {
        fail.what = "root model must honour unused outputs and bypass preprocessing";
        return false;
    }
    opt.outputs = {INT32_MIN};
    Solver invalid(cnf, opt);
    if (invalid.solve().status != SolveStatus::Error) {
        fail.what = "minimum signed output literal must be rejected without overflow";
        return false;
    }
    SignatureConfig cfg;
    cfg.words = 2;
    Signatures sig;
    std::string error;
    if (!sig.generate(cnf, {}, {}, cfg, error) || sig.validSamples() != 128 ||
        sig.var(0)[0] != UINT64_MAX || sig.var(0)[1] != UINT64_MAX) {
        fail.what = "sampler ignored a unit clause";
        return false;
    }
    if (sig.generate(cnf, {mkLit(0, true)}, {}, cfg, error)) {
        fail.what = "sampler accepted contradictory fixed literals";
        return false;
    }
    cfg.focusLits = {mkLit(1, false), mkLit(1, true)};
    if (sig.generate(cnf, {}, {}, cfg, error)) {
        fail.what = "sampler accepted contradictory focus literals";
        return false;
    }
    cfg.focusLits.clear();
    cnf.start.push_back(1);
    cnf.buildOccurrences();
    if (!sig.generate(cnf, {}, {}, cfg, error) || sig.validSamples() != 0) {
        fail.what = "sampler accepted lanes despite an empty clause";
        return false;
    }
    return true;
}

// A random 1..3 literal CNF. Short clauses and few variables keep the models
// dense enough that most instances are satisfiable and most root sets survive,
// so the interesting paths are actually reached instead of every case ending in
// an immediate root conflict.
Cnf randomCnf(Rng& rng, int numVars, int numClauses) {
    Cnf cnf;
    cnf.numVars = numVars;
    cnf.start.push_back(0);
    std::vector<Var> used;
    for (int c = 0; c < numClauses; ++c) {
        const int len = 1 + static_cast<int>(rng.below(3));
        used.clear();
        for (int k = 0; k < len; ++k) {
            const Var v = static_cast<Var>(rng.below(static_cast<uint32_t>(numVars)));
            bool dup = false;
            for (Var u : used) dup = dup || u == v;
            if (dup) continue;
            used.push_back(v);
            cnf.lits.push_back(mkLit(v, rng.coin()));
        }
        // Every literal collided: drop the clause rather than emit an empty one,
        // which would make the instance trivially unsatisfiable.
        if (cnf.lits.size() == cnf.start.back()) continue;
        cnf.start.push_back(static_cast<uint32_t>(cnf.lits.size()));
    }
    cnf.buildOccurrences();
    return cnf;
}

bool checkSampling(uint64_t seed, Failure& fail) {
    Rng rng(seed);
    const Cnf cnf = randomCnf(rng, 8, 16);
    SignatureConfig cfg;
    cfg.words = 2;
    cfg.seed = seed;
    Signatures sig;
    std::string error;
    if (!sig.generate(cnf, {}, {}, cfg, error)) {
        // Conflicting units are an explicitly rejected sampling request.
        return error == "contradictory fixed or focused sample literals";
    }
    for (int w = 0; w < sig.words(); ++w) {
        for (size_t c = 0; c < cnf.clauseCount(); ++c) {
            uint64_t sat = 0;
            for (uint32_t i = 0; i < cnf.clauseLen(c); ++i) {
                const Lit l = cnf.clauseBegin(c)[i];
                const uint64_t val = sig.var(litVar(l))[w];
                sat |= litSign(l) ? ~val : val;
            }
            if (sig.validMask()[w] & ~sat) {
                fail.what = "propagated sample falsifies a clause";
                return false;
            }
        }
    }
    return true;
}

// Every satisfying assignment, one bit per variable. numVars stays under 16, so
// the enumeration is a few thousand steps.
std::vector<uint32_t> allModels(const Cnf& cnf) {
    std::vector<uint32_t> models;
    const int n = cnf.numVars;
    const size_t nc = cnf.clauseCount();
    for (uint32_t m = 0; m < (1u << n); ++m) {
        bool ok = true;
        for (size_t c = 0; c < nc && ok; ++c) {
            const uint32_t len = cnf.clauseLen(c);
            const Lit* b = cnf.clauseBegin(c);
            bool sat = false;
            for (uint32_t k = 0; k < len; ++k) {
                const bool val = ((m >> litVar(b[k])) & 1u) != 0;
                if (val != litSign(b[k])) { sat = true; break; }
            }
            ok = sat;
        }
        if (ok) models.push_back(m);
    }
    return models;
}

bool modelHonours(uint32_t m, const std::vector<Lit>& lits) {
    for (Lit l : lits) {
        const bool val = ((m >> litVar(l)) & 1u) != 0;
        if (val == litSign(l)) return false;
    }
    return true;
}

// One formula, one set of roots, every claim the Cdcl contract makes.
bool checkCdcl(uint64_t seed, Failure& fail) {
    Rng rng(seed);
    const int numVars = 4 + static_cast<int>(rng.below(9));  // 4..12
    const int numClauses =
        numVars + static_cast<int>(rng.below(static_cast<uint32_t>(4 * numVars)));
    const Cnf cnf = randomCnf(rng, numVars, numClauses);

    std::vector<Lit> roots;
    const int numRoots = static_cast<int>(rng.below(4));
    for (int i = 0; i < numRoots; ++i) {
        roots.push_back(
            mkLit(static_cast<Var>(rng.below(static_cast<uint32_t>(numVars))), rng.coin()));
    }

    // The models that survive the roots. This is the oracle for everything
    // below: an empty set is the exact meaning of RootConflict, and a literal
    // true in every survivor is the exact meaning of an implied unit.
    std::vector<uint32_t> survivors;
    for (uint32_t m : allModels(cnf)) {
        if (modelHonours(m, roots)) survivors.push_back(m);
    }

    // Vacuously true when the roots are refuted, which is exactly right: with no
    // models left, everything is implied and only the RootConflict check bites.
    auto impliedByAll = [&](Lit l) {
        for (uint32_t m : survivors) {
            const bool val = ((m >> litVar(l)) & 1u) != 0;
            if (val == litSign(l)) return false;
        }
        return true;
    };
    auto trailImplied = [&](const std::vector<Lit>& rt, size_t from) {
        for (size_t i = from; i < rt.size(); ++i) {
            if (!impliedByAll(rt[i])) return false;
        }
        return true;
    };

    Cdcl cdcl;
    bool refuted = !cdcl.attach(cnf, seed);
    // Anything attach() derived follows from the formula alone, so it holds
    // under the roots too.
    if (!refuted && !trailImplied(cdcl.rootTrail(), 0)) {
        fail.what = "attach() put a literal on the level 0 trail that is not implied";
        return false;
    }

    size_t seen = refuted ? 0 : cdcl.rootTrail().size();
    for (Lit r : roots) {
        if (refuted) break;
        if (!cdcl.addRoot(r)) { refuted = true; break; }
        // The delta addRoot() leaves behind is what the root synchronisation bug
        // used to discard: learned clauses from an earlier phase turning a newly
        // pinned literal into further units. Each one has to be a genuine
        // consequence of the formula and the roots pinned so far.
        if (!trailImplied(cdcl.rootTrail(), seen)) {
            fail.what = "addRoot() derived a literal that is not implied by the roots";
            return false;
        }
        seen = cdcl.rootTrail().size();
    }

    CdclResult r = CdclResult::Budget;
    const std::function<bool()> never;
    while (!refuted) {
        r = cdcl.run(0, never);
        // Whatever run() returned, what it leaves behind is the level 0 trail,
        // so every entry has to be implied. This is where the decisions leaked
        // out before the Solved path learned to cancel back to level 0.
        if (!trailImplied(cdcl.rootTrail(), 0)) {
            fail.what = "run() left a literal on the level 0 trail that is not implied";
            return false;
        }
        if (r != CdclResult::Implied) break;
    }

    if (refuted || r == CdclResult::RootConflict) {
        if (!survivors.empty()) {
            fail.what = "refuted a formula that has a model honouring the roots";
            return false;
        }
        return true;
    }
    if (r == CdclResult::Solved) {
        if (survivors.empty()) {
            fail.what = "solved a formula with no model honouring the roots";
            return false;
        }
        uint32_t m = 0;
        const std::vector<int8_t>& model = cdcl.model();
        for (Var v = 0; v < numVars; ++v) {
            if (model[static_cast<size_t>(v)] > 0) m |= (1u << v);
        }
        bool found = false;
        for (uint32_t s : survivors) found = found || s == m;
        if (!found) {
            fail.what = "the reported model does not satisfy the formula and the roots";
            return false;
        }
        return true;
    }
    // With no conflict budget and no cancel callback there is no other way out.
    fail.what = "an unbounded run() returned neither Solved nor RootConflict";
    return false;
}

// The counter based propagator under the probe loop. Its undo has to put the
// clause counters back exactly as they were, or the next speculative branch
// reads a state that never existed - and nothing downstream would notice.
bool checkPropagatorUndo(uint64_t seed, Failure& fail) {
    Rng rng(seed);
    const int numVars = 4 + static_cast<int>(rng.below(9));
    const Cnf cnf = randomCnf(rng, numVars, numVars * 3);

    Propagator p;
    p.attach(cnf);
    const uint32_t satBefore = p.satisfiedClauses();
    const size_t m = p.mark();
    for (int i = 0; i < 4; ++i) {
        const Lit l =
            mkLit(static_cast<Var>(rng.below(static_cast<uint32_t>(numVars))), rng.coin());
        if (!p.enqueue(l)) break;
        if (!p.propagate()) break;
    }
    p.undoTo(m);
    if (p.assignedCount() != m || p.satisfiedClauses() != satBefore || p.conflict()) {
        fail.what = "undoTo() did not restore the propagator state";
        return false;
    }
    for (Var v = 0; v < numVars; ++v) {
        if (p.assigned(v)) {
            fail.what = "undoTo() left a variable assigned";
            return false;
        }
    }
    return true;
}


// The GF(2) layer, checked the same way: a random linear system, its Tseitin
// encoding, and the full model set by brute force.
//
// Soundness is the easy half - every unit and every equivalence must hold in
// every model. The half worth having is completeness, because that is what
// says elimination is actually doing the work: if a variable takes the same
// value in every model of a system made only of parities, a reduced system has
// to say so as a one-variable row. A reduction that stops early passes the
// soundness check and fails this one.
bool checkGf2(uint64_t seed, Failure& fail) {
    Rng rng(seed);
    const int numVars = 4 + static_cast<int>(rng.below(9));  // 4..12
    const int numEqs = 1 + static_cast<int>(rng.below(static_cast<uint32_t>(2 * numVars)));

    // The system, and its CNF encoding side by side.
    std::vector<std::vector<Var>> eqs;
    std::vector<uint8_t> rhs;
    Cnf cnf;
    cnf.numVars = numVars;
    cnf.start.push_back(0);
    std::vector<Var> vs;
    for (int e = 0; e < numEqs; ++e) {
        const int len = 2 + static_cast<int>(rng.below(2));  // 2 or 3 variables
        vs.clear();
        for (int k = 0; k < len; ++k) {
            const Var v = static_cast<Var>(rng.below(static_cast<uint32_t>(numVars)));
            bool dup = false;
            for (Var u : vs) dup = dup || u == v;
            if (!dup) vs.push_back(v);
        }
        if (static_cast<int>(vs.size()) < 2) continue;
        const uint8_t r = static_cast<uint8_t>(rng.coin() ? 1 : 0);
        // The clauses forbidding every assignment of the wrong parity. A clause
        // is falsified by exactly one assignment - the one negating each of its
        // literals - so the clause whose sign pattern has parity p rules out the
        // assignment of parity p ^ (len & 1)... which is easier to just compute.
        const size_t n = vs.size();
        for (uint32_t mask = 0; mask < (1u << n); ++mask) {
            uint32_t par = 0;
            for (size_t k = 0; k < n; ++k) par ^= (mask >> k) & 1u;
            if (par == r) continue;  // this assignment is allowed
            // Forbid `mask`: the clause whose literals are all false under it.
            for (size_t k = 0; k < n; ++k) {
                cnf.lits.push_back(mkLit(vs[k], ((mask >> k) & 1u) != 0));
            }
            cnf.start.push_back(static_cast<uint32_t>(cnf.lits.size()));
        }
        eqs.push_back(vs);
        rhs.push_back(r);
    }
    if (eqs.empty()) return true;
    cnf.buildOccurrences();

    // A partial assignment to reduce against, drawn the way the solver's trail
    // would be: a few variables pinned, the rest open.
    std::vector<int8_t> values(static_cast<size_t>(numVars), 0);
    const int nfixed = static_cast<int>(rng.below(4));
    for (int i = 0; i < nfixed; ++i) {
        const Var v = static_cast<Var>(rng.below(static_cast<uint32_t>(numVars)));
        values[static_cast<size_t>(v)] = rng.coin() ? int8_t(1) : int8_t(-1);
    }

    // Every model of the system that honours the pinned values.
    std::vector<uint32_t> models;
    for (uint32_t m = 0; m < (1u << numVars); ++m) {
        bool ok = true;
        for (size_t e = 0; e < eqs.size() && ok; ++e) {
            uint32_t par = 0;
            for (Var v : eqs[e]) par ^= (m >> v) & 1u;
            ok = par == rhs[e];
        }
        for (Var v = 0; v < numVars && ok; ++v) {
            const int8_t want = values[static_cast<size_t>(v)];
            if (want != 0) ok = (((m >> v) & 1u) != 0) == (want > 0);
        }
        if (ok) models.push_back(m);
    }

    Gf2System sys;
    sys.build(cnf);
    if (sys.equationCount() != eqs.size()) {
        // Two equations over the same variables collapse into one group, and a
        // pair with the same variables but different parity makes the whole
        // group unrecoverable - both are fine, but then this instance says
        // nothing about the reduction, so skip it.
        return true;
    }
    Gf2Result res;
    sys.solve(values, res);

    if (res.conflict) {
        if (!models.empty()) {
            fail.what = "gf2 refuted a system that has a model";
            return false;
        }
        return true;
    }
    if (models.empty()) {
        fail.what = "gf2 missed that the system has no model";
        return false;
    }

    auto trueInAll = [&](Lit l) {
        for (uint32_t m : models) {
            if ((((m >> litVar(l)) & 1u) != 0) == litSign(l)) return false;
        }
        return true;
    };

    for (Lit l : res.units) {
        if (!trueInAll(l)) {
            fail.what = "gf2 proved a literal that is false in some model";
            return false;
        }
    }
    // An equivalence is the clause pair (~a | b) and (a | ~b): the two literals
    // are either both true or both false in every model.
    for (size_t i = 0; i < res.equivA.size(); ++i) {
        const Lit a = res.equivA[i], b = res.equivB[i];
        for (uint32_t m : models) {
            const bool va = (((m >> litVar(a)) & 1u) != 0) != litSign(a);
            const bool vb = (((m >> litVar(b)) & 1u) != 0) != litSign(b);
            if (va != vb) {
                fail.what = "gf2 reported an equivalence that a model breaks";
                return false;
            }
        }
    }

    // Completeness: a variable fixed across every model must come back as a
    // unit, unless the caller already knew it.
    for (Var v = 0; v < numVars; ++v) {
        if (values[static_cast<size_t>(v)] != 0) continue;
        const uint32_t bit = (models[0] >> v) & 1u;
        bool fixed = true;
        for (uint32_t m : models) fixed = fixed && ((m >> v) & 1u) == bit;
        if (!fixed) continue;
        bool found = false;
        for (Lit l : res.units) found = found || litVar(l) == v;
        if (!found) {
            fail.what = "gf2 left a variable unproven that every model agrees on";
            return false;
        }
    }
    return true;
}


// A random Tseitin encoded circuit: a few free inputs, then gates whose inputs
// are drawn from everything defined so far, emitted in exactly the shapes
// gates.cpp matches. Anything else would come back as a residual clause and the
// fast sampler - which is what the focus loop runs on - would never be reached.
Cnf randomCircuit(Rng& rng, int inputs, int gates) {
    Cnf cnf;
    cnf.numVars = inputs + gates;
    cnf.start.push_back(0);
    auto push = [&](std::vector<Lit> cl) {
        for (Lit l : cl) cnf.lits.push_back(l);
        if (cl.size() > cnf.maxClauseLen) cnf.maxClauseLen = static_cast<uint32_t>(cl.size());
        cnf.start.push_back(static_cast<uint32_t>(cnf.lits.size()));
    };
    for (int g = 0; g < gates; ++g) {
        const Var o = static_cast<Var>(inputs + g);
        const int defined = inputs + g;
        Var a = static_cast<Var>(rng.below(static_cast<uint32_t>(defined)));
        Var b = static_cast<Var>(rng.below(static_cast<uint32_t>(defined)));
        if (a == b) b = static_cast<Var>((a + 1) % defined);
        const Lit x = mkLit(a, rng.coin());
        const Lit y = mkLit(b, rng.coin());
        // The output literal carries the gate's own polarity, which is how NAND,
        // NOR and XNOR are these same two patterns.
        const Lit O = mkLit(o, rng.coin());
        if (rng.coin()) {
            // O == x & y
            push({static_cast<Lit>(O ^ 1), x});
            push({static_cast<Lit>(O ^ 1), y});
            push({O, static_cast<Lit>(x ^ 1), static_cast<Lit>(y ^ 1)});
        } else {
            // O == x ^ y: forbid every assignment of the three literals whose
            // parity is odd, one ternary clause each.
            const Lit trio[3] = {O, x, y};
            for (uint32_t m = 0; m < 8; ++m) {
                if ((((m >> 0) ^ (m >> 1) ^ (m >> 2)) & 1u) == 0) continue;
                std::vector<Lit> cl;
                for (int k = 0; k < 3; ++k) {
                    // Assignment bit set means the literal is true, so the
                    // clause that forbids it holds the negation.
                    cl.push_back(static_cast<Lit>(trio[k] ^ (((m >> k) & 1u) ? 1 : 0)));
                }
                push(cl);
            }
        }
    }
    cnf.buildOccurrences();
    return cnf;
}

// The --focus rejection loop, checked against a target that is reachable by
// construction: one lane of an ordinary population is read back and its values
// on a handful of variables become the bits the next population has to hit.
//
// Soundness is that every lane that comes back reproduces those bits and still
// satisfies every clause. The half worth having is again completeness: the
// target is a valuation the circuit really does produce, so a loop that keeps
// what it has landed must converge on *all* of the lanes. A loop that lost a
// landed lane to a later redraw, or that stopped short, would still hand back a
// population where every surviving lane matches - and only the lane count says
// so.
bool checkFocus(uint64_t seed, Failure& fail) {
    Rng rng(seed);
    const int inputs = 3 + static_cast<int>(rng.below(5));   // 3..7
    const int gates = 4 + static_cast<int>(rng.below(12));   // 4..15
    Cnf cnf = randomCircuit(rng, inputs, gates);

    GateNetwork net;
    extractGates(cnf, net);
    // A random circuit can still fool the matcher - two gates sharing a clause
    // shape, a cycle broken by freeing a variable - and then the fast path is
    // not taken at all. Nothing to test in that case.
    if (!net.complete()) return true;

    SignatureConfig cfg;
    cfg.words = 4;  // 256 lanes; enough for the loop to have a tail to converge
    cfg.seed = seed | 1u;
    cfg.threads = 1 + static_cast<int>(rng.below(2));
    cfg.gates = &net;

    std::string error;
    Signatures base;
    if (!base.generate(cnf, {}, net.freeVars, cfg, error)) {
        fail.what = "generating the reference population failed";
        return false;
    }
    if (!base.gateSampling() || base.validSamples() != base.sampleCount()) {
        // The network did not vouch for itself, so the focus loop would not run
        // on it either.
        return true;
    }

    // The target: what lane 0 of that population happens to hold on a few
    // variables. Reachable by construction, which is what lets the completeness
    // check below be an assertion rather than a hope.
    const int bits = 1 + static_cast<int>(rng.below(3));  // 1..3
    std::vector<Lit> focus;
    for (int i = 0; i < bits; ++i) {
        const Var v = static_cast<Var>(rng.below(static_cast<uint32_t>(cnf.numVars)));
        const bool isTrue = (base.var(v)[0] & 1ull) != 0;
        focus.push_back(mkLit(v, !isTrue));
    }
    cfg.focusLits = focus;
    cfg.seed = (seed * 2654435761u) | 1u;

    Signatures focused;
    if (!focused.generate(cnf, {}, net.freeVars, cfg, error)) {
        fail.what = "generating the focused population failed";
        return false;
    }
    if (!focused.gateSampling()) return true;

    if (focused.validSamples() != focused.sampleCount()) {
        fail.what = "the focus loop left lanes short of a reachable target";
        return false;
    }
    if (focused.focusLanes() != focused.validSamples()) {
        fail.what = "the reported focused lane count disagrees with the valid mask";
        return false;
    }

    const uint64_t* valid = focused.validMask();
    for (int w = 0; w < focused.words(); ++w) {
        for (int bit = 0; bit < 64; ++bit) {
            const uint64_t m = 1ull << bit;
            if (!(valid[w] & m)) continue;
            for (Lit l : focus) {
                const bool isTrue = (focused.var(litVar(l))[w] & m) != 0;
                if (isTrue == (litSign(l) != 0)) {
                    fail.what = "a valid lane misses one of the focused bits";
                    return false;
                }
            }
            // Constraining the population must not cost it its one real
            // property: every lane is still a model of the formula.
            for (size_t c = 0; c < cnf.clauseCount(); ++c) {
                bool sat = false;
                const Lit* b = cnf.clauseBegin(c);
                for (uint32_t k = 0, e = cnf.clauseLen(c); k < e && !sat; ++k) {
                    const bool isTrue = (focused.var(litVar(b[k]))[w] & m) != 0;
                    sat = isTrue != (litSign(b[k]) != 0);
                }
                if (!sat) {
                    fail.what = "a focused lane falsifies a clause";
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace

int runSelfTest(uint64_t seed, int rounds) {
    if (rounds <= 0) rounds = 20000;
    Failure fail;
    if (!checkRegressions(fail)) {
        std::printf("FAIL regression: %s\n", fail.what);
        return 1;
    }
    std::printf("TurboCryptoSAT self test - %d rounds from seed %llu\n", rounds,
                static_cast<unsigned long long>(seed));

    for (int i = 0; i < rounds; ++i) {
        const uint64_t s = seed + static_cast<uint64_t>(i);
        // This property is heavier than the scalar contracts: it compares two
        // populations and two probes under an order-preserving sparse renaming.
        if (i < 200) {
            Rng rng(s);
            const Cnf circuit = randomCircuit(rng, 4, 10);
            if (!SolverTestAccess::checkNumbering(circuit, s, true) ||
                !SolverTestAccess::checkNumbering(circuit, s, false)) {
                std::printf("FAIL numbering/signature scan (--seed %llu)\n",
                            static_cast<unsigned long long>(s));
                return 1;
            }
        }
        if (!checkSampling(s, fail)) {
            std::printf("FAIL sampling round %d (--seed %llu): %s\n", i,
                        static_cast<unsigned long long>(s), fail.what);
            return 1;
        }
        if (!checkCdcl(s, fail)) {
            std::printf("FAIL cdcl round %d (--seed %llu): %s\n", i,
                        static_cast<unsigned long long>(s), fail.what);
            return 1;
        }
        if (!checkGf2(s, fail)) {
            std::printf("FAIL gf2 round %d (--seed %llu): %s\n", i,
                        static_cast<unsigned long long>(s), fail.what);
            return 1;
        }
        if (!checkPropagatorUndo(s, fail)) {
            std::printf("FAIL propagator round %d (--seed %llu): %s\n", i,
                        static_cast<unsigned long long>(s), fail.what);
            return 1;
        }
        if (!checkFocus(s, fail)) {
            std::printf("FAIL focus round %d (--seed %llu): %s\n", i,
                        static_cast<unsigned long long>(s), fail.what);
            return 1;
        }
    }
    std::printf("ok - solver regressions, cdcl contract, gf2 elimination, propagator undo, "
                "propagated and focused sampling "
                "over %d formulas each\n", rounds);
    return 0;
}

}  // namespace tcs
