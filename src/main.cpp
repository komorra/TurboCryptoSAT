// TurboCryptoSAT - signature based SAT solver for cryptographic instances.
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "cnf.h"
#include "genbench.h"
#include "options.h"
#include "platform.h"
#include "selftest.h"
#include "solver.h"
#include "tune.h"
#include "ui.h"

namespace fs = std::filesystem;

namespace tcs {
namespace {

const char* kVersion = "1.0.0";

void printUsage() {
    std::printf(
        "TurboCryptoSAT %s - signature based SAT solver for cryptographic instances\n"
        "\n"
        "USAGE\n"
        "  turbocryptosat <instance.cnf> [options]\n"
        "  turbocryptosat benchmark <directory> [options]\n"
        "  turbocryptosat tune <instance.cnf> [solution.cnf] [options]\n"
        "  turbocryptosat tune <directory> [options]\n"
        "  turbocryptosat gen-benchmark <directory>\n"
        "  turbocryptosat selftest [rounds] [--seed <n>]\n"
        "\n"
        "INSTANCE OPTIONS\n"
        "  --inputs <a-b|auto>   Range of input variables (1-based, inclusive).\n"
        "                        Default: auto, variables are filled in index order.\n"
        "  --outputs <lits>      Target valuation as signed DIMACS literals, e.g.\n"
        "                        --outputs \"12 -13 14\". May be repeated. When omitted\n"
        "                        the unit clauses of the file are used instead.\n"
        "  --outputs-file <f>    Read the target valuation from a file.\n"
        "  --out <file>          Solution path. Default: <instance>.solution.cnf\n"
        "\n"
        "SEARCH OPTIONS\n"
        "  --siglen <n>          64-bit lanes per variable. Default 1024 (65536 samples).\n"
        "  --initk <n>           Assigned literals mixed into each probe. Default 6.\n"
        "                        Each one roughly halves the surviving sample set,\n"
        "                        so raising it sharpens the filter but invites\n"
        "                        verdicts drawn from too few samples.\n"
        "  --mink <n>            Minimum surviving samples for a verdict, counted in\n"
        "                        samples (lanes), not 64-bit words. Default 640,\n"
        "                        with an alternative word threshold below.\n"
        "  --mink-unit <unit>   samples (default) or words (nonempty 64-bit words,\n"
        "                        matching Piessra). Set --mink explicitly for words.\n"
        "  --probe-order <dir>  ascending (default) or descending (Piessra order).\n"
        "  --probe-vars <n>      Variables probed at once (2^n branches). Default 1.\n"
        "  --focus <n>           Bits of the target valuation every sample must\n"
        "                        reproduce. Lanes that miss one are redrawn until\n"
        "                        they hit, which narrows the population onto the\n"
        "                        neighbourhood of the solution at a cost of about\n"
        "                        2^n redraws. Default 8; 0 leaves the samples free\n"
        "                        executions of the circuit.\n"
        "  --threads <n>         Worker threads. Default: number of hardware threads.\n"
        "  --attempts <n>        Restarts after a conflict. Default 5.\n"
        "  --stall-limit <n>     Barren rounds before the sample population is\n"
        "                        redrawn and, failing that, a CDCL phase is run.\n"
        "                        Default 1000.\n"
        "  --cdcl-conflicts <n>  Conflict budget for one CDCL phase, doubled every\n"
        "                        time a phase proves nothing. Default 10000;\n"
        "                        0 means bounded only by --timeout.\n"
        "  --no-cdcl             Never run a CDCL phase; plateaus are answered by\n"
        "                        resampling and further probing only.\n"
        "  --no-gf2              Do not recover the XOR constraints or reduce them\n"
        "                        over GF(2). The parity structure of a hash round\n"
        "                        function is invisible to plain propagation, so\n"
        "                        this is on by default.\n"
        "  --gf2-interval <n>    New assignments between elimination passes.\n"
        "                        Default 16; a pass also runs at every plateau.\n"
        "  --sample-rounds <n>   Retry rounds while building the samples. Default 12.\n"
        "  --keep-samples        Reuse the sample population across restarts.\n"
        "  --timeout <sec>       Abort after the given number of seconds.\n"
        "  --seed <n>            Fix the random seed for reproducible runs.\n"
        "\n"
        "TUNING OPTIONS (tune mode)\n"
        "  --preset <name>       quick, balanced or thorough. Default balanced.\n"
        "  --tune-timeout <sec>  Budget for one trial. Default 30.\n"
        "  --tune-budget <sec>   Budget for the whole search. Default unlimited.\n"
        "  --tune-seeds <n>      Runs per setting; results are seed-noisy.\n"
        "                        Default 3.\n"
        "\n"
        "OUTPUT OPTIONS\n"
        "  --no-ui               Plain log output instead of the dashboard.\n"
        "  --quiet               Only print the final verdict.\n"
        "  --verbose             Print the configuration before solving.\n"
        "  -h, --help            This text.\n"
        "  --version             Print the version and exit.\n"
        "\n"
        "The solver assigns variables in place and never backtracks, so it targets\n"
        "satisfiable instances. Press Ctrl+C or ESC twice to abort a run.\n",
        kVersion);
}

// `lo`/`hi` are inclusive. Every option that ends up in an `int` passes
// INT_MAX as `hi`: without it the value is truncated on the cast and a typo
// like --siglen 99999999999 turns into a small or negative table size instead
// of an error.
bool parseIntArg(const char* s, long long& out, long long lo, long long hi) {
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s, &end, 10);
    if (!end || *end != 0 || end == s || errno == ERANGE) return false;
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

void appendLiterals(const std::string& text, std::vector<int>& out) {
    const char* p = text.c_str();
    while (*p) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        if (!*p) break;
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p) { ++p; continue; }
        p = end;
        if (v != 0) out.push_back(static_cast<int>(v));
    }
}

