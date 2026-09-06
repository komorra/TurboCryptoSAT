#include "tune.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cnf.h"
#include "platform.h"
#include "solver.h"

namespace fs = std::filesystem;

namespace tcs {
namespace {

// One instance plus the solution that validates a run over it.
struct Target {
    std::string path;
    std::string solutionPath;
    Cnf cnf;
    std::vector<int8_t> oracle;  // per variable: 1, -1, or 0 for "no opinion"
};

// The parameters the search moves. Everything else in Options is left alone.
struct Params {
    int sigLen;
    int initk;
    int mink;
    long long stallLimit;
    int probeVars;
    int sampleRounds;
};

// One parameter and the values the preset lets the search try for it.
struct Axis {
    const char* name;
    std::vector<long long> values;
    long long Params::* slotLL = nullptr;
    int Params::* slotInt = nullptr;
};

struct Score {
    double frac = 0.0;      // mean share of variables assigned, over every run
    double seconds = 0.0;   // mean wall time
    int solved = 0;
    int wrong = 0;          // runs that committed a literal against the solution
    int runs = 0;
    bool full() const { return runs > 0 && solved == runs; }
};

// How close two partial results have to be before the difference is treated as
// seed noise rather than signal. Measured: across a whole preset sweep of a
// 17-round SHA-256 preimage every setting landed between 31.0% and 33.4%, so
// anything inside a couple of points is not a ranking, it is a coin flip.
constexpr double kNoise = 0.02;

// Ordering the search optimises: more variables correctly assigned wins, and
// among settings that finish the instance outright, the faster one wins.
//
// Two refinements, both there to stop the search chasing noise:
//
// Time is not a tie-break between partial results. A trial that reached 40% and
// then committed a wrong literal after two seconds is not better than one that
// reached 40% and was still going at the timeout - rewarding the first would
// select for settings that fail fast.
//
// When neither setting finishes and their progress is within the noise band, the
// one that went wrong fewer times wins. A run that poisons the assignment can
// never solve the instance no matter how long it is given; a run that merely
// stalls still might. Progress outside the band still decides on its own, so
// this only settles ties.
bool better(const Score& a, const Score& b) {
    if (a.full() != b.full()) return a.full();
    if (a.full()) return a.seconds < b.seconds;
    if (a.frac > b.frac + kNoise) return true;
    if (b.frac > a.frac + kNoise) return false;
    if (a.wrong != b.wrong) return a.wrong < b.wrong;
    return a.frac > b.frac;
}

std::string describe(const Params& p) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "--siglen %d --initk %d --mink %d --stall-limit %lld --probe-vars %d "
                  "--sample-rounds %d",
                  p.sigLen, p.initk, p.mink, p.stallLimit, p.probeVars, p.sampleRounds);
    return std::string(buf);
}

void apply(const Params& p, Options& o) {
    o.sigLen = p.sigLen;
    o.initk = p.initk;
    o.mink = p.mink;
    o.stallLimit = p.stallLimit;
    o.probeVars = p.probeVars;
    o.sampleRounds = p.sampleRounds;
}

bool loadOracle(Target& t, std::string& error) {
    Cnf sol;
    std::vector<Lit> units;
    if (!loadDimacs(t.solutionPath, sol, units, error)) return false;
    if (units.empty()) {
        error = "no unit clauses in " + t.solutionPath + "; it is not a solution file";
        return false;
    }
    t.oracle.assign(static_cast<size_t>(t.cnf.numVars), 0);
    for (Lit l : units) {
        const Var v = litVar(l);
        if (v >= t.cnf.numVars) continue;  // solution names variables we do not have
        const int8_t want = litSign(l) ? static_cast<int8_t>(-1) : static_cast<int8_t>(1);
        if (t.oracle[static_cast<size_t>(v)] != 0 &&
            t.oracle[static_cast<size_t>(v)] != want) {
            error = "the solution assigns variable " + std::to_string(v + 1) + " both ways";
            return false;
        }
        t.oracle[static_cast<size_t>(v)] = want;
    }

    // A solution that does not satisfy the instance would abort every trial and
    // the whole run would report nothing but noise. Catch it here.
    const size_t nc = t.cnf.clauseCount();
    for (size_t c = 0; c < nc; ++c) {
        const Lit* b = t.cnf.clauseBegin(c);
        const uint32_t len = t.cnf.clauseLen(c);
        bool sat = false;
        bool complete = true;
        for (uint32_t i = 0; i < len; ++i) {
            const int8_t x = t.oracle[static_cast<size_t>(litVar(b[i]))];
            if (x == 0) { complete = false; continue; }
            if (litSign(b[i]) ? x < 0 : x > 0) { sat = true; break; }
        }
        if (!sat && complete) {
            error = t.solutionPath + " does not satisfy " + t.path + " (clause " +
                    std::to_string(c + 1) + ")";
            return false;
        }
    }
    return true;
}

