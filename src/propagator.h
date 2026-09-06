// Counter based unit propagation with O(1) amortised undo.
//
// Every clause keeps the number of satisfied and falsified literals it holds.
// Assigning a literal walks the two occurrence lists of that literal, which is
// exactly the work that undoing it costs again, so speculative lookaheads can
// be pushed and rolled back without copying the assignment.
#pragma once

#include <cstdint>
#include <vector>

#include "cnf.h"

namespace tcs {

class Propagator {
public:
    void attach(const Cnf& cnf);

    // 0 = unassigned, 1 = true, -1 = false.
    int8_t varValue(Var v) const { return value_[static_cast<size_t>(v)]; }
    int litValue(Lit l) const {
        const int8_t x = value_[static_cast<size_t>(l >> 1)];
        return (l & 1) ? -x : x;
    }
    bool assigned(Var v) const { return value_[static_cast<size_t>(v)] != 0; }

    const std::vector<int8_t>& values() const { return value_; }
    size_t assignedCount() const { return trail_.size(); }
    const std::vector<Lit>& trail() const { return trail_; }

    // Pushes a literal. Returns false if it contradicts the current assignment.
    bool enqueue(Lit l) {
        const Var v = l >> 1;
        const int8_t cur = value_[static_cast<size_t>(v)];
        const int8_t want = (l & 1) ? static_cast<int8_t>(-1) : static_cast<int8_t>(1);
        if (cur != 0) return cur == want;
        value_[static_cast<size_t>(v)] = want;
        trail_.push_back(l);
        return true;
    }

    // Runs to fixpoint. Returns false when a clause became empty.
    bool propagate();

    size_t mark() const { return trail_.size(); }
    void undoTo(size_t m);

    bool conflict() const { return conflict_; }
    bool clauseSatisfied(size_t c) const { return satCnt_[c] != 0; }
    bool clauseFalsified(size_t c) const {
        return satCnt_[c] == 0 && falseCnt_[c] == cnf_->clauseLen(c);
    }
    uint32_t satisfiedClauses() const { return satisfied_; }

private:
    const Cnf* cnf_ = nullptr;
    std::vector<int8_t> value_;
    std::vector<Lit> trail_;
    std::vector<uint32_t> satCnt_;
    std::vector<uint32_t> falseCnt_;
    size_t qhead_ = 0;
    bool conflict_ = false;
    uint32_t satisfied_ = 0;
};

}  // namespace tcs
