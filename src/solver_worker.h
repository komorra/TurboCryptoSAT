// Internal worker state shared with the probe contract tests.
#pragma once
#include "solver.h"

namespace tcs {
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

    std::vector<uint32_t> stamp;
    std::vector<int8_t> stampPol;
    uint32_t stampCounter = 0;

    std::vector<uint64_t> keep;
    std::vector<uint32_t> keepIdx;
    std::vector<uint64_t> keepVal;
    std::vector<Var> candidates;

    bool hardConflict = false;
    uint64_t sigVerdicts = 0;
    uint64_t sigBails = 0;
};

}  // namespace tcs
