#include "signatures.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <stdexcept>
#include <thread>

#include "platform.h"
#include "rng.h"

namespace tcs {
namespace {

// Decides whether the recovered circuit can stand in for propagation, and
// turns the fixed literals into a per variable value while it is at it.
//
// Two things have to hold. The network must account for every clause, so that
// any valuation of the free variables extends to a satisfying assignment - a
// residual clause could be violated by an execution, and the whole point of the
// fast path is that it never has to retry. And every fixed literal must land on
// a free variable, where honouring it is just a matter of not randomising it;
// a literal pinning a gate output would need the circuit run backwards, which
// is exactly the search the solver is trying to avoid.
const GateNetwork* usableNetwork(const GateNetwork* net, const Cnf& cnf,
                                 const std::vector<Lit>& fixedLits,
                                 std::vector<int8_t>& fixedVals) {
    if (!net || !net->complete()) return nullptr;
    if (net->clauseCount != cnf.clauseCount()) return nullptr;
    if (static_cast<int>(net->isFree.size()) != cnf.numVars) return nullptr;

    fixedVals.assign(static_cast<size_t>(cnf.numVars), 0);
    for (Lit l : fixedLits) {
        const Var v = litVar(l);
        if (v < 0 || v >= cnf.numVars) return nullptr;
        if (!net->isFree[static_cast<size_t>(v)]) return nullptr;
        const int8_t want = litSign(l) ? -1 : 1;
        if (fixedVals[static_cast<size_t>(v)] != 0 && fixedVals[static_cast<size_t>(v)] != want) {
            return nullptr;  // the fixed literals contradict each other
        }
        fixedVals[static_cast<size_t>(v)] = want;
    }
    return net;
}

}  // namespace

bool Signatures::generate(const Cnf& cnf, const std::vector<Lit>& fixedLits,
                          const std::vector<Var>& inputVars, const SignatureConfig& cfg,
                          std::string& error) {
    words_ = std::max(1, cfg.words);
    numVars_ = cnf.numVars;
    focusBits_ = static_cast<int>(cfg.focusLits.size());
    focusLanes_ = 0;
    focusRounds_ = 0;

    // All generators must honour the same units, including callers that pass
    // a CNF directly rather than Solver's unit-free sampling formula.
    std::vector<Lit> units = fixedLits;
    for (size_t c = 0; c < cnf.clauseCount(); ++c) {
        if (cnf.clauseLen(c) == 0) {
            disable(cnf.numVars, words_);
            return true;
        }
        if (cnf.clauseLen(c) == 1) units.push_back(cnf.clauseBegin(c)[0]);
    }
    std::vector<int8_t> requested(static_cast<size_t>(numVars_), 0);
    auto validate = [&](const std::vector<Lit>& lits) {
        for (Lit l : lits) {
            const Var v = litVar(l);
            if (v < 0 || v >= numVars_) {
                error = "sample literal out of range";
                return false;
            }
            const int8_t want = litSign(l) ? -1 : 1;
            if (requested[v] && requested[v] != want) {
                error = "contradictory fixed or focused sample literals";
                return false;
            }
            requested[v] = want;
        }
        return true;
    };
    if (!validate(units) || !validate(cfg.focusLits)) return false;

    std::vector<int8_t> fixedVals;
    const GateNetwork* net = gateDisabled_ ? nullptr
                                           : usableNetwork(cfg.gates, cnf, units, fixedVals);
    const size_t cells = static_cast<size_t>(numVars_) * static_cast<size_t>(words_);

    // The target bits split in two by what it costs to honour them. One that
    // lands on a free variable is not sampled for at all - the variable is
    // simply not randomised, exactly as a fixed literal is - so only the bits a
    // gate defines are left to the rejection loop, which is where the 2^n is.
    std::vector<Lit> focus;
    if (net && !cfg.focusLits.empty()) {
        for (Lit l : cfg.focusLits) {
            const Var v = litVar(l);
            if (v < 0 || v >= cnf.numVars) continue;
            if (!net->isFree[static_cast<size_t>(v)]) {
                focus.push_back(l);
                continue;
            }
            const int8_t want = litSign(l) ? -1 : 1;
            if (fixedVals[static_cast<size_t>(v)] != 0 &&
                fixedVals[static_cast<size_t>(v)] != want) {
                error = "the target bits to focus on contradict the unit clauses";
                return false;
            }
            fixedVals[static_cast<size_t>(v)] = want;
        }
    }

    // The propagating generator has no rejection loop. There the target bits go
    // in as ordinary fixed literals: propagation either extends a lane to
    // honour them or the lane conflicts and drops out, which reaches the same
    // population far less efficiently - and, on an instance whose outputs are
    // this hard to hit, an almost empty one. It is the honest answer rather
    // than the fast one, and the lane count in the summary shows which it was.
    std::vector<Lit> propFixed;
    const std::vector<Lit>* propLits = &units;
    if (!cfg.focusLits.empty()) {
        propFixed = units;
        propFixed.insert(propFixed.end(), cfg.focusLits.begin(), cfg.focusLits.end());
        propLits = &propFixed;
    }

    // The clause check only has to run once. The gate list is the same on every
    // redraw and its execution is lane independent, so a population that came
    // out clean vouches for the network itself, not just for those lanes.
    bool verify = !gateVerified_;
    // Set by any worker that stopped early on the cancel callback.
    std::atomic<bool> aborted(false);

    // bad_alloc is the ordinary failure; length_error is what an absurd size
    // raises before the allocator is ever asked, and it has to be caught here
    // too or a malformed header takes the process down.
    auto tooBig = [&] {
        error = "not enough memory for the signature table; lower --siglen";
        return false;
    };

    auto runPass = [&](bool useGates) {
        try {
            sig_.assign(cells, 0);
            // Executing a circuit never leaves a variable undecided, so the
            // fast path needs no assigned mask - and with it goes half the
            // memory.
            if (useGates) std::vector<uint64_t>().swap(def_);
            else def_.assign(cells, 0);
            valid_.assign(static_cast<size_t>(words_), 0);
        } catch (const std::bad_alloc&) {
            return tooBig();
        } catch (const std::length_error&) {
            return tooBig();
        }

        int threads = std::max(1, cfg.threads);
        threads = std::min(threads, words_);

        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(threads));
        const int per = (words_ + threads - 1) / threads;
        for (int t = 0; t < threads; ++t) {
            const int b = t * per;
            const int e = std::min(words_, b + per);
            if (b >= e) break;
            const uint64_t seed = cfg.seed ^ (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(t + 1));
            auto run = [&, b, e, seed] {
                if (useGates) {
                    gateChunk(cnf, *net, fixedVals, b, e, seed, verify, cfg.cancelled, aborted);
                } else {
                    generateChunk(cnf, *propLits, inputVars, cfg.maxRounds, b, e, seed,
                                  cfg.cancelled);
                }
            };
            if (t + 1 == threads) run();
            else pool.emplace_back(run);
        }
        for (auto& th : pool) th.join();

        validSamples_ = 0;
        for (uint64_t w : valid_) validSamples_ += popcount64(w);
        return true;
    };

