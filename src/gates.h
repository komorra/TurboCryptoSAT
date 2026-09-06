// Gate recovery from a Tseitin encoded CNF.
//
// The sample population only has to be a set of free executions of the encoded
// circuit, and a circuit is far cheaper to execute than to propagate: reading
// the gates back out of the clauses turns sample generation from repeated unit
// propagation sweeps over the whole formula into one word-parallel pass over
// the gate list, in topological order, with no conflicts and no retries.
//
// Three patterns are matched, which together cover what an AND/OR/XOR/NOT
// Tseitin encoder emits (OR is an AND of negated literals, and both polarities
// of the output are tried, so NOR/NAND/XNOR fall out of the same three):
//
//   o == x & y   ->  (~o | x), (~o | y), (o | ~x | ~y)
//   o == ~x      ->  (~o | ~x), (o | x)      - and o == x, the same shape
//   o == x ^ y   ->  the four ternary clauses over {o,x,y} that forbid every
//                    assignment of one parity
//
// The second one matters more than its size suggests. An encoder that folds
// negation into the literal never emits it, but one that gives NOT a variable
// of its own puts inverters in the middle of the circuit, and every gate
// reachable only through one of them is lost with it.
//
// Anything not matched simply stays a residual clause; the caller decides
// whether the recovered network explains enough of the formula to be used.
#pragma once

#include <cstdint>
#include <vector>

#include "cnf.h"

namespace tcs {

// out := negOut ^ (in0 op in1). Input polarities are folded into the literals,
// so evaluating a gate is one XOR per input plus the operation itself.
struct Gate {
    enum Op : uint8_t { And, Xor };
    Var out = 0;
    Lit in0 = 0;
    Lit in1 = 0;
    Op op = And;
    bool negOut = false;
};

struct GateNetwork {
    // Topologically ordered: every input of a gate is either a free variable or
    // the output of an earlier gate.
    std::vector<Gate> gates;
    std::vector<Var> freeVars;    // nothing defines these; they carry the randomness
    std::vector<uint8_t> isFree;  // per variable, indexed by Var
    size_t residualClauses = 0;   // clauses no accepted gate accounts for
    size_t clauseCount = 0;

    // A network that accounts for every clause reproduces the formula exactly:
    // any valuation of the free variables extends to a satisfying assignment.
    bool complete() const { return residualClauses == 0 && !gates.empty(); }
};

// Matches the patterns above and orders what it finds. Never fails: an
// unrecognised formula simply comes back with no gates and every clause
// residual.
void extractGates(const Cnf& cnf, GateNetwork& net);

}  // namespace tcs
