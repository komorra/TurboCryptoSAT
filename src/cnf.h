// Compact CNF storage plus DIMACS reading/writing.
//
// Literals use the standard internal encoding: variable v (1-based in DIMACS)
// maps to index v-1, and literal l = 2*(v-1) + (negated ? 1 : 0). The negation
// of l is l^1, which keeps polarity flips branch free.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tcs {

using Lit = int32_t;
using Var = int32_t;

// Largest DIMACS variable number the encoding can carry. A literal is
// 2*(v-1) + sign in an int32_t, and the occurrence index is 2*numVars + 1, so
// anything at or above 2^30 is signed overflow rather than a large instance.
// No real formula comes near this; a file that does is malformed.
constexpr int kMaxVar = 1 << 28;

inline Lit dimacsToLit(int d) {
    return d > 0 ? ((d - 1) << 1) : (((-d) - 1) << 1) | 1;
}
inline int litToDimacs(Lit l) {
    const int v = (l >> 1) + 1;
    return (l & 1) ? -v : v;
}
inline Var litVar(Lit l) { return l >> 1; }
inline bool litSign(Lit l) { return (l & 1) != 0; }  // true when negated
inline Lit litNeg(Lit l) { return l ^ 1; }
inline Lit mkLit(Var v, bool negated) { return (v << 1) | (negated ? 1 : 0); }

// Clauses are stored in one flat literal pool with an index of start offsets,
// which keeps them contiguous in memory and free of per-clause allocations.
struct Cnf {
    int numVars = 0;
    std::vector<Lit> lits;
    std::vector<uint32_t> start;  // size = clauseCount() + 1

    // Occurrence lists, also flattened: occ[occStart[l] .. occStart[l+1]) holds
    // the indices of every clause containing literal l.
    std::vector<uint32_t> occ;
    std::vector<uint32_t> occStart;  // size = 2*numVars + 1

    uint32_t maxClauseLen = 0;
    bool hasEmptyClause = false;

    size_t clauseCount() const { return start.empty() ? 0 : start.size() - 1; }
    const Lit* clauseBegin(size_t c) const { return lits.data() + start[c]; }
    uint32_t clauseLen(size_t c) const { return start[c + 1] - start[c]; }

    void buildOccurrences();
};

// Parses a DIMACS CNF file. Duplicate literals inside a clause are removed and
// tautological clauses are dropped. Unit clause literals are reported
// separately so the caller can decide whether they are a target valuation.
bool loadDimacs(const std::string& path, Cnf& cnf, std::vector<Lit>& unitLits,
                std::string& error);

// Writes the assignment as a DIMACS CNF made of unit clauses, which is both a
// valid CNF and a directly readable solution.
bool writeSolutionCnf(const std::string& path, const std::string& sourceName,
                      const std::vector<int8_t>& value, int numVars, std::string& error);

// Path of the solution file that belongs to the given input file:
// "foo.cnf" becomes "foo.solution.cnf".
std::string solutionPathFor(const std::string& cnfPath);

}  // namespace tcs