    gateSampling_ = net != nullptr;
    if (!runPass(gateSampling_)) return false;

    // An interrupted pass says nothing about the network, so the verdict below
    // is only reached when the check actually ran to the end.
    if (gateSampling_ && verify && !aborted.load()) {
        if (validSamples_ == sampleCount()) {
            gateVerified_ = true;
        } else {
            // A lane that fails a clause means the recovered network is not the
            // formula after all - a pattern was matched that does not mean what
            // it looked like. Rather than solve on a poisoned population, drop
            // the fast path for good and redraw by propagation.
            gateDisabled_ = true;
            gateSampling_ = false;
            verify = false;
            if (!runPass(false)) return false;
        }
    }

    // Narrowing the population onto the target bits comes after the network has
    // vouched for itself: the loop below redraws lanes through the same gates,
    // and doing that on a network that turned out not to be the formula would
    // spend its whole budget chasing a target those lanes cannot reach.
    if (gateSampling_ && !focus.empty()) {
        focusPopulation(*net, fixedVals, focus, cfg);
    } else if (!cfg.focusLits.empty()) {
        // Either every requested bit landed on a free variable, or the
        // propagating generator honoured them as fixed literals. A valid lane
        // reproduces them either way.
        focusLanes_ = validSamples_;
    }

    validSamples_ = 0;
    for (uint64_t w : valid_) validSamples_ += popcount64(w);

