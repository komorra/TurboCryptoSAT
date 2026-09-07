#include "gf2.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace tcs {

namespace {

// A clause forbids exactly one assignment of its variables: the one making
// every literal false. So the 2^(n-1) clauses over the same n variables whose
// sign patterns all have the same parity forbid every assignment of that
// parity, which says precisely that the variables sum to the other one.
//
// n = 2 is `x = y` / `x = ~y` written as two binary clauses, n = 3 is what a
// Tseitin XOR gate emits. Larger n costs 2^(n-1) clauses to state and is rare
// in practice, so the scan stops at 4.
constexpr uint32_t kMaxXorLen = 4;

// Lowest set bit of `row` at or above column `from`, or `n` when there is none.
size_t nextSetBit(const uint64_t* row, size_t n, size_t from) {
    if (from >= n) return n;
    size_t w = from >> 6;
    uint64_t bits = row[w] & ~((1ull << (from & 63)) - 1ull);
    for (;;) {
        if (bits) {
            const size_t c = w * 64 + ctz64(bits);
            return c < n ? c : n;
        }
        if (++w >= (n + 63) / 64) return n;
        bits = row[w];
    }
}

struct Group {
    uint8_t len = 0;
    uint8_t signs = 0;   // bit i set when the i-th variable occurs negated
    Var v[kMaxXorLen] = {0, 0, 0, 0};
};

bool groupLess(const Group& a, const Group& b) {
    if (a.len != b.len) return a.len < b.len;
    for (uint32_t i = 0; i < kMaxXorLen; ++i) {
        if (a.v[i] != b.v[i]) return a.v[i] < b.v[i];
    }
    return a.signs < b.signs;
}

bool sameVars(const Group& a, const Group& b) {
    if (a.len != b.len) return false;
    for (uint32_t i = 0; i < a.len; ++i) {
        if (a.v[i] != b.v[i]) return false;
    }
    return true;
}

}  // namespace

void Gf2System::build(const Cnf& cnf) {
    rows_.clear();
    vars_.clear();
    cols_.clear();
    clausesUsed_ = 0;
    colOf_.assign(static_cast<size_t>(cnf.numVars), -1);

    std::vector<Group> groups;
    groups.reserve(cnf.clauseCount());

    const size_t nc = cnf.clauseCount();
    for (size_t c = 0; c < nc; ++c) {
        const uint32_t len = cnf.clauseLen(c);
        if (len < 2 || len > kMaxXorLen) continue;
        const Lit* b = cnf.clauseBegin(c);
        Group g;
        g.len = static_cast<uint8_t>(len);
        uint8_t sign[kMaxXorLen];
        for (uint32_t i = 0; i < len; ++i) {
            g.v[i] = litVar(b[i]);
            sign[i] = litSign(b[i]) ? 1u : 0u;
        }
        // Sort the variables, carrying polarity along, so clauses over the same
        // variables land next to each other whatever order the file wrote them.
        for (uint32_t i = 1; i < len; ++i) {
            for (uint32_t j = i; j > 0 && g.v[j - 1] > g.v[j]; --j) {
                std::swap(g.v[j - 1], g.v[j]);
                std::swap(sign[j - 1], sign[j]);
            }
        }
        bool dup = false;
        for (uint32_t i = 1; i < len; ++i) dup = dup || g.v[i - 1] == g.v[i];
        if (dup) continue;  // fewer distinct variables than literals
        g.signs = 0;
        for (uint32_t i = 0; i < len; ++i) {
            g.signs = static_cast<uint8_t>(g.signs | (sign[i] << i));
        }
        groups.push_back(g);
    }

    std::sort(groups.begin(), groups.end(), groupLess);

    size_t i = 0;
    while (i < groups.size()) {
        size_t j = i + 1;
        while (j < groups.size() && sameVars(groups[i], groups[j])) ++j;

        const uint32_t len = groups[i].len;
        const uint32_t patterns = 1u << len;
        const uint32_t needed = patterns / 2;
        if (j - i >= needed) {
            // Which sign patterns are present, and of which parity. Duplicate
            // clauses count once.
            uint32_t seenOdd = 0, seenEven = 0;
            uint32_t maskOdd = 0, maskEven = 0;
            for (size_t k = i; k < j; ++k) {
                const uint8_t p = groups[k].signs;
                const bool odd = (popcount64(p) & 1u) != 0;
                const uint32_t bit = 1u << p;
                if (odd) {
                    if (!(maskOdd & bit)) { maskOdd |= bit; ++seenOdd; }
                } else {
                    if (!(maskEven & bit)) { maskEven |= bit; ++seenEven; }
                }
            }
            // Every odd pattern forbidden means no assignment of odd parity is
            // allowed, so the variables sum to 0; every even pattern forbidden
            // means they sum to 1.
            int rhs = -1;
            if (seenOdd == needed) rhs = 0;
            else if (seenEven == needed) rhs = 1;

            if (rhs >= 0) {
                Row r;
                r.begin = static_cast<uint32_t>(vars_.size());
                for (uint32_t k = 0; k < len; ++k) {
                    const Var v = groups[i].v[k];
                    vars_.push_back(v);
                    if (colOf_[static_cast<size_t>(v)] < 0) {
                        colOf_[static_cast<size_t>(v)] = static_cast<int32_t>(cols_.size());
                        cols_.push_back(v);
                    }
                }
                r.end = static_cast<uint32_t>(vars_.size());
                r.rhs = static_cast<uint8_t>(rhs);
                rows_.push_back(r);
                clausesUsed_ += needed;
            }
        }
        i = j;
    }

    words_ = (cols_.size() + 63) / 64;
    work_.clear();
    workRhs_.clear();
    pivotOfCol_.clear();
    pivotCols_.clear();
}