bool parseArgs(int argc, char** argv, Options& opt, int& exitCode) {
    exitCode = 0;
    if (argc < 2) {
        printUsage();
        exitCode = 1;
        return false;
    }

    int i = 1;
    if (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
        printUsage();
        return false;
    }
    if (std::strcmp(argv[1], "--version") == 0) {
        std::printf("TurboCryptoSAT %s\n", kVersion);
        return false;
    }
    if (std::strcmp(argv[1], "benchmark") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "benchmark mode needs a directory\n");
            exitCode = 1;
            return false;
        }
        opt.benchmarkDir = argv[2];
        i = 3;
    } else if (std::strcmp(argv[1], "tune") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "tune mode needs an instance or a directory\n");
            exitCode = 1;
            return false;
        }
        opt.tunePath = argv[2];
        i = 3;
        // A bare second path is the solution; anything starting with - is a flag.
        if (argc > 3 && argv[3][0] != '-') {
            opt.tuneSolution = argv[3];
            i = 4;
        }
    } else {
        opt.cnfPath = argv[1];
        i = 2;
    }

    for (; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                exitCode = 1;
                return nullptr;
            }
            return argv[++i];
        };
        long long n = 0;

        if (a == "--inputs") {
            const char* v = need("--inputs");
            if (!v) return false;
            if (std::strcmp(v, "auto") != 0) {
                const char* dash = std::strchr(v, '-');
                if (!dash) {
                    std::fprintf(stderr, "--inputs expects a range like 1-24\n");
                    exitCode = 1;
                    return false;
                }
                opt.inputFrom = std::atoi(std::string(v, dash).c_str());
                opt.inputTo = std::atoi(dash + 1);
                opt.inputsGiven = true;
            }
        } else if (a == "--outputs") {
            const char* v = need("--outputs");
            if (!v) return false;
            appendLiterals(v, opt.outputs);
            opt.outputsGiven = true;
        } else if (a == "--outputs-file") {
            const char* v = need("--outputs-file");
            if (!v) return false;
            FILE* f = std::fopen(v, "rb");
            if (!f) {
                std::fprintf(stderr, "cannot open %s\n", v);
                exitCode = 1;
                return false;
            }
            std::string text;
            char buf[4096];
            size_t got;
            while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, got);
            std::fclose(f);
            appendLiterals(text, opt.outputs);
            opt.outputsGiven = true;
        } else if (a == "--out") {
            const char* v = need("--out");
            if (!v) return false;
            opt.solutionPath = v;
        } else if (a == "--siglen") {
            const char* v = need("--siglen");
            if (!v || !parseIntArg(v, n, 1, INT_MAX)) { exitCode = 1; return false; }
            opt.sigLen = static_cast<int>(n);
        } else if (a == "--initk") {
            const char* v = need("--initk");
            if (!v || !parseIntArg(v, n, 0, INT_MAX)) { exitCode = 1; return false; }
            opt.initk = static_cast<int>(n);
        } else if (a == "--mink") {
            const char* v = need("--mink");
            if (!v || !parseIntArg(v, n, 0, INT_MAX)) { exitCode = 1; return false; }
            opt.mink = static_cast<int>(n);
        } else if (a == "--focus") {
            const char* v = need("--focus");
            if (!v || !parseIntArg(v, n, 0, INT_MAX)) { exitCode = 1; return false; }
            opt.focusBits = static_cast<int>(n);
        } else if (a == "--mink-unit") {
            const char* v = need("--mink-unit");
            if (!v) { exitCode = 1; return false; }
            if (std::strcmp(v, "words") == 0) opt.minkWords = true;
            else if (std::strcmp(v, "samples") == 0) opt.minkWords = false;
            else {
                std::fprintf(stderr, "--mink-unit expects samples or words\n");
                exitCode = 1;
                return false;
            }
        } else if (a == "--probe-order") {
            const char* v = need("--probe-order");
            if (!v) { exitCode = 1; return false; }
            if (std::strcmp(v, "descending") == 0) opt.probeDescending = true;
            else if (std::strcmp(v, "ascending") == 0) opt.probeDescending = false;
            else {
                std::fprintf(stderr, "--probe-order expects ascending or descending\n");
                exitCode = 1;
                return false;
            }
        } else if (a == "--probe-vars") {
            const char* v = need("--probe-vars");
            if (!v || !parseIntArg(v, n, 1, 16)) { exitCode = 1; return false; }
            opt.probeVars = static_cast<int>(n);
        } else if (a == "--threads") {
            const char* v = need("--threads");
            if (!v || !parseIntArg(v, n, 1, 1024)) { exitCode = 1; return false; }
            opt.threads = static_cast<int>(n);
        } else if (a == "--attempts") {
            const char* v = need("--attempts");
            if (!v || !parseIntArg(v, n, 1, INT_MAX)) { exitCode = 1; return false; }
            opt.attempts = static_cast<int>(n);
        } else if (a == "--stall-limit") {
            const char* v = need("--stall-limit");
            if (!v || !parseIntArg(v, n, 0, LLONG_MAX)) { exitCode = 1; return false; }
            opt.stallLimit = n;
        } else if (a == "--preset") {
            const char* v = need("--preset");
            if (!v) return false;
            opt.tunePreset = v;
        } else if (a == "--tune-timeout") {
            const char* v = need("--tune-timeout");
            if (!v) return false;
            opt.tuneTrialTimeout = std::atof(v);
        } else if (a == "--tune-budget") {
            const char* v = need("--tune-budget");
            if (!v) return false;
            opt.tuneBudget = std::atof(v);
        } else if (a == "--tune-seeds") {
            const char* v = need("--tune-seeds");
            if (!v || !parseIntArg(v, n, 1, INT_MAX)) { exitCode = 1; return false; }
            opt.tuneSeeds = static_cast<int>(n);
        } else if (a == "--cdcl-conflicts") {
            const char* v = need("--cdcl-conflicts");
            if (!v || !parseIntArg(v, n, 0, LLONG_MAX)) { exitCode = 1; return false; }
            opt.cdclConflicts = static_cast<uint64_t>(n);
        } else if (a == "--no-cdcl") {
            opt.cdcl = false;
        } else if (a == "--no-gf2") {
            opt.gf2 = false;
        } else if (a == "--gf2-interval") {
            const char* v = need("--gf2-interval");
            if (!v || !parseIntArg(v, n, 1, INT_MAX)) { exitCode = 1; return false; }
            opt.gf2Interval = static_cast<int>(n);
        } else if (a == "--sample-rounds") {
            const char* v = need("--sample-rounds");
            if (!v || !parseIntArg(v, n, 1, INT_MAX)) { exitCode = 1; return false; }
            opt.sampleRounds = static_cast<int>(n);
        } else if (a == "--keep-samples") {
            opt.keepSamples = true;
        } else if (a == "--timeout") {
            const char* v = need("--timeout");
            if (!v) return false;
            opt.timeout = std::atof(v);
        } else if (a == "--seed") {
            const char* v = need("--seed");
            if (!v || !parseIntArg(v, n, LLONG_MIN, LLONG_MAX)) { exitCode = 1; return false; }
            opt.seed = static_cast<uint64_t>(n);
            opt.seedGiven = true;
        } else if (a == "--no-ui") {
            opt.ui = false;
        } else if (a == "--quiet") {
            opt.quiet = true;
            opt.ui = false;
        } else if (a == "--verbose") {
            opt.verbose = true;
        } else if (a == "-h" || a == "--help") {
            printUsage();
            return false;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            exitCode = 1;
            return false;
        }
    }
    return true;
}