    // The assigned mask is scratch space; give the memory back before solving.
    std::vector<uint64_t>().swap(def_);

    // Transpose the leading lane words. Eight of them are 512 lanes, enough for
    // the prefilter to be exact in practice while costing 8 words per variable.
    probeWords_ = std::min(8, words_);
    try {
        probeT_.assign(static_cast<size_t>(probeWords_) * static_cast<size_t>(numVars_), 0);
    } catch (const std::bad_alloc&) {
        return tooBig();
    } catch (const std::length_error&) {
        return tooBig();
    }
    for (int w = 0; w < probeWords_; ++w) {
        uint64_t* col = probeT_.data() + static_cast<size_t>(w) * static_cast<size_t>(numVars_);
        const uint64_t* src = sig_.data() + static_cast<size_t>(w);
        for (int v = 0; v < numVars_; ++v) {
            col[v] = src[static_cast<size_t>(v) * static_cast<size_t>(words_)];
        }
    }

    // An empty population is not an error. It means random values for the input
    // set conflict on every lane - random k-SAT does this - and the solver
    // simply runs without the statistical layer: with no valid lane, every
    // filter comes back empty and each probe falls back to propagation. The
    // summary reports the lane count so the cause is visible.
    return true;
}

// Rejection sampling: redraw the lanes that miss a target bit until none does.
//
// This is the whole of the --focus mechanism, and the shape of it is what makes
// it affordable. A lane that hits the target is finished - its values are
// masked out of every later redraw - so the population converges lane by lane
// rather than being thrown away and regenerated whole. Lanes are redrawn in
// lockstep 64 at a time, which is what keeps the bit parallelism, and a word
// whose every lane has landed drops out of the loop entirely. The cost is
// therefore the expected worst of 64 geometric draws per word, around
// 4.7 * 2^bits redraws of that word, not 2^bits redraws of the population.
void Signatures::focusPopulation(const GateNetwork& net, const std::vector<int8_t>& fixedVals,
                                 const std::vector<Lit>& focus, const SignatureConfig& cfg) {
    focusBad_.assign(static_cast<size_t>(words_), 0);
    for (Lit l : focus) {
        const uint64_t* row =
            sig_.data() + static_cast<size_t>(litVar(l)) * static_cast<size_t>(words_);
        // A negated literal wants the variable false, so its own row is the
        // mismatch; a plain one wants it true, and the complement is.
        const bool neg = litSign(l) != 0;
        for (int w = 0; w < words_; ++w) {
            focusBad_[static_cast<size_t>(w)] |= neg ? row[w] : ~row[w];
        }
    }
    // Lanes that are not samples at all stay out of it: redrawing one would not
    // make it valid, and counting it would put a floor under the loop that it
    // could never get past.
    for (int w = 0; w < words_; ++w) {
        focusBad_[static_cast<size_t>(w)] &= valid_[static_cast<size_t>(w)];
    }

    auto missing = [&] {
        uint64_t n = 0;
        for (uint64_t x : focusBad_) n += popcount64(x);
        return n;
    };

    const uint64_t total = validSamples_;
    uint64_t bad = missing();
    if (cfg.focusProgress) cfg.focusProgress(total - bad, total, 0);

    int threads = std::max(1, cfg.threads);
    threads = std::min(threads, words_);
    const int per = (words_ + threads - 1) / threads;

    // Redraws are dispatched in batches. One redraw is nowhere near enough work
    // to pay for launching threads, and running the whole loop inside the
    // workers would leave the caller - and so the dashboard - with nothing to
    // show for however long convergence takes.
    const int kBatch = 32;
    while (bad > 0) {
        if (cfg.cancelled && cfg.cancelled()) break;

        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(threads));
        for (int t = 0; t < threads; ++t) {
            const int b = t * per;
            const int e = std::min(words_, b + per);
            if (b >= e) break;
            const uint64_t seed = cfg.seed ^
                                  (0x9E3779B97F4A7C15ull * static_cast<uint64_t>(t + 1)) ^
                                  (0xD1B54A32D192ED03ull * (focusRounds_ + 1));
            auto run = [&, b, e, seed] {
                focusChunk(net, fixedVals, focus, b, e, seed, kBatch, cfg.cancelled);
            };
            if (t + 1 == threads) run();
            else pool.emplace_back(run);
        }
        for (auto& th : pool) th.join();

        focusRounds_ += static_cast<uint64_t>(kBatch);
        bad = missing();
        if (cfg.focusProgress) cfg.focusProgress(total - bad, total, focusRounds_);
    }

    // A lane still missing a bit when the loop ends - interrupted, or out of
    // time - is not a sample of the focused population and must not be left in
    // it. Dropping it leaves a smaller but honest population, and the solver
    // already copes with one that came back thin.
    for (int w = 0; w < words_; ++w) {
        valid_[static_cast<size_t>(w)] &= ~focusBad_[static_cast<size_t>(w)];
    }
    focusLanes_ = total - bad;
}

