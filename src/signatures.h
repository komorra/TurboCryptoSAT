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

    // Bits of the target valuation every lane must reproduce. Free executions
    // of the circuit hit them only by accident, so they are imposed by
    // rejection: a lane that misses one is redrawn, and only a lane that hits
    // all of them ever becomes valid. Empty leaves the population unfiltered,
    // which is what the solver's own filtering step assumes.
    std::vector<Lit> focusLits;

    // Called from the rejection loop between batches of redraws, with the lanes
    // that already match, the lanes there are, and the redraws spent so far.
    // Runs on the calling thread while the workers are joined, so it may touch
    // the caller's state without locking.
    std::function<void(uint64_t ok, uint64_t total, uint64_t rounds)> focusProgress;

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
    // Target bits the population was focused on, the lanes that reproduce them
    // and the redraws that took, all zero when focusing was off.
    int focusBits() const { return focusBits_; }
    uint64_t focusLanes() const { return focusLanes_; }
    uint64_t focusRounds() const { return focusRounds_; }
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

    // Rejection sampling that pins `focus` onto every lane of the population.
    // Lanes are redrawn in lockstep a word at a time: a lane that matches is
    // never touched again, so a word drops out of the loop once all 64 of its
    // lanes have landed. `focusBad_` is the shared state - each range owns its
    // own words - and whatever is still set in it when the loop ends never met
    // the target and is struck from the valid mask.
    void focusPopulation(const GateNetwork& net, const std::vector<int8_t>& fixedVals,
                         const std::vector<Lit>& focus, const SignatureConfig& cfg);
    void focusChunk(const GateNetwork& net, const std::vector<int8_t>& fixedVals,
                    const std::vector<Lit>& focus, int wordBegin, int wordEnd, uint64_t seed,
                    int iterations, const std::function<bool()>& cancelled);

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
    std::vector<uint64_t> focusBad_;  // words, bit set == lane misses a target bit
    int probeWords_ = 0;
    int focusBits_ = 0;
    uint64_t focusLanes_ = 0;
    uint64_t focusRounds_ = 0;
    uint64_t validSamples_ = 0;
    bool gateSampling_ = false;
    bool gateVerified_ = false;   // the network has been checked against the clauses
    bool gateDisabled_ = false;   // ...and failed, so never use it again
};

}  // namespace tcs