struct RunOutcome {
    SolveResult result;
    double wallSeconds = 0.0;
    size_t numVars = 0;
    size_t numClauses = 0;
    bool verified = false;
    size_t inputVars = 0;
    std::string solutionPath;
};

// Tracks recent progress so the dashboard can show a meaningful rate and ETA.
// Turns bursty progress into a usable estimate.
//
// Signature propagation does not assign variables at a steady pace: one
// productive probe cascades through hundreds of them at once and is then
// followed by seconds of nothing, so a rate measured over a few seconds swings
// by orders of magnitude and the estimate derived from it is unreadable. Two
// things fix that. The rate is measured over a wide window, and the estimate
// itself is filtered with a time constant instead of being recomputed from
// scratch every frame. While progress is stalled the estimate grows with the
// stall rather than blinking out, which is both steadier and more honest.
class EtaTracker {
public:
    void reset() {
        samples_.clear();
        rate_ = 0.0;
        eta_ = -1.0;
        lastT_ = 0.0;
    }

    void add(double t, int assigned, int total) {
        if (!samples_.empty() && t <= samples_.back().t) return;
        samples_.push_back({t, assigned});
        while (samples_.size() > 2 && t - samples_.front().t > kWindow) {
            samples_.pop_front();
        }

        const double span = samples_.back().t - samples_.front().t;
        const int gained = samples_.back().v - samples_.front().v;
        rate_ = (span > 1e-6 && gained > 0) ? gained / span : 0.0;

        const double dt = eta_ < 0.0 ? 0.0 : t - lastT_;
        lastT_ = t;

        const int remaining = total - assigned;
        if (remaining <= 0) {
            eta_ = 0.0;
            return;
        }

        double raw;
        if (rate_ > 1e-9) {
            raw = remaining / rate_;
        } else if (eta_ >= 0.0) {
            raw = eta_ + dt;  // stalled: the wait got longer, not unknown
        } else {
            return;  // nothing measured yet
        }

        if (eta_ < 0.0) {
            eta_ = raw;
        } else {
            const double alpha = 1.0 - std::exp(-dt / kTau);
            eta_ += alpha * (raw - eta_);
        }
    }