void Signatures::focusChunk(const GateNetwork& net, const std::vector<int8_t>& fixedVals,
                            const std::vector<Lit>& focus, int wordBegin, int wordEnd,
                            uint64_t seed, int iterations,
                            const std::function<bool()>& cancelled) {
    const int words = words_;
    const int span = wordEnd - wordBegin;
    uint64_t* sig = sig_.data();
    uint64_t* bad = focusBad_.data();
    Rng rng(seed);

    const uint32_t kPollEvery = 4096;
    std::vector<int> act;  // words of this range that still miss a bit
    act.reserve(static_cast<size_t>(span));
    std::vector<uint64_t> miss(static_cast<size_t>(span), 0);

    for (int it = 0; it < iterations; ++it) {
        // Walking the finished words out of the inner loops rather than testing
        // them there matters once most of the population has landed: what is
        // left is a handful of words, and the gate pass should cost a handful
        // of words rather than the whole range.
        act.clear();
        for (int w = wordBegin; w < wordEnd; ++w) {
            if (bad[w]) act.push_back(w);
        }
        if (act.empty()) return;

        // Only the missing lanes are redrawn. Everything else keeps the values
        // that already reproduce the target, which is the whole reason this
        // converges instead of rolling the same dice over and over.
        for (Var v : net.freeVars) {
            if (fixedVals[static_cast<size_t>(v)] != 0) continue;
            uint64_t* row = sig + static_cast<size_t>(v) * static_cast<size_t>(words);
            for (int w : act) {
                const uint64_t m = bad[w];
                row[w] = (row[w] & ~m) | (rng.next() & m);
            }
        }

        uint32_t poll = 0;
        for (const Gate& g : net.gates) {
            if (++poll == kPollEvery) {
                poll = 0;
                // Giving up here leaves the redrawn lanes half executed, which
                // is harmless: they are still flagged, so the caller strikes
                // them from the valid mask rather than reading them back.
                if (cancelled && cancelled()) return;
            }
            const uint64_t* a =
                sig + static_cast<size_t>(litVar(g.in0)) * static_cast<size_t>(words);
            const uint64_t* b =
                sig + static_cast<size_t>(litVar(g.in1)) * static_cast<size_t>(words);
            uint64_t* o = sig + static_cast<size_t>(g.out) * static_cast<size_t>(words);
            const uint64_t na = litSign(g.in0) ? ~0ull : 0ull;
            const uint64_t nb = litSign(g.in1) ? ~0ull : 0ull;
            const uint64_t no = g.negOut ? ~0ull : 0ull;
            if (g.op == Gate::And) {
                for (int w : act) {
                    const uint64_t m = bad[w];
                    o[w] = (o[w] & ~m) | (((((a[w] ^ na) & (b[w] ^ nb)) ^ no)) & m);
                }
            } else {
                const uint64_t k = na ^ nb ^ no;
                for (int w : act) {
                    const uint64_t m = bad[w];
                    o[w] = (o[w] & ~m) | ((a[w] ^ b[w] ^ k) & m);
                }
            }
        }

        std::fill(miss.begin(), miss.end(), 0);
        for (Lit l : focus) {
            const uint64_t* row =
                sig + static_cast<size_t>(litVar(l)) * static_cast<size_t>(words);
            const bool neg = litSign(l) != 0;
            for (int w : act) {
                miss[static_cast<size_t>(w - wordBegin)] |= neg ? row[w] : ~row[w];
            }
        }
        // A lane only ever leaves the set: one that landed on the target stays
        // landed, since nothing touches it again.
        for (int w : act) bad[w] &= miss[static_cast<size_t>(w - wordBegin)];
    }
}

