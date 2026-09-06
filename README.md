<div align="center">

# TurboCryptoSAT

**A signature-based SAT solver for cryptographic instances.**

Instead of searching, it *watches* 65 536 random executions of the encoded circuit at once
and reads off the implications that unit propagation cannot see.

[![build](https://github.com/komorra/TurboCryptoSAT/actions/workflows/ci.yml/badge.svg)](https://github.com/komorra/TurboCryptoSAT/actions/workflows/ci.yml)
[![license](https://img.shields.io/github/license/komorra/TurboCryptoSAT)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![platforms](https://img.shields.io/badge/platforms-Windows%20x64%20%7C%20Linux-informational)](#building)
[![stars](https://img.shields.io/github/stars/komorra/TurboCryptoSAT?style=flat)](https://github.com/komorra/TurboCryptoSAT/stargazers)
[![last commit](https://img.shields.io/github/last-commit/komorra/TurboCryptoSAT)](https://github.com/komorra/TurboCryptoSAT/commits/main)

</div>

---

## What it does

A CDCL solver explores an assignment tree. TurboCryptoSAT does not: it fills the assignment
**in place**, one batch of implied literals at a time, and never backtracks. What lets it get
away with that is a population of samples.

1. **Sample.** The input variables of the encoded circuit are given random values and unit
   propagation is run — 65 536 times in parallel, every variable holding a bit vector of
   `sigLen` 64-bit lanes. The result is a population of complete assignments that all satisfy
   the circuit, but not the target output valuation.
2. **Probe.** Pick an unassigned variable and propagate both of its polarities. Two sound
   things fall out at once: a polarity whose propagation conflicts is refuted outright, and
   whatever the surviving polarities propagate *in common* holds regardless of which one is
   real — the dilemma rule.
3. **Filter.** Now the samples. Take one random window of the literals already assigned, and
   for each surviving polarity filter the population down to the samples agreeing with the
   window *and* that polarity. Every variable with the same value across all surviving samples
   is a candidate implication.
4. **Intersect.** Keep only what every polarity agrees on, propagate it, and commit. The
   dilemma part is sound; the sample part is a statistical bet.
5. **Repeat** until every variable is assigned, then verify the assignment against the formula.

Because step 4 can be wrong, the solver can paint itself into a corner. When every polarity of
a probe is refuted, the attempt is dead and the solver restarts with a fresh sample population
(five attempts by default). This makes it a tool for **satisfiable** instances — reduced-round
hash preimages, circuit inversion, algebraic attacks — and not a decision procedure.

The one parameter with no safe default is `initk`, the width of the window in step 3. Each
literal in it roughly halves the surviving sample set: too wide and nothing looks constant, too
narrow and merely *biased* variables — an AND deep in a circuit that is almost always 0 — pass
for implied ones and poison the assignment. So the restarts double as a search over it: every
attempt halves `initk`, walking from aggressive to conservative.

```
                     ONCE, BEFORE SOLVING
   instance.cnf   +----------------------------------------------+
   ------------>  |  random values for the input variables       |
                  |  + bit-parallel unit propagation             |
                  |  = signature table: one 65536-bit vector      |
                  |    per variable, sampling the circuit        |
                  +-----------------------+----------------------+
                                          |
                     EVERY ROUND,         |  N threads probe in parallel
   target valuation  ON N THREADS         v
   (unit clauses  +----------------------------------------------+
    or --outputs) |     probe v                  probe not-v      |
   ------------>  |       |  propagate             |  propagate   |
                  |       +- filter samples        +- filter      |
                  |              \                /               |
                  |               what both agree on              |
                  +-----------------------+----------------------+
                                          |
        both polarities refuted           v
        -----> restart attempt   commit + propagate ----> repeat
```

## Building

Requires a C++17 compiler. Windows x64 and Linux are both supported and tested in CI
(MSVC, GCC, Clang).

```bash
git clone https://github.com/komorra/TurboCryptoSAT.git
cd TurboCryptoSAT

# with CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j

# or just
./build.sh          # Linux / macOS / Git Bash
build.bat           # Windows
```

The build scripts fall back to invoking the compiler directly when CMake is not installed.
The result is a single self-contained binary, `build/turbocryptosat`.

## Usage

```
turbocryptosat <instance.cnf> [options]
turbocryptosat benchmark <directory> [options]
turbocryptosat gen-benchmark <directory>
```

The simplest possible run — the target valuation is taken from the unit clauses in the file
and the input variables are detected automatically:

```bash
turbocryptosat instance.cnf
```

Pinning things down explicitly:

```bash
turbocryptosat sha256_17.cnf \
    --inputs 1-24 \                 # message bits are variables 1..24
    --outputs "-513 514 515 ..." \  # digest, as signed DIMACS literals
    --siglen 1024 \                 # 1024 lanes = 65536 samples per variable
    --initk 8 --mink 32 \
    --threads 16 --attempts 5
```

### Options

| Option | Meaning | Default |
| --- | --- | --- |
| `--inputs a-b` \| `auto` | Range of input variables, 1-based and inclusive | `auto` |
| `--outputs "<lits>"` | Target valuation as signed DIMACS literals; repeatable | unit clauses |
| `--outputs-file <f>` | Read the target valuation from a file | — |
| `--out <file>` | Solution path | `<instance>.solution.cnf` |
| `--siglen <n>` | 64-bit lanes per variable; `n * 64` samples | `1024` |
| `--initk <n>` | Literals of the assignment each probe filters the samples with; halved on every restart | `8` |
| `--mink <n>` | Minimum surviving sample words for a signature verdict | `32` |
| `--probe-vars <n>` | Variables probed at once, giving `2^n` branches | `1` |
| `--threads <n>` | Worker threads | hardware threads |
| `--attempts <n>` | Restarts after a conflict | `5` |
| `--stall-limit <n>` | Barren rounds before the samples are redrawn and, failing that, a variable is guessed | `1000` |
| `--sample-rounds <n>` | Retry rounds while building the samples | `12` |
| `--keep-samples` | Reuse the sample population across restarts | off |
| `--timeout <sec>` | Abort after this many seconds | unlimited |
| `--seed <n>` | Fix the random seed for reproducible runs | time-based |
| `--no-ui` / `--quiet` / `--verbose` | Output control | dashboard on a tty |

### Input variables and the target valuation

The two are what make the method work, so it is worth being precise about them.

* **Input variables** drive the sample population. With `auto` the solver runs one scalar
  propagation pass and keeps every variable that was still undetermined when it got there —
  for a Tseitin-encoded circuit that is exactly its inputs. Pass `--inputs a-b` when you know
  the range; it is faster and more accurate.
  On an instance with no such structure — random k-SAT — random values conflict on nearly
  every lane and the population comes back almost empty. That is visible in the summary as a
  low valid-lane count, and the solver keeps going on propagation alone. Sampling is
  interruptible between propagation passes, so it never overruns `--timeout`.
* **The target valuation** is what the solver must satisfy: the pinned outputs. Without
  `--outputs`, the unit clauses of the file are adopted as the target and are therefore *not*
  imposed on the sample population — the samples have to be free executions of the circuit for
  the filtering in step 3 to mean anything. With `--outputs`, unit clauses are treated as
  structural constants instead and do constrain the samples.

### Stopping a run

`Ctrl+C`, or `ESC` twice within a second. The solver finishes the round in flight, restores
the terminal and prints what it had.

### The solution file

After a solved instance is re-verified against the formula, the assignment is written next to
the input as `<instance>.solution.cnf` — a DIMACS CNF of unit clauses, one per variable, so it
can be fed straight back into any SAT tool.

## The dashboard

On an interactive terminal the solver draws a live view sized to the console. The left pane is
a map of every clause in the formula, in a fixed order (by clause length, then by smallest
variable) so a cell always stands for the same clauses; cells go from dark to green as their
clauses become satisfied. The right pane is the run status.

```
 TurboCryptoSAT  |  solving  |  signature propagation
 ###############################+++++++::.....  |  STATUS
 ##########################+++++::::...           |
 #####################+++++:::....                |   attempt      2 / 5
 ################+++++::::...                     |   variables    9218 / 24765  (37.2%)
 ###########++++::::..                            |   [=======               ]
 ######++++::::..                                 |   clauses sat  31585 / 82564  (38.3%)
 ###++++:::...                                    |   [========              ]
 ...                                              |   elapsed      00:01:26
                                                  |   eta          00:03:41
                                                  |   rate         42.7 vars/s
                                                  |   probes       2752992 (ok 1446, rej 0)
                                                  |   guesses      1073    restarts 3
                                                  |   samples      65536 / 65536 lanes
                                                  |   tuning       sigLen 1024 initk 8 mink 32
                                                  |   threads      16
                                                  |   cpu          1489 %
                                                  |   memory       412 MB / 32 GB
 clause map: sorted by length then first variable; # satisfied . open
 press Ctrl+C or ESC ESC to abort
```

Use `--no-ui` for a plain progress log (CI, redirected output) or `--quiet` for the verdict only.

## Benchmarks

The repository ships 26 satisfiable instances in [`benchmark/`](benchmark), from trivial to well
out of reach, in four families:

| Instances | Family | What it stresses |
| --- | --- | --- |
| `01`–`08` | Planted random 3-SAT, n = 60…900 at ratio ≈ 4.2 | The worst case for this solver: no input set drives the formula, so the sample population is nearly empty and it degrades to propagation plus guessing |
| `09`–`14` | Random mixed AND/OR/XOR circuits, 24…128 inputs | Circuit inversion with only the output layer pinned |
| `15`–`20` | XOR-heavy random circuits, 24…128 inputs | Parity structure, which unit propagation handles badly |
| `21`–`26` | Reduced-round SHA-256 preimages, 8…20 rounds, 3–4 byte messages | The intended target |

Every instance is satisfiable by construction (the formula is built around a planted
assignment) and reproducible — `turbocryptosat gen-benchmark <dir>` regenerates the identical
set from fixed seeds.

Run the whole suite:

```bash
./benchmark.sh                    # Linux / macOS / Git Bash
benchmark.bat                     # Windows
./benchmark.sh --timeout 600      # extra flags go straight to the solver
```

or point the solver at any directory of `.cnf` files:

```bash
turbocryptosat benchmark path/to/instances --no-ui --timeout 300
```

It prints a per-instance line as it goes and a summary table at the end:

```
+------------------------------------+---------+---------+------------+-----+----------+----------+------------+
| instance                           |    vars | clauses | status     | att |  sample  |   total  |     probes |
+------------------------------------+---------+---------+------------+-----+----------+----------+------------+
| 09-circuit-i24-g300.cnf            |     322 |     981 | SOLVED     |   1 |    0.01s |    0.17s |      25344 |
| 24-sha256-r17-c03.cnf              |   24765 |   82564 | SOLVED     |   2 |    2.31s |   94.60s |    1204832 |
+------------------------------------+---------+---------+------------+-----+----------+----------+------------+
```

### Tests

The benchmark is also the test suite. Every solved instance is re-checked clause by clause
against the file it came from, independently of the solver's own bookkeeping; an assignment
that does not satisfy the formula is reported as `BAD` rather than `SOLVED`, so a wrong answer
fails the run rather than passing quietly. CI builds on Linux (GCC and Clang) and Windows
(MSVC), regenerates the whole suite to check the generator is deterministic, and runs a short
benchmark pass. Locally:

```bash
ctest --test-dir build          # smoke tests
./benchmark.sh --timeout 30     # the full suite on a short budget
```

## Tuning

The summary after every run is meant to be read as a diagnosis.

| Symptom | What it means | Knob |
| --- | --- | --- |
| `samples` shows far fewer valid lanes than total | Random values for the input set conflict on most lanes | `--inputs`, or raise `--sample-rounds` |
| `input variables` is a large fraction of all variables | Auto-detection found no small driving set | `--inputs` |
| Many probes, few productive, high `probes short of samples` | The filter is too narrow and keeps bailing out | Lower `--initk` or raise `--siglen` |
| Many probes, few productive, low `probes short of samples` | The filter is too wide: thousands of samples survive and nothing looks constant | Raise `--initk` |
| Restarts early and often | Statistical verdicts are firing on biased variables | Raise `--siglen`, or start from a lower `--initk` |
| Progress stalls, few guesses | Nothing left for the probes to find | Raise `--probe-vars` to 2, or lower `--stall-limit` |
| Out of memory | The table is `numVars * sigLen * 8` bytes, twice that while it is being built | Lower `--siglen` |

## Limitations

* **Satisfiable instances only.** The solver assigns in place and cannot prove unsatisfiability.
  A run that exhausts its attempts reports `EXHAUSTED`, which says nothing about satisfiability.
* **Statistical, not complete.** A committed batch of literals can be wrong. Restarts are the
  recovery mechanism, and they are not guaranteed to succeed.
* **Needs a driving input set.** On instances with no such structure (random k-SAT) the sample
  population collapses and the solver falls back to propagation with guessing.

## Star history

<a href="https://star-history.com/#komorra/TurboCryptoSAT&Date">
  <img alt="Star history chart" src="https://api.star-history.com/svg?repos=komorra/TurboCryptoSAT&type=Date" width="600">
</a>

## License

[MIT](LICENSE)
