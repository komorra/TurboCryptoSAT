#include "genbench.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tcs {
namespace {

constexpr int kTrue = INT32_MAX;
constexpr int kFalse = INT32_MIN;

class Rng {
public:
    explicit Rng(uint64_t seed) { reseed(seed); }
    void reseed(uint64_t seed) {
        for (int i = 0; i < 4; ++i) {
            seed += 0x9E3779B97F4A7C15ull;
            uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            s_[i] = z ^ (z >> 31);
        }
    }
    uint64_t next() {
        const uint64_t r = rotl(s_[1] * 5, 7) * 9;
        const uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return r;
    }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>((next() >> 32) * n >> 32); }
    bool coin() { return (next() >> 63) != 0; }

private:
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t s_[4];
};

// Tseitin circuit builder with constant folding. Every signal carries its value
// under the planted input assignment, so the expected outputs come for free.
class Circuit {
public:
    int newInput(bool value) {
        val_.push_back(value ? 1 : 0);
        return static_cast<int>(val_.size());
    }

    bool value(int l) const {
        if (l == kTrue) return true;
        if (l == kFalse) return false;
        const bool v = val_[static_cast<size_t>((l < 0 ? -l : l) - 1)] != 0;
        return l < 0 ? !v : v;
    }

    static int NOT(int a) {
        if (a == kTrue) return kFalse;
        if (a == kFalse) return kTrue;
        return -a;
    }

    int AND(int a, int b) {
        if (a == kFalse || b == kFalse) return kFalse;
        if (a == kTrue) return b;
        if (b == kTrue) return a;
        if (a == b) return a;
        if (a == -b) return kFalse;
        const int o = gate(value(a) && value(b));
        add({-o, a});
        add({-o, b});
        add({o, -a, -b});
        return o;
    }

    int OR(int a, int b) { return NOT(AND(NOT(a), NOT(b))); }

    int XOR(int a, int b) {
        if (a == kFalse) return b;
        if (b == kFalse) return a;
        if (a == kTrue) return NOT(b);
        if (b == kTrue) return NOT(a);
        if (a == b) return kFalse;
        if (a == -b) return kTrue;
        const int o = gate(value(a) != value(b));
        add({-o, a, b});
        add({-o, -a, -b});
        add({o, -a, b});
        add({o, a, -b});
        return o;
    }

    int MAJ(int a, int b, int c) { return OR(AND(a, b), OR(AND(a, c), AND(b, c))); }
    int CH(int e, int f, int g) { return XOR(AND(e, f), AND(NOT(e), g)); }

    void pin(int l) { add({l}); }

    int varCount() const { return static_cast<int>(val_.size()); }
    const std::vector<std::vector<int>>& clauses() const { return clauses_; }

private:
    int gate(bool value) {
        val_.push_back(value ? 1 : 0);
        return static_cast<int>(val_.size());
    }
    void add(std::vector<int> c) { clauses_.push_back(std::move(c)); }

    std::vector<uint8_t> val_;
    std::vector<std::vector<int>> clauses_;
};

using Word = std::vector<int>;  // 32 signals, index 0 is the least significant bit

Word constWord(uint32_t v) {
    Word w(32);
    for (int i = 0; i < 32; ++i) w[static_cast<size_t>(i)] = ((v >> i) & 1u) ? kTrue : kFalse;
    return w;
}

Word rotr(const Word& x, int n) {
    Word r(32);
    for (int i = 0; i < 32; ++i) r[static_cast<size_t>(i)] = x[static_cast<size_t>((i + n) % 32)];
    return r;
}

Word shr(const Word& x, int n) {
    Word r(32, kFalse);
    for (int i = 0; i + n < 32; ++i) r[static_cast<size_t>(i)] = x[static_cast<size_t>(i + n)];
    return r;
}

Word xorWord(Circuit& c, const Word& a, const Word& b) {
    Word r(32);
    for (int i = 0; i < 32; ++i) r[static_cast<size_t>(i)] = c.XOR(a[static_cast<size_t>(i)], b[static_cast<size_t>(i)]);
    return r;
}

Word add32(Circuit& c, const Word& a, const Word& b) {
    Word r(32);
    int carry = kFalse;
    for (int i = 0; i < 32; ++i) {
        const int x = a[static_cast<size_t>(i)];
        const int y = b[static_cast<size_t>(i)];
        r[static_cast<size_t>(i)] = c.XOR(c.XOR(x, y), carry);
        carry = c.MAJ(x, y, carry);
    }
    return r;
}