void Signatures::disable(int numVars, int words) {
    words_ = std::max(1, words);
    numVars_ = numVars;
    sig_.assign(static_cast<size_t>(numVars_) * static_cast<size_t>(words_), 0);
    valid_.assign(static_cast<size_t>(words_), 0);
    probeT_.clear();
    probeWords_ = 0;
    validSamples_ = 0;
    gateSampling_ = false;
    focusBits_ = 0;
    focusLanes_ = 0;
    focusRounds_ = 0;
    std::vector<uint64_t>().swap(focusBad_);
    std::vector<uint64_t>().swap(def_);
}

void Signatures::gateChunk(const Cnf& cnf, const GateNetwork& net,
                           const std::vector<int8_t>& fixedVals, int wordBegin, int wordEnd,
                           uint64_t seed, bool verify,
                           const std::function<bool()>& cancelled, std::atomic<bool>& aborted) {
    const int words = words_;
    uint64_t* sig = sig_.data();
    Rng rng(seed);

    // Executing the circuit is one straight pass with no natural checkpoint, so
    // the poll goes on a counter rather than in the inner loop, where the
    // indirect call through std::function would be most of the cost. The lanes
    // of an abandoned range are worthless either way, so it drops them.
    const uint32_t kPollEvery = 4096;
    auto giveUp = [&] {
        aborted.store(true);
        for (int w = wordBegin; w < wordEnd; ++w) valid_[static_cast<size_t>(w)] = 0;
    };

    // The free variables are the whole of the randomness; a fixed one is simply
    // not randomised, which is how a unit clause is honoured here.
    for (Var v : net.freeVars) {
        uint64_t* row = sig + static_cast<size_t>(v) * static_cast<size_t>(words);
        const int8_t f = fixedVals[static_cast<size_t>(v)];
        if (f == 0) {
            for (int w = wordBegin; w < wordEnd; ++w) row[w] = rng.next();
        } else {
            const uint64_t bits = f > 0 ? ~0ull : 0ull;
            for (int w = wordBegin; w < wordEnd; ++w) row[w] = bits;
        }
    }

    // One forward pass in topological order. Every lane of every variable is
    // decided by the time the pass ends, no lane can conflict, and nothing is
    // ever revisited - which is the whole difference to propagating clauses.
    uint32_t poll = 0;
    for (const Gate& g : net.gates) {
        if (++poll == kPollEvery) {
            poll = 0;
            if (cancelled && cancelled()) { giveUp(); return; }
        }
        const uint64_t* a = sig + static_cast<size_t>(litVar(g.in0)) * static_cast<size_t>(words);
        const uint64_t* b = sig + static_cast<size_t>(litVar(g.in1)) * static_cast<size_t>(words);
        uint64_t* o = sig + static_cast<size_t>(g.out) * static_cast<size_t>(words);
        const uint64_t na = litSign(g.in0) ? ~0ull : 0ull;
        const uint64_t nb = litSign(g.in1) ? ~0ull : 0ull;
        const uint64_t no = g.negOut ? ~0ull : 0ull;
        if (g.op == Gate::And) {
            for (int w = wordBegin; w < wordEnd; ++w) o[w] = ((a[w] ^ na) & (b[w] ^ nb)) ^ no;
        } else {
            // Every polarity flip of an XOR folds into a single constant.
            const uint64_t k = na ^ nb ^ no;
            for (int w = wordBegin; w < wordEnd; ++w) o[w] = a[w] ^ b[w] ^ k;
        }
    }

    // The network was accepted only because it accounts for every clause, so
    // this pass is a check of that claim rather than a filter: it should leave
    // every lane standing. Running it once keeps a mistake in the pattern
    // matching from quietly poisoning the sample population, which the solver
    // has no way to detect on its own.
    for (int w = wordBegin; w < wordEnd; ++w) valid_[static_cast<size_t>(w)] = ~0ull;
    if (!verify) return;

    const size_t nc = cnf.clauseCount();
    const uint32_t* cstart = cnf.start.data();
    const Lit* clits = cnf.lits.data();
    poll = 0;
    for (size_t c = 0; c < nc; ++c) {
        if (++poll == kPollEvery) {
            poll = 0;
            if (cancelled && cancelled()) { giveUp(); return; }
        }
        const uint32_t from = cstart[c];
        const uint32_t len = cstart[c + 1] - from;
        const Lit* b = clits + from;
        for (int w = wordBegin; w < wordEnd; ++w) {
            uint64_t sat = 0;
            for (uint32_t i = 0; i < len; ++i) {
                const Lit l = b[i];
                const uint64_t row =
                    sig[static_cast<size_t>(litVar(l)) * static_cast<size_t>(words) +
                        static_cast<size_t>(w)];
                sat |= (l & 1) ? ~row : row;
            }
            valid_[static_cast<size_t>(w)] &= sat;
        }
    }
}