void Gf2System::solve(const std::vector<int8_t>& values, Gf2Result& out) {
    out.conflict = false;
    out.units.clear();
    out.equivA.clear();
    out.equivB.clear();
    if (rows_.empty() || words_ == 0) return;

    work_.assign(rows_.size() * words_, 0ull);
    workRhs_.assign(rows_.size(), 0);
    pivotOfCol_.assign(cols_.size(), -1);
    pivotCols_.clear();

    // Substitute what is already known. A variable with a value drops out of
    // the row and flips the right hand side when it is true.
    size_t live = 0;
    for (const Row& r : rows_) {
        uint64_t* dst = work(live);
        uint8_t rhs = r.rhs;
        uint32_t n = 0;
        for (uint32_t k = r.begin; k < r.end; ++k) {
            const Var v = vars_[k];
            const int8_t val = values[static_cast<size_t>(v)];
            if (val == 0) {
                const int32_t c = colOf_[static_cast<size_t>(v)];
                dst[static_cast<size_t>(c) >> 6] ^= 1ull << (static_cast<size_t>(c) & 63);
                ++n;
            } else if (val > 0) {
                rhs ^= 1u;
            }
        }
        if (n == 0) {
            // Fully determined: it either holds or the assignment is refuted.
            if (rhs) { out.conflict = true; return; }
            continue;  // the slot stays all zero and is reused by the next row
        }
        workRhs_[live] = rhs;
        ++live;
    }

    // Forward elimination. Each row is reduced against the pivots installed so
    // far and then becomes the pivot for its own lowest remaining column, which
    // leaves the system in row echelon form.
    size_t kept = 0;
    for (size_t r = 0; r < live; ++r) {
        uint64_t* row = work(r);
        uint8_t rhs = workRhs_[r];
        for (;;) {
            const size_t c = nextSetBit(row, cols_.size(), 0);
            if (c >= cols_.size()) {
                // Reduced to nothing: consistent, or a refutation of the input.
                if (rhs) { out.conflict = true; return; }
                break;
            }
            const int32_t p = pivotOfCol_[c];
            if (p < 0) {
                pivotOfCol_[c] = static_cast<int32_t>(kept);
                pivotCols_.push_back(static_cast<int32_t>(c));
                if (static_cast<size_t>(kept) != r) {
                    std::memcpy(work(kept), row, words_ * sizeof(uint64_t));
                }
                workRhs_[kept] = rhs;
                ++kept;
                break;
            }
            const uint64_t* pr = work(static_cast<size_t>(p));
            for (size_t w = 0; w < words_; ++w) row[w] ^= pr[w];
            rhs ^= workRhs_[static_cast<size_t>(p)];
        }
    }

    // Back substitution to reduced row echelon form. Pivots are processed from
    // the highest column down, so by the time a row is reached every pivot row
    // it could still mention has itself been reduced to its own pivot column
    // plus free columns only - which is why one forward scan over the set bits
    // suffices and no bit examined here can reappear.
    std::sort(pivotCols_.begin(), pivotCols_.end(), std::greater<int32_t>());
    for (int32_t pc : pivotCols_) {
        const size_t r = static_cast<size_t>(pivotOfCol_[static_cast<size_t>(pc)]);
        uint64_t* row = work(r);
        uint8_t rhs = workRhs_[r];
        // Every pivot above this one is already reduced: it holds its own pivot
        // column and free columns only. XORing one in therefore clears exactly
        // one pivot column from this row and can never introduce another, so
        // the set of pivot columns above `pc` only shrinks and the loop ends.
        for (;;) {
            size_t c = nextSetBit(row, cols_.size(), static_cast<size_t>(pc) + 1);
            while (c < cols_.size() && pivotOfCol_[c] < 0) {
                c = nextSetBit(row, cols_.size(), c + 1);
            }
            if (c >= cols_.size()) break;
            const size_t q = static_cast<size_t>(pivotOfCol_[c]);
            const uint64_t* qr = work(q);
            for (size_t x = 0; x < words_; ++x) row[x] ^= qr[x];
            rhs ^= workRhs_[q];
        }
        workRhs_[r] = rhs;
    }

    // Read off the short rows. One variable is a proven assignment; two is an
    // equivalence, which is a pair of binary clauses the original encoding
    // never contained.
    for (int32_t pc : pivotCols_) {
        const size_t r = static_cast<size_t>(pivotOfCol_[static_cast<size_t>(pc)]);
        const uint64_t* row = work(r);
        const size_t ncol = cols_.size();
        const size_t first = nextSetBit(row, ncol, 0);
        const size_t second = first < ncol ? nextSetBit(row, ncol, first + 1) : ncol;
        const size_t third = second < ncol ? nextSetBit(row, ncol, second + 1) : ncol;
        const int n = (first < ncol) + (second < ncol) + (third < ncol);
        if (n == 1) {
            // x = rhs
            out.units.push_back(mkLit(cols_[first], workRhs_[r] == 0));
        } else if (n == 2) {
            // x ^ y = rhs. With rhs 0 the two are equal, with rhs 1 opposite;
            // either way it is "x true forces y", recorded as the implication
            // pair the caller turns into clauses.
            const Var x = cols_[first], y = cols_[second];
            const bool same = workRhs_[r] == 0;
            out.equivA.push_back(mkLit(x, false));
            out.equivB.push_back(mkLit(y, !same));
        }
    }

    // Two pivots that depend on exactly the same free variables are related to
    // each other whatever those free variables turn out to be:
    //
    //     x = c_x ^ (sum of S)      y = c_y ^ (sum of S)   =>   x ^ y = c_x ^ c_y
    //
    // Reading rows one at a time never sees this - both rows can be long - so
    // without it the reduction throws away most of what it derived. Rows are
    // bucketed by a hash of their support and compared exactly inside a bucket.
    sup_.clear();
    sup_.reserve(pivotCols_.size());
    for (int32_t pc : pivotCols_) {
        const size_t r = static_cast<size_t>(pivotOfCol_[static_cast<size_t>(pc)]);
        const uint64_t* row = work(r);
        // FNV-1a over the row with the pivot column masked out.
        uint64_t h = 1469598103934665603ull;
        size_t bits = 0;
        for (size_t w = 0; w < words_; ++w) {
            uint64_t x = row[w];
            if (w == (static_cast<size_t>(pc) >> 6)) x &= ~(1ull << (static_cast<size_t>(pc) & 63));
            bits += popcount64(x);
            h = (h ^ x) * 1099511628211ull;
        }
        if (bits == 0) continue;  // already reported as a unit
        sup_.push_back(Support{h, static_cast<uint32_t>(r), pc});
    }
    std::sort(sup_.begin(), sup_.end(), [](const Support& a, const Support& b) {
        if (a.hash != b.hash) return a.hash < b.hash;
        return a.col < b.col;
    });

    for (size_t i = 0; i < sup_.size();) {
        size_t j = i + 1;
        while (j < sup_.size() && sup_[j].hash == sup_[i].hash) ++j;
        // Same hash is not yet the same support; compare the rows for real,
        // chaining each match to the previous one so k rows yield k-1 relations.
        for (size_t a = i; a < j; ++a) {
            for (size_t b = a + 1; b < j; ++b) {
                if (!sameSupport(sup_[a], sup_[b])) continue;
                const size_t ra = sup_[a].row, rb = sup_[b].row;
                const Var x = cols_[static_cast<size_t>(sup_[a].col)];
                const Var y = cols_[static_cast<size_t>(sup_[b].col)];
                const bool same = workRhs_[ra] == workRhs_[rb];
                out.equivA.push_back(mkLit(x, false));
                out.equivB.push_back(mkLit(y, !same));
                break;  // one link per row is enough; the rest follow
            }
        }
        i = j;
    }
}

bool Gf2System::sameSupport(const Support& a, const Support& b) const {
    const uint64_t* ra = work(a.row);
    const uint64_t* rb = work(b.row);
    const size_t wa = static_cast<size_t>(a.col) >> 6, wb = static_cast<size_t>(b.col) >> 6;
    const uint64_t ma = 1ull << (static_cast<size_t>(a.col) & 63);
    const uint64_t mb = 1ull << (static_cast<size_t>(b.col) & 63);
    for (size_t w = 0; w < words_; ++w) {
        uint64_t x = ra[w], y = rb[w];
        if (w == wa) x &= ~ma;
        if (w == wb) y &= ~mb;
        if (x != y) return false;
    }
    return true;
}

uint64_t Gf2System::memoryBytes() const {
    uint64_t n = static_cast<uint64_t>(work_.capacity()) * sizeof(uint64_t);
    n += static_cast<uint64_t>(vars_.capacity() + cols_.capacity()) * sizeof(Var);
    n += static_cast<uint64_t>(rows_.capacity()) * sizeof(Row);
    n += static_cast<uint64_t>(colOf_.capacity() + pivotOfCol_.capacity()) * sizeof(int32_t);
    return n;
}

}  // namespace tcs