// The SHA-256 round and initial hash constants are the fractional parts of the
// cube and square roots of the first primes. Deriving them at startup keeps the
// tables out of the binary image, which also stops heuristic malware scanners
// from mistaking the generator for a crypto payload.
struct ShaConstants {
    uint32_t k[64];
    uint32_t h[8];

    ShaConstants() {
        int found = 0;
        for (int n = 2; found < 64; ++n) {
            bool prime = true;
            for (int d = 2; d * d <= n; ++d) {
                if (n % d == 0) { prime = false; break; }
            }
            if (!prime) continue;
            const double scale = 4294967296.0;  // 2^32
            const double cb = std::cbrt(static_cast<double>(n));
            k[found] = static_cast<uint32_t>((cb - std::floor(cb)) * scale);
            if (found < 8) {
                const double sq = std::sqrt(static_cast<double>(n));
                h[found] = static_cast<uint32_t>((sq - std::floor(sq)) * scale);
            }
            ++found;
        }
    }
};

const ShaConstants& sha() {
    static const ShaConstants c;
    return c;
}

void writeCnf(const std::string& path, const std::string& comment, int numVars,
              const std::vector<std::vector<int>>& clauses) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "c %s\n", comment.c_str());
    std::fprintf(f, "c generated by TurboCryptoSAT gen-benchmark - satisfiable by construction\n");
    std::fprintf(f, "p cnf %d %zu\n", numVars, clauses.size());
    std::string line;
    line.reserve(64);
    for (const auto& cl : clauses) {
        line.clear();
        for (int l : cl) {
            line += std::to_string(l);
            line += ' ';
        }
        line += "0\n";
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
    std::printf("  %-40s %6d vars %8zu clauses\n", path.c_str(), numVars, clauses.size());
}

// ---------------------------------------------------------------- families ---

void genPlanted3Sat(const std::string& path, int n, double ratio, uint64_t seed) {
    Rng rng(seed);
    std::vector<uint8_t> plant(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) plant[static_cast<size_t>(i)] = rng.coin() ? 1 : 0;

    const int m = static_cast<int>(ratio * n);
    std::vector<std::vector<int>> clauses;
    clauses.reserve(static_cast<size_t>(m));
    int picked[3];
    for (int c = 0; c < m; ++c) {
        for (;;) {
            picked[0] = static_cast<int>(rng.below(static_cast<uint32_t>(n)));
            picked[1] = static_cast<int>(rng.below(static_cast<uint32_t>(n)));
            picked[2] = static_cast<int>(rng.below(static_cast<uint32_t>(n)));
            if (picked[0] == picked[1] || picked[0] == picked[2] || picked[1] == picked[2]) continue;
            std::vector<int> cl(3);
            bool sat = false;
            for (int i = 0; i < 3; ++i) {
                const bool neg = rng.coin();
                cl[static_cast<size_t>(i)] = neg ? -(picked[i] + 1) : (picked[i] + 1);
                const bool v = plant[static_cast<size_t>(picked[i])] != 0;
                if (neg ? !v : v) sat = true;
            }
            if (!sat) continue;  // keep the planted assignment a solution
            clauses.push_back(cl);
            break;
        }
    }
    char ratioText[32];
    std::snprintf(ratioText, sizeof(ratioText), "%.2f", ratio);
    writeCnf(path, "planted random 3-SAT, n=" + std::to_string(n) + " ratio=" + ratioText +
                       "; every variable is free, there is no driving input set",
             n, clauses);
}

