#include "cdcl.h"

#include <algorithm>
#include <cmath>

namespace tcs {

namespace {
// Luby sequence, the restart schedule: 1 1 2 1 1 2 4 ... Finite subsequences of
// it keep the search from tunnelling into one corner without giving up the long
// runs a hard instance needs.
double luby(double y, int x) {
    int size = 1, seq = 0;
    while (size < x + 1) {
        ++seq;
        size = 2 * size + 1;
    }
    while (size - 1 != x) {
        size = (size - 1) >> 1;
        --seq;
        x = x % size;
    }
    return std::pow(y, seq);
}
}  // namespace

bool Cdcl::attach(const Cnf& cnf, uint64_t seed) {
    cnf_ = &cnf;
    numVars_ = cnf.numVars;
    rng_.reseed(seed);

    const size_t nv = static_cast<size_t>(numVars_);
    assigns_.assign(nv, 0);
    polarity_.assign(nv, 1);  // MiniSat's default phase: try false first
    level_.assign(nv, 0);
    reason_.assign(nv, kNoRef);
    activity_.assign(nv, 0.0);
    heapPos_.assign(nv, -1);
    seen_.assign(nv, 0);
    lbdStamp_.assign(nv + 2, 0);
    model_.assign(nv, 0);
    watches_.clear();
    watches_.resize(nv * 2);
    trail_.clear();
    trail_.reserve(nv);
    trailLim_.clear();
    qhead_ = 0;
    conflicts_ = 0;
    varInc_ = 1.0;
    clauseInc_ = 1.0f;
    lbdCounter_ = 0;
    unsat_ = false;

    heap_.clear();
    heap_.reserve(nv);
    for (Var v = 0; v < numVars_; ++v) heapInsert(v);

    arena_.clear();
    originals_.clear();
    learnts_.clear();
    // One flat pool for the whole formula, sized up front: no per clause
    // allocation, and reduceDb() rebuilds it rather than leaving holes.
    arena_.reserve(cnf.lits.size() + kHeader * cnf.clauseCount() + 1024);

    const size_t nc = cnf.clauseCount();
    for (size_t c = 0; c < nc; ++c) {
        const uint32_t len = cnf.clauseLen(c);
        const Lit* b = cnf.clauseBegin(c);
        if (len == 0) { unsat_ = true; return false; }
        if (len == 1) {
            if (litValue(b[0]) == -1) { unsat_ = true; return false; }
            if (litValue(b[0]) == 0) uncheckedEnqueue(b[0], kNoRef);
            continue;
        }
        const CRef cr = allocClause(b, len, false, len);
        originals_.push_back(cr);
        attachClause(cr);
    }
    maxLearnts_ = static_cast<double>(originals_.size()) / 3.0 + 1000.0;

    if (propagate() != kNoRef) { unsat_ = true; return false; }
    return true;
}

void Cdcl::reset() {
    if (!numVars_ || !cnf_) return;
    // Learned clauses are implied by the formula *and* the pinned literals, so
    // retracting those retracts the clauses with them.
    const Cnf& cnf = *cnf_;
    attach(cnf, rng_.next());
}

Cdcl::CRef Cdcl::allocClause(const Lit* lits, uint32_t n, bool learnt, uint32_t lbd) {
    const CRef cr = static_cast<CRef>(arena_.size());
    arena_.resize(arena_.size() + kHeader + n);
    arena_[cr] = n;
    arena_[cr + 1] = (learnt ? 0x80000000u : 0u) | (lbd & 0x7FFFFFFFu);
    setClauseAct(cr, 0.0f);
    Lit* dst = clauseLits(cr);
    for (uint32_t i = 0; i < n; ++i) dst[i] = lits[i];
    return cr;
}

void Cdcl::attachClause(CRef c) {
    const Lit* l = clauseLits(c);
    const Lit l0 = l[0], l1 = l[1];
    watches_[static_cast<size_t>(litNeg(l0))].push_back(Watcher{c, l1});
    watches_[static_cast<size_t>(litNeg(l1))].push_back(Watcher{c, l0});
}

void Cdcl::uncheckedEnqueue(Lit l, CRef from) {
    const Var v = litVar(l);
    assigns_[static_cast<size_t>(v)] = (l & 1) ? static_cast<int8_t>(-1) : static_cast<int8_t>(1);
    level_[static_cast<size_t>(v)] = decisionLevel();
    reason_[static_cast<size_t>(v)] = from;
    trail_.push_back(l);
}

// Two watched literals. `watches_[p]` holds every clause that has ~p as one of
// its two watches, so assigning p true names exactly the clauses to revisit.
Cdcl::CRef Cdcl::propagate() {
    CRef confl = kNoRef;
    while (qhead_ < trail_.size()) {
        const Lit p = trail_[qhead_++];
        const Lit falseLit = litNeg(p);
        std::vector<Watcher>& ws = watches_[static_cast<size_t>(p)];
        size_t i = 0, j = 0;
        const size_t n = ws.size();
        while (i < n) {
            const Watcher w = ws[i];
            if (litValue(w.blocker) == 1) {
                ws[j++] = w;
                ++i;
                continue;
            }
            const CRef cr = w.cref;
            Lit* c = clauseLits(cr);
            const uint32_t sz = clauseSize(cr);
            if (c[0] == falseLit) {
                c[0] = c[1];
                c[1] = falseLit;
            }
            ++i;
            const Lit first = c[0];
            if (first != w.blocker && litValue(first) == 1) {
                ws[j++] = Watcher{cr, first};
                continue;
            }
            bool moved = false;
            for (uint32_t k = 2; k < sz; ++k) {
                if (litValue(c[k]) != -1) {
                    c[1] = c[k];
                    c[k] = falseLit;
                    // Never watches_[p] itself: clauses hold no duplicates, so
                    // the literal moved into c[1] cannot be falseLit again.
                    watches_[static_cast<size_t>(litNeg(c[1]))].push_back(Watcher{cr, first});
                    moved = true;
                    break;
                }
            }
            if (moved) continue;

            ws[j++] = Watcher{cr, first};
            if (litValue(first) == -1) {
                confl = cr;
                qhead_ = trail_.size();
                while (i < n) ws[j++] = ws[i++];
            } else {
                uncheckedEnqueue(first, cr);
            }
        }
        ws.resize(j);
    }
    return confl;
}

void Cdcl::cancelUntil(int level) {
    if (decisionLevel() <= level) return;
    const size_t stop = trailLim_[static_cast<size_t>(level)];
    for (size_t i = trail_.size(); i-- > stop;) {
        const Var v = litVar(trail_[i]);
        polarity_[static_cast<size_t>(v)] = static_cast<int8_t>(trail_[i] & 1);
        assigns_[static_cast<size_t>(v)] = 0;
        reason_[static_cast<size_t>(v)] = kNoRef;
        if (heapPos_[static_cast<size_t>(v)] < 0) heapInsert(v);
    }
    trail_.resize(stop);
    trailLim_.resize(static_cast<size_t>(level));
    qhead_ = stop;
}

// First UIP conflict analysis. Walks the implication graph backwards along the
// trail until one literal of the conflicting level is left; that literal
// negated, plus every lower level literal met on the way, is the learned clause.
void Cdcl::analyze(CRef confl, std::vector<Lit>& outLearnt, int& outBtLevel, uint32_t& outLbd) {
    int pathC = 0;
    Lit p = -1;
    outLearnt.clear();
    outLearnt.push_back(0);  // room for the asserting literal
    size_t index = trail_.size() - 1;

    do {
        const uint32_t sz = clauseSize(confl);
        if (clauseLearnt(confl)) setClauseAct(confl, clauseAct(confl) + clauseInc_);
        const Lit* c = clauseLits(confl);
        for (uint32_t k = (p < 0 ? 0u : 1u); k < sz; ++k) {
            const Lit q = c[k];
            const Var v = litVar(q);
            if (!seen_[static_cast<size_t>(v)] && level_[static_cast<size_t>(v)] > 0) {
                varBump(v);
                seen_[static_cast<size_t>(v)] = 1;
                if (level_[static_cast<size_t>(v)] >= decisionLevel()) {
                    ++pathC;
                } else {
                    outLearnt.push_back(q);
                }
            }
        }
        while (!seen_[static_cast<size_t>(litVar(trail_[index]))]) --index;
        p = trail_[index];
        if (index > 0) --index;
        confl = reason_[static_cast<size_t>(litVar(p))];
        seen_[static_cast<size_t>(litVar(p))] = 0;
        --pathC;
    } while (pathC > 0);

    outLearnt[0] = litNeg(p);

    // Self-subsuming resolution: a literal whose own reason is already covered
    // by the rest of the clause adds nothing to it.
    analyzeClear_.assign(outLearnt.begin(), outLearnt.end());
    size_t keep = 1;
    for (size_t i = 1; i < outLearnt.size(); ++i) {
        if (!litRedundant(outLearnt[i])) outLearnt[keep++] = outLearnt[i];
    }
    outLearnt.resize(keep);
    for (Lit l : analyzeClear_) seen_[static_cast<size_t>(litVar(l))] = 0;

    if (outLearnt.size() == 1) {
        outBtLevel = 0;
    } else {
        size_t best = 1;
        for (size_t i = 2; i < outLearnt.size(); ++i) {
            if (level_[static_cast<size_t>(litVar(outLearnt[i]))] >
                level_[static_cast<size_t>(litVar(outLearnt[best]))]) {
                best = i;
            }
        }
        std::swap(outLearnt[1], outLearnt[best]);
        outBtLevel = level_[static_cast<size_t>(litVar(outLearnt[1]))];
    }
    outLbd = computeLbd(outLearnt);
}

// True when `l` is implied by literals the learned clause already carries, so
// dropping it leaves the clause asserting. One resolution step deep, which is
// where most of the shrinking is and costs nothing to check.
bool Cdcl::litRedundant(Lit l) {
    const CRef cr = reason_[static_cast<size_t>(litVar(l))];
    if (cr == kNoRef) return false;
    const uint32_t sz = clauseSize(cr);
    const Lit* c = clauseLits(cr);
    for (uint32_t i = 1; i < sz; ++i) {
        const Var v = litVar(c[i]);
        if (!seen_[static_cast<size_t>(v)] && level_[static_cast<size_t>(v)] > 0) return false;
    }
    return true;
}

uint32_t Cdcl::computeLbd(const std::vector<Lit>& lits) {
    ++lbdCounter_;
    uint32_t n = 0;
    for (Lit l : lits) {
        const size_t lv = static_cast<size_t>(level_[static_cast<size_t>(litVar(l))]);
        if (lv < lbdStamp_.size() && lbdStamp_[lv] != lbdCounter_) {
            lbdStamp_[lv] = lbdCounter_;
            ++n;
        }
    }
    return n ? n : 1;
}

Lit Cdcl::pickBranchLit() {
    // A few random decisions keep the activity heap from tunnelling.
    if (rng_.below(1000) < 20) {
        const Var v = static_cast<Var>(rng_.below(static_cast<uint32_t>(numVars_)));
        if (assigns_[static_cast<size_t>(v)] == 0) {
            return mkLit(v, polarity_[static_cast<size_t>(v)] != 0);
        }
    }
    while (!heap_.empty()) {
        const Var v = heapPop();
        if (assigns_[static_cast<size_t>(v)] == 0) {
            return mkLit(v, polarity_[static_cast<size_t>(v)] != 0);
        }
    }
    return -1;
}

// Halves the learned clause database, then rebuilds the arena so the space the
// dropped clauses held is actually returned. Only ever called at level 0, where
// no learned clause is the reason for anything conflict analysis will look at.
void Cdcl::reduceDb() {
    std::sort(learnts_.begin(), learnts_.end(), [this](CRef a, CRef b) {
        const uint32_t la = clauseLbd(a), lb = clauseLbd(b);
        if (la != lb) return la > lb;  // worst (highest LBD) first
        return clauseAct(a) < clauseAct(b);
    });

    const size_t drop = learnts_.size() / 2;

    std::vector<uint32_t> next;
    next.reserve(arena_.size());
    std::vector<CRef> newOriginals;
    newOriginals.reserve(originals_.size());
    std::vector<CRef> newLearnts;
    newLearnts.reserve(learnts_.size() - drop);

    auto copyClause = [&](CRef cr) {
        const CRef out = static_cast<CRef>(next.size());
        const size_t n = kHeader + clauseSize(cr);
        next.insert(next.end(), arena_.begin() + cr, arena_.begin() + cr + n);
        return out;
    };
    for (CRef cr : originals_) newOriginals.push_back(copyClause(cr));
    for (size_t i = drop; i < learnts_.size(); ++i) newLearnts.push_back(copyClause(learnts_[i]));

    arena_.swap(next);
    originals_.swap(newOriginals);
    learnts_.swap(newLearnts);

    for (auto& w : watches_) w.clear();
    for (CRef cr : originals_) attachClause(cr);
    for (CRef cr : learnts_) attachClause(cr);
    // Everything on the trail sits at level 0 and conflict analysis never walks
    // into level 0, so the stale reasons are simply dropped.
    for (Lit l : trail_) reason_[static_cast<size_t>(litVar(l))] = kNoRef;
}

bool Cdcl::addRoot(Lit l) {
    if (unsat_) return false;
    cancelUntil(0);
    const int val = litValue(l);
    if (val == -1) { unsat_ = true; return false; }
    if (val == 0) {
        uncheckedEnqueue(l, kNoRef);
        if (propagate() != kNoRef) { unsat_ = true; return false; }
    }
    return true;
}

CdclResult Cdcl::run(uint64_t maxConflicts, const std::function<bool()>& cancelled) {
    if (unsat_) return CdclResult::RootConflict;
    cancelUntil(0);
    if (propagate() != kNoRef) { unsat_ = true; return CdclResult::RootConflict; }

    const size_t rootStart = trail_.size();
    const uint64_t conflictsStart = conflicts_;
    int restart = 0;
    double budgetToRestart = luby(2.0, restart) * 100.0;
    uint64_t conflictsThisRestart = 0;

    for (;;) {
        const CRef confl = propagate();
        if (confl != kNoRef) {
            ++conflicts_;
            ++conflictsThisRestart;
            if (decisionLevel() == 0) {
                unsat_ = true;
                return CdclResult::RootConflict;
            }
            int btLevel = 0;
            uint32_t lbd = 1;
            analyze(confl, learnt_, btLevel, lbd);
            cancelUntil(btLevel);
            if (learnt_.size() == 1) {
                uncheckedEnqueue(learnt_[0], kNoRef);
            } else {
                const CRef cr = allocClause(learnt_.data(),
                                            static_cast<uint32_t>(learnt_.size()), true, lbd);
                learnts_.push_back(cr);
                attachClause(cr);
                setClauseAct(cr, clauseInc_);
                uncheckedEnqueue(clauseLits(cr)[0], cr);
            }
            varDecay();
            clauseInc_ *= 1.001f;
            if (clauseInc_ > 1e20f) {
                for (CRef cr : learnts_) setClauseAct(cr, clauseAct(cr) * 1e-20f);
                clauseInc_ *= 1e-20f;
            }
            continue;
        }

        // No conflict, so the level 0 trail is closed under propagation:
        // anything new on it is proven and the phase has done its job.
        if (decisionLevel() == 0 && trail_.size() > rootStart) return CdclResult::Implied;

        if (cancelled && cancelled()) {
            cancelUntil(0);
            return CdclResult::Aborted;
        }
        if (maxConflicts > 0 && conflicts_ - conflictsStart >= maxConflicts) {
            cancelUntil(0);
            return CdclResult::Budget;
        }
        if (static_cast<double>(conflictsThisRestart) >= budgetToRestart) {
            conflictsThisRestart = 0;
            budgetToRestart = luby(2.0, ++restart) * 100.0;
            cancelUntil(0);
            if (static_cast<double>(learnts_.size()) >= maxLearnts_) {
                reduceDb();
                maxLearnts_ *= 1.1;
            }
            continue;
        }

        const Lit d = pickBranchLit();
        if (d < 0) {
            model_.assign(static_cast<size_t>(numVars_), 0);
            for (Var v = 0; v < numVars_; ++v) {
                const int8_t x = assigns_[static_cast<size_t>(v)];
                model_[static_cast<size_t>(v)] = x != 0 ? x : static_cast<int8_t>(1);
            }
            return CdclResult::Solved;
        }
        trailLim_.push_back(trail_.size());
        uncheckedEnqueue(d, kNoRef);
    }
}

uint64_t Cdcl::memoryBytes() const {
    uint64_t n = static_cast<uint64_t>(arena_.capacity()) * sizeof(uint32_t);
    for (const auto& w : watches_) n += static_cast<uint64_t>(w.capacity()) * sizeof(Watcher);
    n += static_cast<uint64_t>(originals_.capacity() + learnts_.capacity()) * sizeof(CRef);
    return n;
}

void Cdcl::varBump(Var v) {
    activity_[static_cast<size_t>(v)] += varInc_;
    if (activity_[static_cast<size_t>(v)] > 1e100) {
        for (auto& a : activity_) a *= 1e-100;
        varInc_ *= 1e-100;
    }
    if (heapPos_[static_cast<size_t>(v)] >= 0) {
        heapPercolateUp(heapPos_[static_cast<size_t>(v)]);
    }
}

void Cdcl::varDecay() { varInc_ *= (1.0 / 0.95); }

void Cdcl::heapInsert(Var v) {
    heapPos_[static_cast<size_t>(v)] = static_cast<int32_t>(heap_.size());
    heap_.push_back(v);
    heapPercolateUp(static_cast<int>(heap_.size()) - 1);
}

void Cdcl::heapPercolateUp(int i) {
    const Var v = heap_[static_cast<size_t>(i)];
    const double a = activity_[static_cast<size_t>(v)];
    while (i > 0) {
        const int p = (i - 1) >> 1;
        const Var pv = heap_[static_cast<size_t>(p)];
        if (activity_[static_cast<size_t>(pv)] >= a) break;
        heap_[static_cast<size_t>(i)] = pv;
        heapPos_[static_cast<size_t>(pv)] = static_cast<int32_t>(i);
        i = p;
    }
    heap_[static_cast<size_t>(i)] = v;
    heapPos_[static_cast<size_t>(v)] = static_cast<int32_t>(i);
}

void Cdcl::heapPercolateDown(int i) {
    const Var v = heap_[static_cast<size_t>(i)];
    const double a = activity_[static_cast<size_t>(v)];
    const int n = static_cast<int>(heap_.size());
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && activity_[static_cast<size_t>(heap_[static_cast<size_t>(c + 1)])] >
                             activity_[static_cast<size_t>(heap_[static_cast<size_t>(c)])]) {
            ++c;
        }
        const Var cv = heap_[static_cast<size_t>(c)];
        if (activity_[static_cast<size_t>(cv)] <= a) break;
        heap_[static_cast<size_t>(i)] = cv;
        heapPos_[static_cast<size_t>(cv)] = static_cast<int32_t>(i);
        i = c;
    }
    heap_[static_cast<size_t>(i)] = v;
    heapPos_[static_cast<size_t>(v)] = static_cast<int32_t>(i);
}

Var Cdcl::heapPop() {
    const Var top = heap_.front();
    heapPos_[static_cast<size_t>(top)] = -1;
    const Var last = heap_.back();
    heap_.pop_back();
    if (!heap_.empty()) {
        heap_[0] = last;
        heapPos_[static_cast<size_t>(last)] = 0;
        heapPercolateDown(0);
    }
    return top;
}

}  // namespace tcs
