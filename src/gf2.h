// Linear reasoning over GF(2): the parity constraints hidden in the CNF.
//
// A Tseitin encoder turns `a ^ b ^ c = 1` into four ternary clauses, and unit
// propagation over those four clauses is exactly as strong as the constraint
// itself - it fires when two of the three are known and not before. What it can
// never do is *add* two constraints together. Chains of parities are the shape
// of a hash round function, and a solver that only ever looks at one of them at
// a time has to assign its way to the end of every chain before anything falls
// out.
//
// Gaussian elimination does add them. Two equations sharing an intermediate
// variable combine into one that does not mention it, and the reduced system
// says things about *distant* variables directly:
//
//   * a row that reduces to one variable is a unit - a proven assignment,
//   * a row that reduces to two is an equivalence, `x = y` or `x = ~y`, which
//     is a pair of binary clauses no amount of BCP over the original encoding
//     would have produced,
//   * a row that reduces to nothing but a `1` on the right is a conflict: the
//     assignment fed in is refuted.
//
// All three are sound consequences of the formula. Nothing here is a bet.
//
// The system is rebuilt and re-reduced from scratch on every call rather than
// maintained incrementally under the assignment. That is the deliberate trade:
// elimination on these instances costs single-digit milliseconds, and a
// from-scratch reduction cannot drift out of step with the solver's trail the
// way an incremental one can.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cnf.h"

namespace tcs {

struct Gf2Result {
    bool conflict = false;         // a row reduced to 0 = 1
    std::vector<Lit> units;        // rows that reduced to a single variable
    // Rows that reduced to two variables, as the pair of binary clauses that
    // says so: `x ^ y = 0` is (~x | y) and (x | ~y), `x ^ y = 1` the other two.
    std::vector<Lit> equivA;       // one literal per equivalence...
    std::vector<Lit> equivB;       // ...and its partner; the relation is
                                   // equivA[i] -> equivB[i] and back.
};

class Gf2System {
public:
    // Recovers every XOR constraint the clauses encode. Never fails: a formula
    // with no parity structure simply comes back with no equations.
    void build(const Cnf& cnf);

    bool empty() const { return rows_.empty(); }
    size_t equationCount() const { return rows_.size(); }
    size_t varCount() const { return cols_.size(); }
    // Clauses accounted for by the recovered equations, for the summary line.
    size_t clausesUsed() const { return clausesUsed_; }

    // Substitutes `values` (per variable, 1 true / -1 false / 0 unassigned),
    // reduces the system to row echelon form and then to reduced row echelon
    // form, and reports what the short rows say. `out` is cleared first.
    void solve(const std::vector<int8_t>& values, Gf2Result& out);

    uint64_t memoryBytes() const;

private:
    // One equation, as the variables it mentions plus the parity they sum to.
    struct Row {
        uint32_t begin = 0;  // into vars_
        uint32_t end = 0;
        uint8_t rhs = 0;
    };

    // A reduced pivot row, keyed by the free variables it still depends on.
    struct Support {
        uint64_t hash;
        uint32_t row;
        int32_t col;   // the row's pivot column
    };
    bool sameSupport(const Support& a, const Support& b) const;

    uint64_t* work(size_t r) { return work_.data() + r * words_; }
    const uint64_t* work(size_t r) const { return work_.data() + r * words_; }

    std::vector<Row> rows_;
    std::vector<Var> vars_;       // flat pool of the rows' variables
    std::vector<Var> cols_;       // column index -> variable
    std::vector<int32_t> colOf_;  // variable -> column index, -1 when absent
    size_t clausesUsed_ = 0;

    // Scratch, reused across calls: the dense bitset matrix the reduction runs
    // on, `words_` 64-bit words per row.
    size_t words_ = 0;
    std::vector<uint64_t> work_;
    std::vector<uint8_t> workRhs_;
    std::vector<int32_t> pivotOfCol_;  // column -> row in work_, -1 when none
    std::vector<int32_t> pivotCols_;   // pivot columns, in the order installed
    std::vector<Support> sup_;
};

}  // namespace tcs