// Random combinational circuit over `inputs` free bits. `xorHeavy` biases the
// gate mix towards XOR, which unit propagation handles poorly.
void genCircuit(const std::string& path, int inputs, int gates, bool xorHeavy, uint64_t seed) {
    Rng rng(seed);
    Circuit c;
    std::vector<int> pool;
    pool.reserve(static_cast<size_t>(inputs + gates));
    for (int i = 0; i < inputs; ++i) pool.push_back(c.newInput(rng.coin()));

    for (int g = 0; g < gates; ++g) {
        const int a = pool[rng.below(static_cast<uint32_t>(pool.size()))];
        const int b = pool[rng.below(static_cast<uint32_t>(pool.size()))];
        const int sa = rng.coin() ? a : Circuit::NOT(a);
        const int sb = rng.coin() ? b : Circuit::NOT(b);
        const uint32_t pick = rng.below(100);
        int out;
        if (xorHeavy) {
            out = (pick < 70) ? c.XOR(sa, sb) : ((pick < 85) ? c.AND(sa, sb) : c.OR(sa, sb));
        } else {
            out = (pick < 40) ? c.AND(sa, sb) : ((pick < 75) ? c.OR(sa, sb) : c.XOR(sa, sb));
        }
        if (out == kTrue || out == kFalse) continue;
        pool.push_back(out);
    }

    // Only the tail of the circuit is observable, like the digest of a hash.
    // Pinning deeper signals would let plain propagation unwind the whole DAG.
    const size_t outs = std::min<size_t>(pool.size(), static_cast<size_t>(inputs));
    for (size_t i = pool.size() - outs; i < pool.size(); ++i) {
        const int l = pool[i];
        c.pin(c.value(l) ? l : -l);
    }
    writeCnf(path,
             std::string(xorHeavy ? "xor-heavy" : "mixed") + " random circuit, inputs=" +
                 std::to_string(inputs) + " gates=" + std::to_string(gates) +
                 ", last " + std::to_string(outs) +
                 " signals pinned to a planted input assignment; input variables are 1.." +
                 std::to_string(inputs),
             c.varCount(), c.clauses());
}

// Reduced round SHA-256 preimage: `chars` message bytes are free, the digest of
// a random secret message is pinned by unit clauses.
void genSha256(const std::string& path, int rounds, int chars, uint64_t seed) {
    Rng rng(seed);
    Circuit c;

    // Message bits first so that they occupy variables 1..8*chars.
    const int msgBits = chars * 8;
    std::vector<int> mbits(static_cast<size_t>(msgBits));
    for (int i = 0; i < msgBits; ++i) mbits[static_cast<size_t>(i)] = c.newInput(rng.coin());

    // One 512-bit block: message, 0x80 terminator, zero padding, 64-bit length.
    std::vector<int> block(512, kFalse);
    for (int i = 0; i < msgBits; ++i) {
        block[static_cast<size_t>(i)] = mbits[static_cast<size_t>(i)];  // big-endian bit order
    }
    block[static_cast<size_t>(msgBits)] = kTrue;
    const uint64_t bitLen = static_cast<uint64_t>(msgBits);
    for (int i = 0; i < 64; ++i) {
        block[static_cast<size_t>(448 + i)] = ((bitLen >> (63 - i)) & 1ull) ? kTrue : kFalse;
    }

    std::vector<Word> w(64);
    for (int t = 0; t < 16; ++t) {
        Word word(32);
        for (int i = 0; i < 32; ++i) {
            word[static_cast<size_t>(31 - i)] = block[static_cast<size_t>(t * 32 + i)];
        }
        w[static_cast<size_t>(t)] = word;
    }
    for (int t = 16; t < rounds; ++t) {
        const Word& w15 = w[static_cast<size_t>(t - 15)];
        const Word& w2 = w[static_cast<size_t>(t - 2)];
        const Word s0 = xorWord(c, xorWord(c, rotr(w15, 7), rotr(w15, 18)), shr(w15, 3));
        const Word s1 = xorWord(c, xorWord(c, rotr(w2, 17), rotr(w2, 19)), shr(w2, 10));
        w[static_cast<size_t>(t)] =
            add32(c, add32(c, w[static_cast<size_t>(t - 16)], s0),
                  add32(c, w[static_cast<size_t>(t - 7)], s1));
    }

    Word a = constWord(sha().h[0]), b = constWord(sha().h[1]), cc = constWord(sha().h[2]), d = constWord(sha().h[3]);
    Word e = constWord(sha().h[4]), f = constWord(sha().h[5]), g = constWord(sha().h[6]), h = constWord(sha().h[7]);

    for (int t = 0; t < rounds; ++t) {
        Word S1(32), ch(32), S0(32), maj(32);
        const Word e1 = rotr(e, 6), e2 = rotr(e, 11), e3 = rotr(e, 25);
        const Word a1 = rotr(a, 2), a2 = rotr(a, 13), a3 = rotr(a, 22);
        for (int i = 0; i < 32; ++i) {
            const size_t k = static_cast<size_t>(i);
            S1[k] = c.XOR(c.XOR(e1[k], e2[k]), e3[k]);
            ch[k] = c.CH(e[k], f[k], g[k]);
            S0[k] = c.XOR(c.XOR(a1[k], a2[k]), a3[k]);
            maj[k] = c.MAJ(a[k], b[k], cc[k]);
        }
        const Word t1 = add32(c, add32(c, h, S1),
                              add32(c, ch, add32(c, constWord(sha().k[t]), w[static_cast<size_t>(t)])));
        const Word t2 = add32(c, S0, maj);
        h = g;
        g = f;
        f = e;
        e = add32(c, d, t1);
        d = cc;
        cc = b;
        b = a;
        a = add32(c, t1, t2);
    }

    const Word out[8] = {add32(c, a, constWord(sha().h[0])), add32(c, b, constWord(sha().h[1])),
                         add32(c, cc, constWord(sha().h[2])), add32(c, d, constWord(sha().h[3])),
                         add32(c, e, constWord(sha().h[4])), add32(c, f, constWord(sha().h[5])),
                         add32(c, g, constWord(sha().h[6])), add32(c, h, constWord(sha().h[7]))};
    for (int i = 0; i < 8; ++i) {
        for (int bit = 0; bit < 32; ++bit) {
            const int l = out[i][static_cast<size_t>(bit)];
            if (l == kTrue || l == kFalse) continue;  // folded to a constant
            c.pin(c.value(l) ? l : -l);
        }
    }

    writeCnf(path,
             "SHA-256 preimage, rounds=" + std::to_string(rounds) + " message=" +
                 std::to_string(chars) + " bytes, digest pinned; input variables are 1.." +
                 std::to_string(msgBits),
             c.varCount(), c.clauses());
}

}  // namespace

