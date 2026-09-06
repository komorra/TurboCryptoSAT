// Bit parallel sample generation.
//
// The solver reasons about implications statistically, over a population of
// complete assignments that all satisfy the circuit part of the CNF. Rather
// than simulating one assignment at a time, every variable owns a bit vector of
// `words` 64-bit lanes, so 64*words sample instances are propagated at once.
//
// A sample is produced by randomising the input variables and running unit
// propagation; variables left undefined afterwards are randomised in turn until
// the whole assignment is total. Lanes that ran into a conflict are retried in
// later rounds and, if they keep failing, are excluded through the valid mask.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cnf.h"
#include "gates.h"

namespace tcs {

struct SignatureConfig {
    int words = 1024;     // 64-bit lanes per variable; 1024 lanes == 65536 samples
    uint64_t seed = 0;
    int threads = 1;
    int maxRounds = 12;   // retry rounds for lanes that hit a conflict

    // Recovered circuit, when the formula turned out to be one. A complete
    // network replaces propagation entirely: the samples are produced by
    // executing the gates, which is what makes resampling cheap enough to be
    // the solver's default answer to a stall.
    const GateNetwork* gates = nullptr;

    // Polled during sampling - once per retry round while propagating clauses,
    // and periodically through the gate pass - so a timeout or Ctrl+C is not
    // held up by a long sampling pass. Lanes generated so far are kept; an
    // interrupted pass simply yields fewer valid lanes.
    std::function<bool()> cancelled;
};

class Signatures {
public:
    // `fixedLits` are literals every sample must honour (unit clauses that were
    // not adopted as the target valuation). `inputVars` are randomised first;
    // the remaining variables are filled in index order as needed.
    bool generate(const Cnf& cnf, const std::vector<Lit>& fixedLits,
                  const std::vector<Var>& inputVars, const SignatureConfig& cfg,
                  std::string& error);

    // Allocates an empty population: every filter comes back empty and the
    // solver runs on propagation alone. Used when the instance has no driving
    // input set, where generating samples would burn seconds for no verdicts.
    void disable(int numVars, int words);

    int words() const { return words_; }
    // True when the population was produced by executing a recovered circuit
    // rather than by propagating the clauses.
    bool gateSampling() const { return gateSampling_; }
    size_t sampleCount() const { return static_cast<size_t>(words_) * 64; }
    uint64_t validSamples() const { return validSamples_; }

    const uint64_t* var(Var v) const {
        return sig_.data() + static_cast<size_t>(v) * static_cast<size_t>(words_);
    }
    const uint64_t* validMask() const { return valid_.data(); }

    // A transposed copy of the first few lanes, laid out variable-contiguously.
    // Testing one lane word across every variable is the innermost loop of the
    // solver; done through var() it is one cache miss per variable, whereas
    // here it is a single sequential scan.
    int probeWordCount() const { return probeWords_; }
    const uint64_t* probeWord(int w) const {
        return probeT_.data() + static_cast<size_t>(w) * static_cast<size_t>(numVars_);
    }

    // Peak resident bytes owned by the signature tables.
    uint64_t memoryBytes() const {
        return static_cast<uint64_t>(sig_.capacity() + valid_.capacity() + probeT_.capacity()) * 8u;
    }

private:
    // Executes the recovered circuit over one range of lanes, then clears from
    // the valid mask every lane that fails a clause - the network is only used
    // when it accounts for the whole formula, so this is a check, not a filter.
    // `aborted` is set when the pass stopped early on `cancelled`. The caller
    // needs to know: an interrupted verification pass leaves lanes standing that
    // were never checked, which must not be read as the network failing.
    void gateChunk(const Cnf& cnf, const GateNetwork& net, const std::vector<int8_t>& fixedVals,
                   int wordBegin, int wordEnd, uint64_t seed, bool verify,
                   const std::function<bool()>& cancelled, std::atomic<bool>& aborted);

    void generateChunk(const Cnf& cnf, const std::vector<Lit>& fixedLits,
                       const std::vector<Var>& inputVars, int maxRounds,
                       int wordBegin, int wordEnd, uint64_t seed,
                       const std::function<bool()>& cancelled);

    int words_ = 0;
    int numVars_ = 0;
    std::vector<uint64_t> sig_;    // numVars * words, bit set == variable is true
    std::vector<uint64_t> def_;    // scratch: assigned mask, released after generation
    std::vector<uint64_t> valid_;  // words, bit set == lane holds a usable sample
    std::vector<uint64_t> probeT_;  // probeWords_ * numVars, transposed
    int probeWords_ = 0;
    uint64_t validSamples_ = 0;
    bool gateSampling_ = false;
    bool gateVerified_ = false;   // the network has been checked against the clauses
    bool gateDisabled_ = false;   // ...and failed, so never use it again
};

}  // namespace tcs
