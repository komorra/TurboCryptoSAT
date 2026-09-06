#include "gates.h"

#include <algorithm>

namespace tcs {
namespace {

// A definition found by pattern matching, before it is known whether it can be
// used: several candidates may claim the same output variable, and an XOR
// relation can be read in three directions.
struct Candidate {
    Var out = 0;
    Lit in0 = 0;
    Lit in1 = 0;
    Gate::Op op = Gate::And;
    bool negOut = false;
    uint8_t clauseCount = 0;
    uint32_t clauses[4] = {0, 0, 0, 0};
};

// One ternary clause reduced to its variable triple plus the polarity pattern,
// so that XOR groups can be found by sorting instead of hashing.
struct Triple {
    Var v[3];
    uint8_t signs;  // bit i set when the literal over v[i] is negated
    uint32_t clause;
};

bool tripleLess(const Triple& a, const Triple& b) {
    if (a.v[0] != b.v[0]) return a.v[0] < b.v[0];
    if (a.v[1] != b.v[1]) return a.v[1] < b.v[1];
    return a.v[2] < b.v[2];
}

// o == x & y, for both polarities of every variable. The binary clauses of ~o
// are stamped first, then each ternary clause containing o is checked against
// them; a hit gives the two inputs directly.
void findAndGates(const Cnf& cnf, std::vector<Candidate>& out) {
    const int nv = cnf.numVars;
    const size_t nl = static_cast<size_t>(nv) * 2u;
    std::vector<uint32_t> stamp(nl, 0);
    std::vector<uint32_t> binClause(nl, 0);
    uint32_t counter = 0;

    for (Var v = 0; v < nv; ++v) {
        for (int p = 0; p < 2; ++p) {
            const Lit o = mkLit(v, p != 0);
            const Lit no = litNeg(o);

            ++counter;
            bool anyBinary = false;
            for (uint32_t i = cnf.occStart[static_cast<size_t>(no)],
                          e = cnf.occStart[static_cast<size_t>(no) + 1];
                 i < e; ++i) {
                const uint32_t c = cnf.occ[i];
                if (cnf.clauseLen(c) != 2) continue;
                const Lit* b = cnf.clauseBegin(c);
                const Lit x = (b[0] == no) ? b[1] : b[0];
                stamp[static_cast<size_t>(x)] = counter;
                binClause[static_cast<size_t>(x)] = c;
                anyBinary = true;
            }
            if (!anyBinary) continue;

            for (uint32_t i = cnf.occStart[static_cast<size_t>(o)],
                          e = cnf.occStart[static_cast<size_t>(o) + 1];
                 i < e; ++i) {
                const uint32_t c = cnf.occ[i];
                if (cnf.clauseLen(c) != 3) continue;
                const Lit* b = cnf.clauseBegin(c);
                Lit p0 = -1, p1 = -1;
                for (int j = 0; j < 3; ++j) {
                    if (b[j] == o) continue;
                    if (p0 < 0) p0 = b[j];
                    else p1 = b[j];
                }
                if (p1 < 0) continue;
                const Lit x = litNeg(p0);
                const Lit y = litNeg(p1);
                if (x == y) continue;
                if (stamp[static_cast<size_t>(x)] != counter) continue;
                if (stamp[static_cast<size_t>(y)] != counter) continue;

                Candidate cd;
                cd.out = v;
                cd.in0 = x;
                cd.in1 = y;
                cd.op = Gate::And;
                // o == x & y with o == ~v means v is the negation of the AND.
                cd.negOut = (p != 0);
                cd.clauseCount = 3;
                cd.clauses[0] = c;
                cd.clauses[1] = binClause[static_cast<size_t>(x)];
                cd.clauses[2] = binClause[static_cast<size_t>(y)];
                out.push_back(cd);
                break;  // one definition per output literal is all that is used
            }
        }
    }
}

// A clause forbids exactly the assignment that makes each of its literals
// false, so a clause over three variables rules out the assignment given by its
// sign pattern. Four clauses ruling out all four patterns of one parity say
// precisely that the parity of the triple is the other one - an XOR relation.
void findXorGates(const Cnf& cnf, std::vector<Candidate>& out) {
    const size_t nc = cnf.clauseCount();
    std::vector<Triple> tr;
    tr.reserve(nc);

    for (size_t c = 0; c < nc; ++c) {
        if (cnf.clauseLen(c) != 3) continue;
        const Lit* b = cnf.clauseBegin(c);
        Triple t;
        t.clause = static_cast<uint32_t>(c);
        t.signs = 0;
        for (int j = 0; j < 3; ++j) t.v[j] = litVar(b[j]);
        uint8_t s[3];
        for (int j = 0; j < 3; ++j) s[j] = litSign(b[j]) ? 1u : 0u;
        // Sort the three by variable, carrying the polarity along.
        for (int a = 0; a < 2; ++a) {
            for (int j = 0; j < 2 - a; ++j) {
                if (t.v[j] > t.v[j + 1]) {
                    std::swap(t.v[j], t.v[j + 1]);
                    std::swap(s[j], s[j + 1]);
                }
            }
        }
        if (t.v[0] == t.v[1] || t.v[1] == t.v[2]) continue;  // not three variables
        t.signs = static_cast<uint8_t>(s[0] | (s[1] << 1) | (s[2] << 2));
        tr.push_back(t);
    }

    std::sort(tr.begin(), tr.end(), tripleLess);

    // Sign patterns by parity: odd popcount is {1,2,4,7}, even is {0,3,5,6}.
    const uint8_t kOddPatterns = 0x96;
    const uint8_t kEvenPatterns = 0x69;

    size_t i = 0;
    while (i < tr.size()) {
        size_t j = i + 1;
        while (j < tr.size() && tr[j].v[0] == tr[i].v[0] && tr[j].v[1] == tr[i].v[1] &&
               tr[j].v[2] == tr[i].v[2]) {
            ++j;
        }
        const size_t run = j - i;
        if (run >= 4) {
            uint8_t seen = 0;
            uint32_t byPattern[8];
            for (size_t k = i; k < j; ++k) {
                const uint8_t bit = static_cast<uint8_t>(1u << tr[k].signs);
                if (!(seen & bit)) {
                    seen = static_cast<uint8_t>(seen | bit);
                    byPattern[tr[k].signs] = tr[k].clause;
                }
            }
            int xorConst = -1;
            if (seen == kOddPatterns) {
                xorConst = 0;  // every odd assignment forbidden: v0^v1^v2 == 0
            } else if (seen == kEvenPatterns) {
                xorConst = 1;
            }
            if (xorConst >= 0) {
                const uint8_t wanted = (xorConst == 0) ? kOddPatterns : kEvenPatterns;
                Candidate cd;
                cd.op = Gate::Xor;
                cd.negOut = (xorConst == 1);
                cd.clauseCount = 0;
                for (int pat = 0; pat < 8; ++pat) {
                    if (wanted & (1u << pat)) cd.clauses[cd.clauseCount++] = byPattern[pat];
                }
                // The relation can define any of the three; which one it ends
                // up being is decided by the ordering pass below.
                for (int k = 0; k < 3; ++k) {
                    cd.out = tr[i].v[k];
                    cd.in0 = mkLit(tr[i].v[(k + 1) % 3], false);
                    cd.in1 = mkLit(tr[i].v[(k + 2) % 3], false);
                    out.push_back(cd);
                }
            }
        }
        i = j;
    }
}

}  // namespace

void extractGates(const Cnf& cnf, GateNetwork& net) {
    net = GateNetwork();
    const int nv = cnf.numVars;
    net.clauseCount = cnf.clauseCount();
    net.isFree.assign(static_cast<size_t>(nv), 0);
    if (nv <= 0) return;

    std::vector<Candidate> cands;
    findAndGates(cnf, cands);
    findXorGates(cnf, cands);
    if (cands.empty()) {
        net.residualClauses = net.clauseCount;
        for (Var v = 0; v < nv; ++v) {
            net.isFree[static_cast<size_t>(v)] = 1;
            net.freeVars.push_back(v);
        }
        return;
    }

    // Inputs of each candidate, deduplicated, so the readiness count below is
    // decremented exactly once per variable.
    std::vector<uint32_t> inCount(cands.size(), 0);
    std::vector<uint32_t> useStart(static_cast<size_t>(nv) + 1, 0);
    for (size_t i = 0; i < cands.size(); ++i) {
        const Var a = litVar(cands[i].in0);
        const Var b = litVar(cands[i].in1);
        inCount[i] = (a == b) ? 1u : 2u;
        useStart[static_cast<size_t>(a) + 1]++;
        if (a != b) useStart[static_cast<size_t>(b) + 1]++;
    }
    for (int v = 0; v < nv; ++v) useStart[static_cast<size_t>(v) + 1] += useStart[static_cast<size_t>(v)];
    std::vector<uint32_t> uses(useStart[static_cast<size_t>(nv)]);
    {
        std::vector<uint32_t> fill(useStart.begin(), useStart.end() - 1);
        for (size_t i = 0; i < cands.size(); ++i) {
            const Var a = litVar(cands[i].in0);
            const Var b = litVar(cands[i].in1);
            uses[fill[static_cast<size_t>(a)]++] = static_cast<uint32_t>(i);
            if (a != b) uses[fill[static_cast<size_t>(b)]++] = static_cast<uint32_t>(i);
        }
    }

    std::vector<uint32_t> defCount(static_cast<size_t>(nv), 0);
    for (const Candidate& c : cands) defCount[static_cast<size_t>(c.out)]++;

    std::vector<uint8_t> known(static_cast<size_t>(nv), 0);
    std::vector<uint8_t> used(net.clauseCount, 0);
    std::vector<Var> queue;
    queue.reserve(static_cast<size_t>(nv));
    net.gates.reserve(cands.size() / 2 + 1);

    auto makeFree = [&](Var v) {
        known[static_cast<size_t>(v)] = 1;
        net.isFree[static_cast<size_t>(v)] = 1;
        net.freeVars.push_back(v);
        queue.push_back(v);
    };

    // Nothing defines these, so they carry the randomness; on a Tseitin encoded
    // circuit they are exactly its inputs.
    for (Var v = 0; v < nv; ++v) {
        if (defCount[static_cast<size_t>(v)] == 0) makeFree(v);
    }

    size_t head = 0;
    Var scan = 0;  // rolling cursor: once known, a variable stays known
    for (;;) {
        while (head < queue.size()) {
            const Var v = queue[head++];
            for (uint32_t k = useStart[static_cast<size_t>(v)],
                          e = useStart[static_cast<size_t>(v) + 1];
                 k < e; ++k) {
                const uint32_t ci = uses[k];
                if (--inCount[ci] != 0) continue;
                Candidate& cd = cands[ci];
                // A variable is defined once: a later candidate for the same
                // output would either duplicate it or contradict it, and an XOR
                // relation already read in one direction must not be read in a
                // second, which would close a cycle.
                if (known[static_cast<size_t>(cd.out)]) continue;
                known[static_cast<size_t>(cd.out)] = 1;
                Gate g;
                g.out = cd.out;
                g.in0 = cd.in0;
                g.in1 = cd.in1;
                g.op = cd.op;
                g.negOut = cd.negOut;
                net.gates.push_back(g);
                for (uint8_t q = 0; q < cd.clauseCount; ++q) used[cd.clauses[q]] = 1;
                queue.push_back(cd.out);
            }
        }
        // Whatever is left is defined only in terms of itself. Breaking the
        // cycle at the lowest variable keeps the pass finite; the clauses that
        // defined it stay residual, which is what the caller checks.
        while (scan < nv && known[static_cast<size_t>(scan)]) ++scan;
        if (scan >= nv) break;
        makeFree(scan);
    }

    net.residualClauses = 0;
    for (uint8_t u : used) {
        if (!u) ++net.residualClauses;
    }
}

}  // namespace tcs