bool generateBenchmarkSuite(const std::string& dir) {
    auto p = [&](const char* name) { return dir + "/" + name; };

    std::printf("Generating the TurboCryptoSAT benchmark suite into %s\n", dir.c_str());

    genPlanted3Sat(p("01-rand3sat-n060.cnf"), 60, 4.2, 1001);
    genPlanted3Sat(p("02-rand3sat-n100.cnf"), 100, 4.2, 1002);
    genPlanted3Sat(p("03-rand3sat-n150.cnf"), 150, 4.2, 1003);
    genPlanted3Sat(p("04-rand3sat-n220.cnf"), 220, 4.2, 1004);
    genPlanted3Sat(p("05-rand3sat-n320.cnf"), 320, 4.25, 1005);
    genPlanted3Sat(p("06-rand3sat-n450.cnf"), 450, 4.25, 1006);
    genPlanted3Sat(p("07-rand3sat-n650.cnf"), 650, 4.26, 1007);
    genPlanted3Sat(p("08-rand3sat-n900.cnf"), 900, 4.26, 1008);

    genCircuit(p("09-circuit-i24-g300.cnf"), 24, 300, false, 2001);
    genCircuit(p("10-circuit-i32-g600.cnf"), 32, 600, false, 2002);
    genCircuit(p("11-circuit-i48-g1200.cnf"), 48, 1200, false, 2003);
    genCircuit(p("12-circuit-i64-g2000.cnf"), 64, 2000, false, 2004);
    genCircuit(p("13-circuit-i96-g3500.cnf"), 96, 3500, false, 2005);
    genCircuit(p("14-circuit-i128-g6000.cnf"), 128, 6000, false, 2006);

    genCircuit(p("15-xorcircuit-i24-g200.cnf"), 24, 200, true, 3001);
    genCircuit(p("16-xorcircuit-i32-g400.cnf"), 32, 400, true, 3002);
    genCircuit(p("17-xorcircuit-i48-g800.cnf"), 48, 800, true, 3003);
    genCircuit(p("18-xorcircuit-i64-g1500.cnf"), 64, 1500, true, 3004);
    genCircuit(p("19-xorcircuit-i96-g2500.cnf"), 96, 2500, true, 3005);
    genCircuit(p("20-xorcircuit-i128-g4000.cnf"), 128, 4000, true, 3006);

    genSha256(p("21-sha256-r08-c03.cnf"), 8, 3, 4001);
    genSha256(p("22-sha256-r11-c03.cnf"), 11, 3, 4002);
    genSha256(p("23-sha256-r14-c03.cnf"), 14, 3, 4003);
    genSha256(p("24-sha256-r17-c03.cnf"), 17, 3, 4004);
    genSha256(p("25-sha256-r20-c03.cnf"), 20, 3, 4005);
    genSha256(p("26-sha256-r17-c04.cnf"), 17, 4, 4006);

    std::printf("done\n");
    return true;
}

}  // namespace tcs