bool collectTargets(const Options& opt, std::vector<Target>& out, std::string& error) {
    std::vector<std::pair<std::string, std::string>> pairs;
    std::error_code ec;

    if (fs::is_directory(opt.tunePath, ec)) {
        // Tune over a whole family: every instance in the directory that has a
        // solution beside it.
        std::vector<std::string> files;
        for (const auto& e : fs::directory_iterator(opt.tunePath, ec)) {
            if (!e.is_regular_file()) continue;
            const std::string p = e.path().string();
            if (p.size() < 4 || p.compare(p.size() - 4, 4, ".cnf") != 0) continue;
            const std::string sfx = ".solution.cnf";
            if (p.size() >= sfx.size() &&
                p.compare(p.size() - sfx.size(), sfx.size(), sfx) == 0) {
                continue;
            }
            files.push_back(p);
        }
        std::sort(files.begin(), files.end());
        for (const std::string& f : files) {
            const std::string sol = solutionPathFor(f);
            if (fs::exists(sol, ec)) pairs.emplace_back(f, sol);
        }
        if (pairs.empty()) {
            error = "no instance in " + opt.tunePath + " has a .solution.cnf beside it";
            return false;
        }
    } else {
        const std::string sol =
            opt.tuneSolution.empty() ? solutionPathFor(opt.tunePath) : opt.tuneSolution;
        if (!fs::exists(sol, ec)) {
            error = "no solution file: " + sol +
                    " (pass one explicitly as the second argument)";
            return false;
        }
        pairs.emplace_back(opt.tunePath, sol);
    }

    out.clear();
    out.resize(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i) {
        Target& t = out[i];
        t.path = pairs[i].first;
        t.solutionPath = pairs[i].second;
        std::vector<Lit> units;
        if (!loadDimacs(t.path, t.cnf, units, error)) return false;
        if (!loadOracle(t, error)) return false;
    }
    return true;
}

// `remaining` reports the seconds left of the whole search, or a negative
// number when there is no overall budget. Checking it only around evaluate()
// would not bound anything: one call runs targets * seeds trials, each allowed
// the full --tune-timeout, so the budget has to reach inside and cap the
// individual trial as well.
Score evaluate(const Options& base, const Params& p, std::vector<Target>& targets, int seeds,
               const std::function<double()>& remaining) {
    Score s;
    Options o = base;
    apply(p, o);
    o.ui = false;
    o.quiet = true;
    o.seedGiven = true;

    // Leaving early has to fall through to the averaging below, not return on
    // the spot: the sums are per run, and a Score reported unaveraged reads as
    // an impossible score - "147.2% of variables" - that also outranks every
    // honest one.
    bool stop = false;
    for (Target& t : targets) {
        if (stop) break;
        for (int k = 0; k < seeds; ++k) {
            if (interruptRequested()) { stop = true; break; }
            const double left = remaining();
            if (left == 0.0) { stop = true; break; }  // budget spent
            o.timeout = left > 0.0 ? std::min(base.tuneTrialTimeout, left)
                                   : base.tuneTrialTimeout;
            o.seed = 0x9E3779B97F4A7C15ull * static_cast<uint64_t>(k + 1) + 12345u;
            Solver solver(t.cnf, o);
            solver.setOracle(&t.oracle);
            const uint64_t t0 = nowNs();
            SolveResult r = solver.solve();
            const double secs = static_cast<double>(nowNs() - t0) * 1e-9;

            // Score only the variables the formula actually constrains. The
            // ones no clause mentions are pinned before solving starts, and on
            // an encoding with a lot of them they would otherwise be a large
            // constant added to every setting alike, compressing the very
            // differences the search is trying to see.
            const double real = static_cast<double>(t.cnf.numVars) -
                                static_cast<double>(r.stats.unusedVars);
            const double done = static_cast<double>(r.stats.assignedVars) -
                                static_cast<double>(r.stats.unusedVars);
            const double frac = real > 0.0 ? done / real : 1.0;
            s.frac += r.status == SolveStatus::Solved ? 1.0 : frac;
            s.seconds += secs;
            ++s.runs;
            if (r.status == SolveStatus::Solved) ++s.solved;
            if (r.status == SolveStatus::OracleMismatch) ++s.wrong;
        }
    }
    if (s.runs) {
        s.frac /= s.runs;
        s.seconds /= s.runs;
    }
    return s;
}

