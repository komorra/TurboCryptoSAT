// A compact CDCL search used as the solver's answer to a stall.
//
// The signature loop assigns literals in place and never backtracks, so when it
// runs out of agreement its only ways forward are a fresh sample population or
// a decision it cannot take back. This is the third way: a conventional CDCL
// search over the same formula, with the literals the signature loop has
// already committed pinned as level 0 units. It is run in bounded phases and
// hands back only what it can prove, so nothing it contributes is a bet.
//
// A phase ends the moment one of three things happens:
//   * a learned unit lands a new literal on the level 0 trail (Implied) - that
//     literal follows from the formula and the pinned assignment alone,
//   * every variable is assigned (Solved),
//   * a clause is falsified at level 0 (RootConflict) - the pinned assignment
//     is refuted, so the attempt is dead and the solver restarts.
//
// Learned clauses survive across phases within one attempt. They are implied by
// the formula *and* the pinned literals, so `reset()` has to drop them whenever
// the solver retracts the assignment they were derived under.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "cnf.h"
#include "rng.h"

namespace tcs {

enum class CdclResult {
    Implied,       // at least one new literal on the level 0 trail
    Solved,        // a full model was found
    RootConflict,  // conflict at level 0: the pinned assignment is refuted
    Budget,        // conflict budget spent without either
    Aborted,       // interrupted or out of time
};

class Cdcl {
public:
    // Loads the formula. Unit clauses are pinned straight away; false means the
    // formula is refuted by them alone.
    bool attach(const Cnf& cnf, uint64_t seed);

    // Drops every learned clause and the whole trail, keeping the original
    // clauses. Needed whenever the caller retracts pinned literals.
    void reset();

    bool attached() const { return numVars_ > 0; }

    // Pins a literal at level 0 and propagates it. False means it contradicts
    // what is already pinned.
    bool addRoot(Lit l);

    // Searches until one of the three stop conditions above, `maxConflicts`
    // conflicts have been analysed (0 = no limit), or `cancelled` says stop.
    // On return the search is back at level 0, so `rootTrail()` is current.
    CdclResult run(uint64_t maxConflicts, const std::function<bool()>& cancelled);

    // The level 0 trail. Valid after attach(), addRoot() and run(); everything
    // past the size the caller last saw is newly proven.
    const std::vector<Lit>& rootTrail() const { return trail_; }

    // Filled in when run() returned Solved: one entry per variable, 1 or -1.
    const std::vector<int8_t>& model() const { return model_; }

    uint64_t conflicts() const { return conflicts_; }
    uint64_t learnedClauses() const { return learnts_.size(); }
    uint64_t memoryBytes() const;

private:
    using CRef = uint32_t;
    static constexpr CRef kNoRef = 0xFFFFFFFFu;
    static constexpr uint32_t kHeader = 3;  // size, learnt|lbd, activity

    struct Watcher {
        CRef cref;
        Lit blocker;  // if this literal is true the clause needs no visit
    };

    // Arena accessors. Every pointer into the arena dies on the next
    // allocation, so nothing holds one across allocClause() or reduceDb().
    uint32_t clauseSize(CRef c) const { return arena_[c]; }
    Lit* clauseLits(CRef c) { return reinterpret_cast<Lit*>(arena_.data() + c + kHeader); }
    const Lit* clauseLits(CRef c) const {
        return reinterpret_cast<const Lit*>(arena_.data() + c + kHeader);
    }
    bool clauseLearnt(CRef c) const { return (arena_[c + 1] >> 31) != 0; }
    uint32_t clauseLbd(CRef c) const { return arena_[c + 1] & 0x7FFFFFFFu; }
    float clauseAct(CRef c) const {
        float f;
        std::memcpy(&f, &arena_[c + 2], sizeof(float));
        return f;
    }
    void setClauseAct(CRef c, float f) { std::memcpy(&arena_[c + 2], &f, sizeof(float)); }

    CRef allocClause(const Lit* lits, uint32_t n, bool learnt, uint32_t lbd);
    void attachClause(CRef c);

    int litValue(Lit l) const {
        const int8_t x = assigns_[static_cast<size_t>(l >> 1)];
        return (l & 1) ? -x : x;
    }
    int decisionLevel() const { return static_cast<int>(trailLim_.size()); }
    void uncheckedEnqueue(Lit l, CRef from);
    CRef propagate();
    void cancelUntil(int level);
    void analyze(CRef confl, std::vector<Lit>& outLearnt, int& outBtLevel, uint32_t& outLbd);
    bool litRedundant(Lit l);
    Lit pickBranchLit();
    void reduceDb();
    uint32_t computeLbd(const std::vector<Lit>& lits);

    // Variable activity, kept in a binary heap so the decision is O(1) to read
    // and O(log n) to maintain.
    void varBump(Var v);
    void varDecay();
    void heapInsert(Var v);
    void heapPercolateUp(int i);
    void heapPercolateDown(int i);
    Var heapPop();
    bool heapEmpty() const { return heap_.empty(); }

    const Cnf* cnf_ = nullptr;
    int numVars_ = 0;

    std::vector<uint32_t> arena_;
    std::vector<CRef> originals_;
    std::vector<CRef> learnts_;
    std::vector<std::vector<Watcher>> watches_;  // indexed by literal

    std::vector<int8_t> assigns_;   // 0 undef, 1 true, -1 false
    std::vector<int8_t> polarity_;  // saved phase: 1 = try negated first
    std::vector<int32_t> level_;
    std::vector<CRef> reason_;
    std::vector<Lit> trail_;
    std::vector<size_t> trailLim_;
    size_t qhead_ = 0;

    std::vector<double> activity_;
    std::vector<int32_t> heapPos_;
    std::vector<Var> heap_;
    double varInc_ = 1.0;
    float clauseInc_ = 1.0f;

    std::vector<uint8_t> seen_;
    std::vector<Lit> analyzeClear_;
    std::vector<Lit> learnt_;
    std::vector<uint32_t> lbdStamp_;
    uint32_t lbdCounter_ = 0;

    std::vector<int8_t> model_;

    Rng rng_;
    uint64_t conflicts_ = 0;
    double maxLearnts_ = 0.0;
    bool unsat_ = false;
};

}  // namespace tcs