    double rate() const { return rate_; }
    double eta() const { return eta_; }

private:
    static constexpr double kWindow = 30.0;  // seconds of history behind the rate
    static constexpr double kTau = 10.0;     // smoothing time constant

    struct S { double t; int v; };
    std::deque<S> samples_;
    double rate_ = 0.0;
    double eta_ = -1.0;
    double lastT_ = 0.0;
};

RunOutcome runInstance(const Options& opt, const std::string& path, bool interactive) {
    RunOutcome out;
    Cnf cnf;
    std::vector<Lit> units;
    std::string error;
    const uint64_t t0 = nowNs();

    if (!loadDimacs(path, cnf, units, error)) {
        out.result.status = SolveStatus::Error;
        out.result.message = error;
        return out;
    }
    out.numVars = static_cast<size_t>(cnf.numVars);
    out.numClauses = cnf.clauseCount();

    Solver solver(cnf, opt);

    Ui ui;
    // TCS_FORCE_UI renders the dashboard even when stdout is redirected, which
    // is how the layout gets exercised in tests and screen captures.
    const bool forceUi = std::getenv("TCS_FORCE_UI") != nullptr;
    ui.setEnabled(interactive && opt.ui && (stdoutIsTty() || forceUi));

    ResourceMonitor rm;
    EtaTracker progress;
    UiModel model;
    model.cnf = &cnf;
    model.prop = &solver.master();
    model.numVars = cnf.numVars;
    model.numClauses = solver.searchClauses();
    model.attempts = opt.attempts;
    model.sigLen = opt.sigLen;
    model.initk = opt.initk;
    model.mink = opt.mink;
    model.focusBits = opt.focusBits;
    model.threads = opt.threads > 0 ? opt.threads
                                    : static_cast<int>(std::thread::hardware_concurrency());

    ResourceSnapshot lastRes;
    uint32_t lastAttempt = 0;
    uint64_t lastResNs = 0;
    uint64_t lastLogNs = 0;

    solver.setProgressCallback([&](const char* phase) {
        const uint64_t now = nowNs();
        const double elapsed = static_cast<double>(now - t0) * 1e-9;
        const SolveStats& st = solver.stats();
        const int assigned = static_cast<int>(solver.master().assignedCount());
        // A restart throws the assignment away, so the history behind the
        // estimate has to go with it.
        if (st.attempt != lastAttempt) {
            lastAttempt = st.attempt;
            progress.reset();
        }
        progress.add(elapsed, assigned, cnf.numVars);

        if (now - lastResNs > 400ull * 1000ull * 1000ull) {
            lastRes = rm.sample();
            lastResNs = now;
        }

        model.phase = phase;
        model.attempt = static_cast<int>(st.attempt);
        model.assignedVars = assigned;
        model.satClauses = solver.master().satisfiedClauses();
        model.elapsed = elapsed;
        model.varsPerSec = progress.rate();
        model.eta = progress.eta();
        model.probes = st.probes;
        model.productive = st.productiveProbes;
        model.rejected = st.rejectedResults;
        model.resamples = st.resamples;
        model.cdclPhases = st.cdclPhases;
        model.cdclConflicts = st.cdclConflicts;
        model.cdclImplied = st.cdclImplied;
        model.restarts = st.restarts;
        model.validSamples = st.validSamples;
        model.totalSamples = st.totalSamples;
        model.focusBits = static_cast<int>(st.focusBits);
        model.focusLanes = st.focusLanes;
        model.focusRounds = st.focusRounds;
        model.sigBytes = st.signatureBytes;
        model.inputVarCount = static_cast<int>(solver.inputVars().size());
        model.gates = st.gates;
        model.unexplained = st.unexplained;
        model.gateSampling = st.gateSampling;
        model.res = lastRes;

        if (ui.enabled()) {
            ui.render(model);
        } else if (!opt.quiet && now - lastLogNs > 2000ull * 1000ull * 1000ull) {
            lastLogNs = now;
            std::printf("[%s] %s vars %d/%d  clauses %u/%zu  probes %llu  attempt %u\n",
                        formatDuration(elapsed).c_str(), phase, assigned, cnf.numVars,
                        solver.master().satisfiedClauses(), solver.searchClauses(),
                        static_cast<unsigned long long>(st.probes), st.attempt);
            std::fflush(stdout);
        }
    });

    out.result = solver.solve();
    out.inputVars = solver.inputVars().size();
    out.wallSeconds = static_cast<double>(nowNs() - t0) * 1e-9;
    ui.end();

    if (out.result.status == SolveStatus::Solved) {
        // Independent re-check of the produced assignment against the file.
        out.verified = true;
        for (size_t c = 0; c < cnf.clauseCount() && out.verified; ++c) {
            bool sat = false;
            const Lit* b = cnf.clauseBegin(c);
            for (uint32_t k = 0, e = cnf.clauseLen(c); k < e; ++k) {
                const int8_t v = out.result.assignment[static_cast<size_t>(b[k] >> 1)];
                if ((b[k] & 1) ? v < 0 : v > 0) { sat = true; break; }
            }
            out.verified = sat;
        }
        if (out.verified) {
            out.solutionPath = opt.solutionPath.empty() ? solutionPathFor(path) : opt.solutionPath;
            std::string werr;
            if (!writeSolutionCnf(out.solutionPath, path, out.result.assignment, cnf.numVars, werr)) {
                std::fprintf(stderr, "warning: %s\n", werr.c_str());
                out.solutionPath.clear();
            }
        } else {
            out.result.status = SolveStatus::Error;
            out.result.message = "the produced assignment does not satisfy the formula";
        }
    }
    return out;
}

