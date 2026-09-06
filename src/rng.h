// xoshiro256** - small, fast, allocation free PRNG used everywhere in the solver.
#pragma once

#include <cstdint>

namespace tcs {

class Rng {
public:
    explicit Rng(uint64_t seed = 0x2545F4914F6CDD1Dull) { reseed(seed); }

    void reseed(uint64_t seed) {
        // SplitMix64 expansion so that nearby seeds still diverge quickly.
        for (int i = 0; i < 4; ++i) {
            seed += 0x9E3779B97F4A7C15ull;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            s_[i] = z ^ (z >> 31);
        }
    }

    uint64_t next() {
        const uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, bound); bound must be positive.
    uint32_t below(uint32_t bound) {
        return static_cast<uint32_t>((next() >> 32) * bound >> 32);
    }

    bool coin() { return (next() >> 63) != 0; }

private:
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t s_[4];
};

}  // namespace tcs
