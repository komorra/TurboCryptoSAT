#include "cnf.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tcs {
namespace {

// strtol family wrappers that report a range error instead of quietly clamping.
// Without this a header like "p cnf 100 -1" turns into a size_t of 2^64-1 and
// the reserve() below takes the process down with an uncaught length_error.
bool readLong(const char*& p, long long& out) {
    errno = 0;
    char* next = nullptr;
    const long long v = std::strtoll(p, &next, 10);
    if (next == p) return false;
    p = next;
    if (errno == ERANGE) return false;
    out = v;
    return true;
}

}  // namespace

void Cnf::buildOccurrences() {
    const size_t nLits = static_cast<size_t>(numVars) * 2;
    occStart.assign(nLits + 1, 0);
    for (Lit l : lits) {
        ++occStart[static_cast<size_t>(l) + 1];
    }
    for (size_t i = 1; i <= nLits; ++i) {
        occStart[i] += occStart[i - 1];
    }
    occ.resize(lits.size());
    std::vector<uint32_t> cursor(occStart.begin(), occStart.end() - 1);
    const size_t nc = clauseCount();
    for (size_t c = 0; c < nc; ++c) {
        for (uint32_t i = start[c]; i < start[c + 1]; ++i) {
            occ[cursor[static_cast<size_t>(lits[i])]++] = static_cast<uint32_t>(c);
        }
    }
}

bool loadDimacs(const std::string& path, Cnf& cnf, std::vector<Lit>& unitLits,
                std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open file: " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        error = "cannot determine size of " + path;
        return false;
    }
    std::vector<char> buf(static_cast<size_t>(size) + 1);
    const size_t got = std::fread(buf.data(), 1, static_cast<size_t>(size), f);
    const bool readFailed = std::ferror(f) != 0 || got != static_cast<size_t>(size);
    std::fclose(f);
    if (readFailed) {
        error = "short read on " + path + "; the file may be truncated";
        return false;
    }
    buf[got] = 0;

    cnf = Cnf();
    unitLits.clear();

    int declaredVars = 0;
    size_t declaredClauses = 0;
    std::vector<int> clause;
    clause.reserve(64);
    cnf.start.push_back(0);

    const char* p = buf.data();
    const char* end = buf.data() + got;
    int maxVarSeen = 0;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (p >= end) break;
        if (*p == 'c' || *p == '%') {  // comment, or the trailing '%' marker
            while (p < end && *p != '\n') ++p;
            continue;
        }
        if (*p == 'p') {
            while (p < end && *p != '\n') {
                if (std::strncmp(p, "cnf", 3) == 0) {
                    p += 3;
                    long long dv = 0, dc = 0;
                    // The clause count only drives reserve(), so it is enough
                    // that it cannot exceed what the file could possibly hold:
                    // the shortest clause anyone can write is "0" plus a
                    // separator.
                    if (!readLong(p, dv) || !readLong(p, dc) || dv < 0 || dv > kMaxVar ||
                        dc < 0 || dc > static_cast<long long>(size)) {
                        error = "bad problem line in " + path;
                        return false;
                    }
                    declaredVars = static_cast<int>(dv);
                    declaredClauses = static_cast<size_t>(dc);
                    break;
                }
                ++p;
            }
            while (p < end && *p != '\n') ++p;
            if (declaredClauses > 0) {
                cnf.start.reserve(declaredClauses + 1);
                cnf.lits.reserve(declaredClauses * 3);
            }
            continue;
        }

        // Read one clause, terminated by 0.
        clause.clear();
        bool sawTerminator = false;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
            if (p >= end) break;
            if (*p == 'c' || *p == '%') {
                while (p < end && *p != '\n') ++p;
                continue;
            }
            long long v = 0;
            if (!readLong(p, v)) {
                error = "unexpected character in " + path;
                return false;
            }
            if (v == 0) {
                sawTerminator = true;
                break;
            }
            const long long av = v < 0 ? -v : v;
            if (av > kMaxVar) {
                error = "variable number out of range in " + path + ": " + std::to_string(v);
                return false;
            }
            if (static_cast<int>(av) > maxVarSeen) maxVarSeen = static_cast<int>(av);
            clause.push_back(static_cast<int>(v));
        }
        // A clause that simply runs out at the end of the file is not an empty
        // tail, it is a clause whose remaining literals are missing - and for a
        // solver, silently treating it as complete means answering a question
        // the file did not ask.
        if (!sawTerminator) {
            if (clause.empty()) continue;
            error = "clause not terminated by 0 in " + path;
            return false;
        }

        std::sort(clause.begin(), clause.end(),
                  [](int a, int b) { return (a < 0 ? -a : a) < (b < 0 ? -b : b) || ((a < 0 ? -a : a) == (b < 0 ? -b : b) && a < b); });
        bool tautology = false;
        size_t w = 0;
        for (size_t i = 0; i < clause.size(); ++i) {
            if (i > 0 && clause[i] == clause[i - 1]) continue;          // duplicate
            if (i > 0 && clause[i] == -clause[i - 1]) { tautology = true; break; }
            clause[w++] = clause[i];
        }
        if (tautology) continue;
        clause.resize(w);

        if (clause.empty()) {
            cnf.hasEmptyClause = true;
            continue;
        }
        for (int d : clause) {
            cnf.lits.push_back(dimacsToLit(d));
        }
        cnf.start.push_back(static_cast<uint32_t>(cnf.lits.size()));
        if (clause.size() > cnf.maxClauseLen) {
            cnf.maxClauseLen = static_cast<uint32_t>(clause.size());
        }
        if (clause.size() == 1) {
            unitLits.push_back(dimacsToLit(clause[0]));
        }
    }

    cnf.numVars = std::max(declaredVars, maxVarSeen);
    if (cnf.numVars <= 0) {
        error = "no variables found in " + path;
        return false;
    }
    cnf.buildOccurrences();
    return true;
}

std::string solutionPathFor(const std::string& cnfPath) {
    const size_t dot = cnfPath.find_last_of('.');
    const size_t slash = cnfPath.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return cnfPath.substr(0, dot) + ".solution" + cnfPath.substr(dot);
    }
    return cnfPath + ".solution.cnf";
}

bool writeSolutionCnf(const std::string& path, const std::string& sourceName,
                      const std::vector<int8_t>& value, int numVars, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    std::fprintf(f, "c Solution produced by TurboCryptoSAT\n");
    std::fprintf(f, "c source: %s\n", sourceName.c_str());
    std::fprintf(f, "c every variable is pinned by a unit clause\n");
    std::fprintf(f, "p cnf %d %d\n", numVars, numVars);
    for (int v = 0; v < numVars; ++v) {
        const int d = (value[static_cast<size_t>(v)] >= 0) ? (v + 1) : -(v + 1);
        std::fprintf(f, "%d 0\n", d);
    }
    // A full disk shows up here, not in any of the fprintf calls above, and
    // fclose is where the last buffer is actually flushed - so both have to be
    // asked before the file may be called written.
    const bool failed = std::ferror(f) != 0;
    if (std::fclose(f) != 0 || failed) {
        error = "failed writing " + path + "; the file is incomplete";
        return false;
    }
    return true;
}

}  // namespace tcs