void printSummary(const RunOutcome& r, const std::string& path) {
    const SolveStats& s = r.result.stats;
    std::printf("\n");
    std::printf("  instance     %s\n", path.c_str());
    std::printf("  size         %zu variables, %zu clauses\n", r.numVars, r.numClauses);
    std::printf("  status       %s%s\n", toString(r.result.status),
                r.result.status == SolveStatus::Solved && r.verified ? " (verified)" : "");
    if (!r.result.message.empty()) {
        std::printf("  detail       %s\n", r.result.message.c_str());
    }
    std::printf("  assigned     %llu / %zu variables\n",
                static_cast<unsigned long long>(s.assignedVars), r.numVars);
    std::printf("  attempts     %u (restarts %u)\n", s.attempt, s.restarts);
    if (s.totalSamples == 0) {
        std::printf("  circuit      sampling not needed or not started\n");
    } else if (s.gateSampling) {
        std::printf("  circuit      %llu gates recovered, samples executed\n",
                    static_cast<unsigned long long>(s.gates));
    } else if (s.gates > 0) {
        std::printf("  circuit      %llu gates recovered but %llu clauses unexplained, "
                    "samples built by propagation\n",
                    static_cast<unsigned long long>(s.gates),
                    static_cast<unsigned long long>(s.unexplained));
    } else {
        std::printf("  circuit      no gate structure found, samples built by propagation\n");
    }
    if (s.focusBits > 0) {
        std::printf("  focus        %llu target bits, %llu / %llu lanes reproduce them "
                    "after %llu redraws\n",
                    static_cast<unsigned long long>(s.focusBits),
                    static_cast<unsigned long long>(s.focusLanes),
                    static_cast<unsigned long long>(s.totalSamples),
                    static_cast<unsigned long long>(s.focusRounds));
    }
    std::printf("  samples      %llu / %llu lanes in %.2fs\n",
                static_cast<unsigned long long>(s.validSamples),
                static_cast<unsigned long long>(s.totalSamples), s.sampleSeconds);
    std::printf("  resamples    %llu\n", static_cast<unsigned long long>(s.resamples));
    if (s.unusedVars) {
        std::printf("  unused vars  %llu assigned up front, mentioned by no clause\n",
                    static_cast<unsigned long long>(s.unusedVars));
    }
    std::printf("  probes       %llu (productive %llu, rejected %llu)\n",
                static_cast<unsigned long long>(s.probes),
                static_cast<unsigned long long>(s.productiveProbes),
                static_cast<unsigned long long>(s.rejectedResults));
    std::printf("  cdcl         %llu phases, %llu conflicts, %llu literals proved, "
                "%llu clauses learned\n",
                static_cast<unsigned long long>(s.cdclPhases),
                static_cast<unsigned long long>(s.cdclConflicts),
                static_cast<unsigned long long>(s.cdclImplied),
                static_cast<unsigned long long>(s.cdclLearned));
    if (s.gf2Equations > 0) {
        std::printf("  gf2          %llu xor equations over %llu vars, %llu passes in %.2fs, "
                    "%llu literals proved, %llu equivalences (%llu clauses added)\n",
                    static_cast<unsigned long long>(s.gf2Equations),
                    static_cast<unsigned long long>(s.gf2Vars),
                    static_cast<unsigned long long>(s.gf2Runs), s.gf2Seconds,
                    static_cast<unsigned long long>(s.gf2Units),
                    static_cast<unsigned long long>(s.gf2Equivs),
                    static_cast<unsigned long long>(s.gf2Clauses));
    }
    std::printf("  signatures   %llu verdicts, %llu probes short of samples\n",
                static_cast<unsigned long long>(s.signatureVerdicts),
                static_cast<unsigned long long>(s.signatureBails));
    std::printf("  time         %.2fs total\n", r.wallSeconds);
    if (!r.solutionPath.empty()) {
        std::printf("  solution     %s\n", r.solutionPath.c_str());
    }
}