void Signatures::generateChunk(const Cnf& cnf, const std::vector<Lit>& fixedLits,
                               const std::vector<Var>& inputVars, int maxRounds,
                               int wordBegin, int wordEnd, uint64_t seed,
                               const std::function<bool()>& cancelled) {
    const int words = words_;
    const size_t numClauses = cnf.clauseCount();
    const int span = wordEnd - wordBegin;

    Rng rng(seed);

    std::vector<uint64_t> pending(static_cast<size_t>(span), ~0ull);
    std::vector<uint64_t> conflict(static_cast<size_t>(span), 0);
    std::vector<uint64_t> active(static_cast<size_t>(span), 0);

    // Propagation is driven by dirty flags swept in clause index order rather
    // than by a FIFO worklist. Tseitin encoders emit clauses in topological
    // order, so a single forward sweep carries a change all the way through the
    // circuit; a worklist would instead revisit each clause once per lane group
    // that happens to resolve at a different depth, which costs an order of
    // magnitude more on deep instances such as reduced round SHA-256.
    std::vector<uint8_t> dirty(numClauses, 0);
    // A clause that is satisfied on every active lane can never force anything
    // again: within a round the assigned mask only grows. Retiring those keeps
    // the later sweeps from re-reading the whole formula.
    std::vector<uint8_t> retired(numClauses, 0);
    // Bounds of the dirty region, so a sweep triggered by a single variable does
    // not walk the whole clause array. They are also the only record of what is
    // still queued: a sweep runs the range it captured, and whatever the sweep
    // itself dirties reopens the range for the next one.
    const uint32_t clauseCount = static_cast<uint32_t>(numClauses);
    uint32_t dirtyLo = clauseCount;
    uint32_t dirtyHi = 0;
    std::vector<uint8_t> varDirty(static_cast<size_t>(numVars_), 0);
    std::vector<Var> dirtyList;
    dirtyList.reserve(32);

    const uint32_t maxLen = std::max<uint32_t>(cnf.maxClauseLen, 1);
    std::vector<uint64_t> fmask(maxLen), pre(maxLen), suf(maxLen);

    uint64_t* sig = sig_.data();
    uint64_t* def = def_.data();
    const uint32_t* occStart = cnf.occStart.data();
    const uint32_t* occ = cnf.occ.data();
    const uint32_t* cstart = cnf.start.data();
    const Lit* clits = cnf.lits.data();

    auto pushClausesOf = [&](Var v) {
        for (int s = 0; s < 2; ++s) {
            const size_t l = static_cast<size_t>((v << 1) | s);
            for (uint32_t i = occStart[l], e = occStart[l + 1]; i < e; ++i) {
                const uint32_t c = occ[i];
                if (dirty[c] || retired[c]) continue;
                dirty[c] = 1;
                if (c < dirtyLo) dirtyLo = c;
                if (c > dirtyHi) dirtyHi = c;
            }
        }
    };

    // Marks a variable as changed; clauses are queued once the current clause
    // has been fully processed so the queue stays coherent.
    auto markDirty = [&](Var v) {
        if (!varDirty[static_cast<size_t>(v)]) {
            varDirty[static_cast<size_t>(v)] = 1;
            dirtyList.push_back(v);
        }
    };

    auto flushDirty = [&]() {
        for (Var v : dirtyList) {
            varDirty[static_cast<size_t>(v)] = 0;
            pushClausesOf(v);
        }
        dirtyList.clear();
    };

    // Assigns `bits` (restricted to the active, still undefined lanes) of a
    // variable to the polarity carried by `valueBits`.
    auto assignBits = [&](Var v, int w, uint64_t mask, uint64_t valueBits) {
        if (!mask) return false;
        const size_t idx = static_cast<size_t>(v) * static_cast<size_t>(words) + static_cast<size_t>(w);
        def[idx] |= mask;
        sig[idx] |= valueBits & mask;
        return true;
    };

    // Returns false when the caller asked to stop; the round is then abandoned
    // without committing, since its lanes are only partially assigned.
    auto runQueue = [&]() {
      while (dirtyLo <= dirtyHi) {
        if (cancelled && cancelled()) return false;
        const uint32_t lo = dirtyLo;
        const uint32_t hi = dirtyHi;
        dirtyLo = clauseCount;
        dirtyHi = 0;
        for (uint32_t c = lo; c <= hi; ++c) {
            if (!dirty[c]) continue;
            dirty[c] = 0;
            const uint32_t len = cstart[c + 1] - cstart[c];
            if (len < 2) continue;  // units are handled through the fixed literals
            const Lit* b = clits + cstart[c];

            uint64_t stillOpen = 0;
            for (int w = wordBegin; w < wordEnd; ++w) {
                const uint64_t act = active[static_cast<size_t>(w - wordBegin)];
                if (!act) continue;

                uint64_t anyTrue = 0;
                uint64_t allFalse = ~0ull;
                for (uint32_t i = 0; i < len; ++i) {
                    const Lit l = b[i];
                    const size_t idx =
                        static_cast<size_t>(l >> 1) * static_cast<size_t>(words) + static_cast<size_t>(w);
                    const uint64_t d = def[idx];
                    const uint64_t val = sig[idx];
                    const uint64_t t = (l & 1) ? (d & ~val) : (d & val);
                    fmask[i] = (l & 1) ? (d & val) : (d & ~val);
                    anyTrue |= t;
                    allFalse &= fmask[i];
                }
                if (allFalse & act) {
                    conflict[static_cast<size_t>(w - wordBegin)] |= allFalse & act;
                }
                const uint64_t notSat = ~anyTrue & act;
                if (!notSat) continue;
                stillOpen |= notSat;

                pre[0] = ~0ull;
                for (uint32_t i = 1; i < len; ++i) pre[i] = pre[i - 1] & fmask[i - 1];
                suf[len - 1] = ~0ull;
                for (uint32_t i = len - 1; i-- > 0;) suf[i] = suf[i + 1] & fmask[i + 1];

                for (uint32_t i = 0; i < len; ++i) {
                    const uint64_t others = pre[i] & suf[i] & notSat;
                    if (!others) continue;
                    const Lit l = b[i];
                    const size_t idx =
                        static_cast<size_t>(l >> 1) * static_cast<size_t>(words) + static_cast<size_t>(w);
                    const uint64_t force = others & ~def[idx];
                    if (!force) continue;
                    def[idx] |= force;
                    if (!(l & 1)) sig[idx] |= force;
                    markDirty(l >> 1);
                }
            }
            if (!stillOpen) retired[c] = 1;
            flushDirty();
        }
      }
      // The loop only ends with an empty range, and every clause the sweep
      // dirtied widened that range, so no dirty flag survives here - which
      // matters, because a clause left flagged would be skipped by
      // pushClausesOf for the rest of the round. Termination holds because a
      // clause is only requeued after a variable was newly forced, and the
      // assigned mask grows monotonically inside a round.
      return true;
    };

    // A round can only commit lanes that are active and have not conflicted.
    // Once that set is empty the rest of the round is pure cost: every variable
    // still to be assigned would be randomised and propagated for a population
    // that cannot take any of it. On an instance with no driving input set this
    // is the difference between paying for the whole sweep and paying for the
    // handful of variables it takes to kill every lane.
    auto anyLaneAlive = [&]() {
        for (int i = 0; i < span; ++i) {
            if (active[static_cast<size_t>(i)] & ~conflict[static_cast<size_t>(i)]) return true;
        }
        return false;
    };

    for (int round = 0; round < maxRounds; ++round) {
        if (round > 0 && cancelled && cancelled()) break;
        bool anyPending = false;
        for (int i = 0; i < span; ++i) {
            active[static_cast<size_t>(i)] = pending[static_cast<size_t>(i)];
            conflict[static_cast<size_t>(i)] = 0;
            if (pending[static_cast<size_t>(i)]) anyPending = true;
        }
        if (!anyPending) break;

        // Wipe the active lanes; lanes already committed keep their values and,
        // being fully defined, never take part in propagation again.
        for (Var v = 0; v < numVars_; ++v) {
            uint64_t* srow = sig + static_cast<size_t>(v) * static_cast<size_t>(words);
            uint64_t* drow = def + static_cast<size_t>(v) * static_cast<size_t>(words);
            for (int w = wordBegin; w < wordEnd; ++w) {
                const uint64_t keep = ~active[static_cast<size_t>(w - wordBegin)];
                srow[w] &= keep;
                drow[w] &= keep;
            }
        }

        std::fill(dirty.begin(), dirty.end(), 0);
        std::fill(retired.begin(), retired.end(), 0);
        dirtyLo = clauseCount;
        dirtyHi = 0;

        for (Lit l : fixedLits) {
            const Var v = l >> 1;
            bool touched = false;
            for (int w = wordBegin; w < wordEnd; ++w) {
                const uint64_t act = active[static_cast<size_t>(w - wordBegin)];
                const size_t idx =
                    static_cast<size_t>(v) * static_cast<size_t>(words) + static_cast<size_t>(w);
                const uint64_t mask = act & ~def[idx];
                touched |= assignBits(v, w, mask, (l & 1) ? 0ull : ~0ull);
            }
            if (touched) pushClausesOf(v);
        }
        if (!runQueue()) break;

        // The detected input set is assigned in one go, which costs a single
        // propagation sweep instead of one per input. It is only valid when the
        // set really is independent, so lanes that conflicted fall back to the
        // incremental sweep below in the following rounds.
        static const std::vector<Var> kNoInputs;
        for (Var v : (round == 0 ? inputVars : kNoInputs)) {
            if (v < 0 || v >= numVars_) continue;
            bool touched = false;
            for (int w = wordBegin; w < wordEnd; ++w) {
                const uint64_t act = active[static_cast<size_t>(w - wordBegin)];
                const size_t idx =
                    static_cast<size_t>(v) * static_cast<size_t>(words) + static_cast<size_t>(w);
                const uint64_t mask = act & ~def[idx];
                touched |= assignBits(v, w, mask, rng.next());
            }
            if (touched) pushClausesOf(v);
        }
        if (!runQueue()) break;

        // One sweep is enough: the assigned mask only ever grows inside a round,
        // so once the cursor has passed a variable it stays fully defined.
        bool aborted = false;
        for (Var v = 0; v < numVars_; ++v) {
            // On an instance where propagation drives nothing this sweep is the
            // whole cost of a round, so it has to be interruptible too.
            bool touched = false;
            for (int w = wordBegin; w < wordEnd; ++w) {
                const uint64_t act = active[static_cast<size_t>(w - wordBegin)];
                if (!act) continue;
                const size_t idx =
                    static_cast<size_t>(v) * static_cast<size_t>(words) + static_cast<size_t>(w);
                const uint64_t mask = act & ~def[idx];
                touched |= assignBits(v, w, mask, rng.next());
            }
            // Without this the sweep below has an empty queue and the value just
            // written is never propagated - and, worse, the clauses it falsifies
            // are never looked at, so the lane commits as valid. On an instance
            // whose inputs drive everything the loop assigns nothing and this
            // costs nothing; where it does assign, it is the only thing standing
            // between a random valuation and the sample population.
            if (!touched) continue;
            pushClausesOf(v);
            if (!runQueue()) { aborted = true; break; }
            if (!anyLaneAlive()) break;
        }

        if (aborted) break;  // the lanes of this round stay invalid

        uint64_t committed = 0;
        uint64_t attempted = 0;
        for (int i = 0; i < span; ++i) {
            const uint64_t good = active[static_cast<size_t>(i)] & ~conflict[static_cast<size_t>(i)];
            attempted += popcount64(active[static_cast<size_t>(i)]);
            committed += popcount64(good);
            pending[static_cast<size_t>(i)] &= ~good;
            valid_[static_cast<size_t>(wordBegin + i)] |= good;
        }

        // Instances whose variables are not determined by a set of free inputs
        // (random k-SAT, for instance) conflict on nearly every lane. Retrying
        // them costs a full propagation sweep for a handful of samples, so give
        // up once the yield collapses.
        if (round >= 1 && committed * 64 < attempted) break;
    }
}

}  // namespace tcs