void buildAxes(const std::string& preset, std::vector<Axis>& axes, bool& ok) {
    ok = true;
    axes.clear();
    // Ordered by how much they move the result, so the cheap early passes are
    // spent where it matters: initk and mink decide whether the statistical
    // layer speaks at all, the rest only shade it.
    if (preset == "quick") {
        axes.push_back({"initk", {4, 5, 6, 7, 8}, nullptr, &Params::initk});
        axes.push_back({"mink", {8, 10, 16, 32}, nullptr, &Params::mink});
        axes.push_back({"siglen", {512, 1024, 2048}, nullptr, &Params::sigLen});
    } else if (preset == "balanced") {
        axes.push_back({"initk", {3, 4, 5, 6, 7, 8, 9, 10}, nullptr, &Params::initk});
        axes.push_back({"mink", {4, 8, 10, 16, 24, 32, 64}, nullptr, &Params::mink});
        axes.push_back({"siglen", {256, 512, 1024, 2048}, nullptr, &Params::sigLen});
        axes.push_back({"stall-limit", {250, 1000, 4000}, &Params::stallLimit, nullptr});
        axes.push_back({"probe-vars", {1, 2}, nullptr, &Params::probeVars});
    } else if (preset == "thorough") {
        axes.push_back({"initk", {2, 3, 4, 5, 6, 7, 8, 9, 10, 12}, nullptr, &Params::initk});
        axes.push_back({"mink", {2, 4, 8, 10, 12, 16, 24, 32, 48, 64}, nullptr, &Params::mink});
        axes.push_back({"siglen", {256, 512, 1024, 2048, 4096}, nullptr, &Params::sigLen});
        axes.push_back({"stall-limit", {100, 250, 1000, 4000, 16000},
                        &Params::stallLimit, nullptr});
        axes.push_back({"probe-vars", {1, 2, 3}, nullptr, &Params::probeVars});
        axes.push_back({"sample-rounds", {6, 12, 24}, nullptr, &Params::sampleRounds});
    } else {
        ok = false;
    }
}

void report(const char* label, const Params& p, const Score& s) {
    std::printf("  %-10s %-62s  ", label, describe(p).c_str());
    if (s.full()) {
        std::printf("SOLVED %d/%d  %.2fs\n", s.solved, s.runs, s.seconds);
    } else {
        std::printf("%5.1f%% vars  solved %d/%d  wrong %d\n", 100.0 * s.frac, s.solved,
                    s.runs, s.wrong);
    }
    std::fflush(stdout);
}

}  // namespace