int runBenchmark(const Options& opt) {
    std::error_code ec;
    if (!fs::is_directory(opt.benchmarkDir, ec)) {
        std::fprintf(stderr, "not a directory: %s\n", opt.benchmarkDir.c_str());
        return 1;
    }
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(opt.benchmarkDir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string p = entry.path().string();
        if (p.size() < 4) continue;
        if (p.compare(p.size() - 4, 4, ".cnf") != 0) continue;
        if (p.find(".solution.") != std::string::npos) continue;
        files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "no .cnf files in %s\n", opt.benchmarkDir.c_str());
        return 1;
    }

    struct Row {
        std::string name;
        size_t vars = 0, clauses = 0;
        std::string status;
        uint32_t attempts = 0;
        double sample = 0, total = 0;
        uint64_t probes = 0;
    };
    std::vector<Row> rows;
    rows.reserve(files.size());

    std::printf("TurboCryptoSAT benchmark - %zu instances from %s\n\n", files.size(),
                opt.benchmarkDir.c_str());

    // The per-instance progress log would break up the table, so silence it and
    // let the runner print one line per instance instead.
    Options runOpt = opt;
    runOpt.quiet = true;
    runOpt.ui = false;

    int solved = 0;
    double totalTime = 0.0;
    bool interrupted = false;
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string name = fs::path(files[i]).filename().string();
        std::printf("[%2zu/%2zu] %-34s ", i + 1, files.size(), name.c_str());
        std::fflush(stdout);

        RunOutcome r = runInstance(runOpt, files[i], false);
        Row row;
        row.name = name;
        row.vars = r.numVars;
        row.clauses = r.numClauses;
        row.status = toString(r.result.status);
        if (r.result.status == SolveStatus::Solved && !r.verified) row.status = "BAD";
        row.attempts = r.result.stats.attempt;
        row.sample = r.result.stats.sampleSeconds;
        row.total = r.wallSeconds;
        row.probes = r.result.stats.probes;
        rows.push_back(row);

        if (r.result.status == SolveStatus::Solved && r.verified) ++solved;
        totalTime += r.wallSeconds;
        std::printf("%-12s %8.2fs\n", row.status.c_str(), row.total);
        std::fflush(stdout);

        if (interruptRequested()) {
            std::printf("\ninterrupted\n");
            interrupted = true;
            break;
        }
    }

    std::printf("\n");
    std::printf("+------------------------------------+---------+---------+------------+-----+----------+----------+------------+\n");
    std::printf("| instance                           |    vars | clauses | status     | att |  sample  |   total  |     probes |\n");
    std::printf("+------------------------------------+---------+---------+------------+-----+----------+----------+------------+\n");
    for (const Row& r : rows) {
        std::string n = r.name;
        if (n.size() > 34) n = n.substr(0, 31) + "...";
        std::printf("| %-34s | %7zu | %7zu | %-10s | %3u | %7.2fs | %7.2fs | %10llu |\n",
                    n.c_str(), r.vars, r.clauses, r.status.c_str(), r.attempts, r.sample,
                    r.total, static_cast<unsigned long long>(r.probes));
    }
    std::printf("+------------------------------------+---------+---------+------------+-----+----------+----------+------------+\n");
    std::printf("\nsolved %d / %zu instances in %.2fs\n", solved, files.size(), totalTime);
    if (interrupted) {
        // Comparing against the rows that did run would call a suite abandoned
        // after one instance a clean pass, which is exactly the reading an
        // automated caller takes from exit 0.
        std::printf("interrupted after %zu of %zu instances; %zu were not run\n", rows.size(),
                    files.size(), files.size() - rows.size());
        return 130;
    }
    return solved == static_cast<int>(files.size()) ? 0 : 2;
}

}  // namespace
}  // namespace tcs