int runTune(const Options& opt) {
    std::vector<Axis> axes;
    bool presetOk = false;
    buildAxes(opt.tunePreset, axes, presetOk);
    if (!presetOk) {
        std::fprintf(stderr, "unknown preset: %s (quick, balanced, thorough)\n",
                     opt.tunePreset.c_str());
        return 1;
    }

    std::vector<Target> targets;
    std::string error;
    if (!collectTargets(opt, targets, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    const int seeds = std::max(1, opt.tuneSeeds);
    std::printf("TurboCryptoSAT tuning - preset %s, %zu instance%s, %d seed%s per setting,"
                " %.0fs per trial\n",
                opt.tunePreset.c_str(), targets.size(), targets.size() == 1 ? "" : "s",
                seeds, seeds == 1 ? "" : "s", opt.tuneTrialTimeout);
    for (const Target& t : targets) {
        std::printf("  target     %s (%d vars, %zu clauses) against %s\n", t.path.c_str(),
                    t.cnf.numVars, t.cnf.clauseCount(), t.solutionPath.c_str());
    }
    std::printf("\n");

    Params best{opt.sigLen, opt.initk, opt.mink,
                opt.stallLimit < 0 ? 1000 : opt.stallLimit, opt.probeVars, opt.sampleRounds};

    const uint64_t start = nowNs();
    // Seconds still available to the whole search: negative when unbounded, and
    // exactly zero once the budget is gone, which is what stops a trial from
    // being started at all.
    auto remaining = [&]() -> double {
        if (opt.tuneBudget <= 0.0) return -1.0;
        const double left = opt.tuneBudget - static_cast<double>(nowNs() - start) * 1e-9;
        return left > 0.0 ? left : 0.0;
    };
    auto outOfBudget = [&] { return remaining() == 0.0; };

    // Every setting the search has already measured. Without this the second
    // pass re-runs the whole first pass verbatim whenever the winner did not
    // move, which is half the budget spent reproducing known numbers.
    std::vector<std::array<long long, 6>> seen;
    auto key = [](const Params& p) {
        return std::array<long long, 6>{p.sigLen, p.initk, p.mink, p.stallLimit,
                                        p.probeVars, p.sampleRounds};
    };
    auto known = [&](const Params& p) {
        const auto k = key(p);
        return std::find(seen.begin(), seen.end(), k) != seen.end();
    };

    Score bestScore = evaluate(opt, best, targets, seeds, remaining);
    seen.push_back(key(best));
    report("baseline", best, bestScore);
    size_t trials = 1;
    int solvedAnywhere = bestScore.solved;

    // Coordinate descent: one axis at a time, each evaluated against the best
    // settings found so far. A full grid over six axes is thousands of trials;
    // this is a few dozen and, because the axes barely interact once initk and
    // mink are in their band, lands in the same place.
    const int passes = opt.tunePreset == "quick" ? 1 : 2;
    for (int pass = 0; pass < passes && !interruptRequested() && !outOfBudget(); ++pass) {
        bool improved = false;
        for (const Axis& a : axes) {
            if (interruptRequested() || outOfBudget()) break;
            std::printf("\n  [%s]\n", a.name);
            for (long long v : a.values) {
                if (interruptRequested() || outOfBudget()) break;
                Params cand = best;
                if (a.slotLL) {
                    if (best.*(a.slotLL) == v) continue;
                    cand.*(a.slotLL) = v;
                } else {
                    if (static_cast<long long>(best.*(a.slotInt)) == v) continue;
                    cand.*(a.slotInt) = static_cast<int>(v);
                }
                if (known(cand)) continue;
                const Score s = evaluate(opt, cand, targets, seeds, remaining);
                seen.push_back(key(cand));
                ++trials;
                solvedAnywhere += s.solved;
                report("try", cand, s);
                if (better(s, bestScore)) {
                    bestScore = s;
                    best = cand;
                    improved = true;
                    std::printf("  %-10s new best\n", "->");
                }
            }
        }
        if (!improved) break;  // a whole pass changed nothing; more will not either
    }

    const double elapsed = static_cast<double>(nowNs() - start) * 1e-9;
    std::printf("\n");
    if (interruptRequested()) std::printf("  interrupted - reporting the best found so far\n");
    else if (outOfBudget()) std::printf("  budget spent - reporting the best found so far\n");

    std::printf("  trials       %zu distinct settings in %s\n", trials,
                formatDuration(elapsed).c_str());
    std::printf("  best         %s\n", describe(best).c_str());
    if (bestScore.full()) {
        std::printf("  result       SOLVED on every run (%d/%d), %.2fs average\n",
                    bestScore.solved, bestScore.runs, bestScore.seconds);
    } else {
        std::printf("  result       %.1f%% of variables on average, solved %d/%d runs,"
                    " %d ended on a wrong literal\n",
                    100.0 * bestScore.frac, bestScore.solved, bestScore.runs, bestScore.wrong);
    }
    if (solvedAnywhere == 0) {
        std::printf("\n  WARNING  no setting solved the instance in %.0fs, so every trial was\n"
                    "           ranked on partial progress - and on a hard instance that spread\n"
                    "           is mostly seed noise. Raise --tune-timeout above the time a\n"
                    "           successful run actually takes, or --tune-seeds, before trusting\n"
                    "           this answer.\n",
                    opt.tuneTrialTimeout);
    }
    std::printf("\n  turbocryptosat <instance.cnf> %s\n\n", describe(best).c_str());
    return 0;
}

}  // namespace tcs