int main(int argc, char** argv) {
    using namespace tcs;

    // Randomised property tests over the CDCL contract and the propagator. Not
    // part of the benchmark: it checks claims a solved instance cannot show.
    if (argc >= 2 && std::strcmp(argv[1], "selftest") == 0) {
        int rounds = 0;
        uint64_t seed = 1;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
                seed = std::strtoull(argv[++i], nullptr, 10);
            } else {
                rounds = std::atoi(argv[i]);
            }
        }
        return runSelfTest(seed, rounds);
    }

    if (argc >= 2 && std::strcmp(argv[1], "gen-benchmark") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "gen-benchmark needs an output directory\n");
            return 1;
        }
        std::error_code ec;
        fs::create_directories(argv[2], ec);
        return generateBenchmarkSuite(argv[2]) ? 0 : 1;
    }

    Options opt;
    int exitCode = 0;
    if (!parseArgs(argc, argv, opt, exitCode)) return exitCode;

    enableAnsi();
    installInterruptHandlers();

    int rc = 0;
    if (!opt.tunePath.empty()) {
        rc = runTune(opt);
    } else if (!opt.benchmarkDir.empty()) {
        rc = runBenchmark(opt);
    } else {
        if (opt.verbose) {
            std::printf("TurboCryptoSAT %s\n", kVersion);
            std::printf("instance %s | siglen %d | initk %d | mink %d | attempts %d | threads %d\n",
                        opt.cnfPath.c_str(), opt.sigLen, opt.initk, opt.mink, opt.attempts,
                        opt.threads > 0 ? opt.threads
                                        : static_cast<int>(std::thread::hardware_concurrency()));
            std::printf("mink-unit %s | probe-vars %d | probe-order %s | focus %d\n",
                        opt.minkWords ? "words" : "samples", opt.probeVars,
                        opt.probeDescending ? "descending" : "ascending", opt.focusBits);
        }
        RunOutcome r = runInstance(opt, opt.cnfPath, true);
        if (!opt.quiet) {
            printSummary(r, opt.cnfPath);
        } else {
            std::printf("%s %s\n", toString(r.result.status), opt.cnfPath.c_str());
        }
        switch (r.result.status) {
            case SolveStatus::Solved: rc = 0; break;
            case SolveStatus::Unsatisfiable: rc = 20; break;
            case SolveStatus::Interrupted: rc = 130; break;
            case SolveStatus::Timeout: rc = 124; break;
            default: rc = 10; break;
        }
    }

    shutdownInterruptHandlers();
    return rc;
}
