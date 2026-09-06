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

1. **Sample.** The input variables of the encoded circuit are given random values and the rest
   of the assignment is derived — 65 536 times in parallel, every variable holding a bit vector
   of `sigLen` 64-bit lanes. The result is a population of complete assignments that all satisfy
   the circuit, but not the target output valuation. Where the formula turns out to *be* a
   circuit, the gates are read back out of the clauses and simply executed; otherwise the
   population is built by bit-parallel unit propagation. See
   [Sampling by executing the circuit](#sampling-by-executing-the-circuit).
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
   ------------>  |  recover the gates from the clauses          |
                  |  + random values for the free inputs         |
                  |  + one bit-parallel pass in topological order |
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

### Sampling by executing the circuit

The population is the expensive part of a run: it is built before the first probe, again on
every restart, and again whenever the solver stalls and decides fresh randomness is a better
answer than a guess. Built by unit propagation it means sweeping the whole formula until every
lane settles — on 17-round SHA-256 (24 765 variables, 82 564 clauses) about **1.3 s** per
population, which is enough to make redrawing one a decision rather than a reflex.

A Tseitin-encoded circuit does not have to be propagated, though. It can be *run*. So before
solving, the clauses are matched against the two definition shapes an AND/OR/XOR encoder emits:

```
o == x & y     (~o | x), (~o | y), (o | ~x | ~y)
o == x ^ y     the four ternary clauses over {o,x,y} that forbid one parity
```

Both polarities of the output are tried, so OR, NAND, NOR and XNOR are these same two patterns
with literals negated. What is found is then ordered: a variable no definition claims becomes a
free input, and a definition is accepted once all of its inputs are known, which yields a
topological order — and breaks a cycle by freeing a variable rather than closing it.

Generating a population is then a single forward pass: random words into the free variables,
one `&` or `^` per gate per 64-lane word, nothing revisited. No conflicts, no retry rounds, and
no assigned mask, since executing a circuit leaves nothing undecided — every lane is a valid
sample by construction, and the table needs half the memory.

| `24-sha256-r17-c03.cnf`, 32 threads | propagating | executing |
| --- | --- | --- |
| first population | 1.3 s | 0.18 s |
| every redraw after it | 1.3 s | 0.03 s |
| valid lanes | 65 536 | 65 536 |

The fast path is used only when the recovered network accounts for **every** clause, so that
any valuation of the free variables extends to a satisfying assignment. On top of that, the
first population it produces is checked clause by clause across all 65 536 lanes — that check
is the 0.15 s difference between the two rows above, and it only has to run once, since the
gate list is the same on every redraw. If a single lane fails it, the recovered network is not
the formula after all: the fast path is dropped for the rest of the run and the propagating
generator takes over. It also steps aside when `--outputs` pins a gate output, because
honouring that means constraining the samples rather than merely running the circuit, and on
anything without recognisable gate structure — random 3-SAT — where the clauses left over
disqualify the network immediately.

The summary line reports which of the two ran:

```
  circuit      24741 gates recovered, samples executed
  circuit      not recovered, samples built by propagation
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

The layout is recomputed on every frame, so resizing the window is picked up within about a
tenth of a second, and the status values switch to compact forms when the pane gets narrow. It is
drawn in the terminal's alternate screen buffer, the way `vim` and `htop` are: the shell's
scrollback is left untouched, the dashboard cannot be scrolled out of place, and only the rows
whose content actually changed are rewritten, which is what keeps it from flickering on
`cmd.exe`. If the console cannot process escape sequences the dashboard turns itself off and
the plain progress log is used instead.

```
 TurboCryptoSAT  |  solving  |  signature propagation
##*...:................................... | STATUS
.......................................... |
.......................................... | attempt      1 / 5
.....:#......................:-....:...... | variables    7587/24765 31%
...........--...-:.................+----+: | [==========                      ]
::....:..:-.++..:#+--++---....:..-+-*-::-* | clauses sat  26468/82564 32%
++++++++....+--+###-::-####*+++*.:..*--+## | [==========                      ]
##########***---.++--#############*#-*+:#+ |
-*#############.+:+-:+*--##############+.. | elapsed      00:00:15
.......................................... | eta          00:06:10
.......................................... | rate         28.3 v/s
.......................................... |
.......................................... | probes       269408 ok94 rj0
...................:..............++:..... | guesses      5  rst 0
..........................-++............. | resamples    1
..................:++:.::::::............. |
...........+****+++--:.........+...-..*+:. | samples      65536/65536
...-###*++++---::::....:-:.:*:++-:--.:+##* | tuning       1024/8/32
*++**+-----:....*++++##++::::.:#########*- | input vars   24
-----.:.-#+++###################*-++-+-.+. |
:#+++*#################*+#++++:+--#+++#### | threads      32
##############*++*++:..+.:#*++*########### | cpu          35 %
 clause map: sorted by length then first variable; # satisfied  . open
 press Ctrl+C or ESC ESC to abort
```

`cpu` is the share of the whole machine, the way a task manager reports it, with the
equivalent number of busy cores beside it when the pane is wide enough. `eta` is filtered
rather than divided straight out of the current rate: propagation assigns variables in
cascades, so an unfiltered estimate swings over two orders of magnitude between frames.

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

It prints a per-instance line as it goes and a summary table at the end. This is the whole
suite on a 16-core / 32-thread desktop, default settings, 60 seconds per instance:

```
+------------------------------------+---------+---------+------------+-----+----------+----------+------------+
| instance                           |    vars | clauses | status     | att |  sample  |   total  |     probes |
+------------------------------------+---------+---------+------------+-----+----------+----------+------------+
| 01-rand3sat-n060.cnf               |      60 |     252 | SOLVED     |   2 |    0.18s |    4.13s |     609056 |
| 02-rand3sat-n100.cnf               |     100 |     420 | SOLVED     |   2 |    0.81s |   12.45s |    1570752 |
| 03-rand3sat-n150.cnf               |     150 |     630 | EXHAUSTED  |   5 |    1.25s |   18.91s |    2404768 |
| 04-rand3sat-n220.cnf               |     220 |     924 | EXHAUSTED  |   5 |    2.12s |   25.19s |    3110496 |
| 05-rand3sat-n320.cnf               |     320 |    1360 | EXHAUSTED  |   5 |    3.42s |   36.55s |    4488992 |
| 06-rand3sat-n450.cnf               |     450 |    1912 | TIMEOUT    |   5 |    5.96s |   60.00s |    7218176 |
| 07-rand3sat-n650.cnf               |     650 |    2769 | TIMEOUT    |   4 |    7.01s |   60.00s |    7040800 |
| 08-rand3sat-n900.cnf               |     900 |    3834 | TIMEOUT    |   3 |    5.36s |   60.00s |    7192800 |
| 09-circuit-i24-g300.cnf            |     322 |     981 | SOLVED     |   1 |    0.32s |    9.59s |    1169376 |
| 10-circuit-i32-g600.cnf            |     625 |    1959 | SOLVED     |   1 |    0.08s |    1.37s |     162496 |
| 11-circuit-i48-g1200.cnf           |    1242 |    3921 | SOLVED     |   1 |    0.17s |    2.75s |     318080 |
| 12-circuit-i64-g2000.cnf           |    2059 |    6571 | SOLVED     |   2 |    1.85s |   14.36s |    1463712 |
| 13-circuit-i96-g3500.cnf           |    3594 |   11420 | SOLVED     |   1 |    0.52s |    4.70s |     505568 |
| 14-circuit-i128-g6000.cnf          |    6125 |   19631 | SOLVED     |   2 |    1.11s |    7.68s |     738656 |
| 15-xorcircuit-i24-g200.cnf         |     223 |     757 | SOLVED     |   1 |    0.06s |    2.05s |     256320 |
| 16-xorcircuit-i32-g400.cnf         |     430 |    1502 | SOLVED     |   1 |    0.07s |    1.30s |     160832 |
| 17-xorcircuit-i48-g800.cnf         |     846 |    3000 | TIMEOUT    |   3 |    1.54s |   60.00s |    7450912 |
| 18-xorcircuit-i64-g1500.cnf        |    1562 |    5586 | TIMEOUT    |   2 |    4.25s |   60.00s |    6144128 |
| 19-xorcircuit-i96-g2500.cnf        |    2590 |    9351 | TIMEOUT    |   1 |    2.78s |   60.00s |    5594560 |
| 20-xorcircuit-i128-g4000.cnf       |    4125 |   14908 | TIMEOUT    |   1 |    8.19s |   60.00s |    4021760 |
| 21-sha256-r08-c03.cnf              |   10490 |   35148 | SOLVED     |   1 |    0.50s |    0.51s |          0 |
| 22-sha256-r11-c03.cnf              |   15177 |   50723 | SOLVED     |   1 |    0.73s |    0.75s |          0 |
| 23-sha256-r14-c03.cnf              |   19894 |   66389 | SOLVED     |   1 |    0.96s |    0.98s |          0 |
| 24-sha256-r17-c03.cnf              |   24765 |   82564 | TIMEOUT    |   1 |    9.46s |   60.01s |    1301856 |
| 25-sha256-r20-c03.cnf              |   29725 |   99060 | TIMEOUT    |   1 |    8.66s |   60.01s |    1121760 |
| 26-sha256-r17-c04.cnf              |   25264 |   84217 | TIMEOUT    |   1 |    9.76s |   60.01s |    1297440 |
+------------------------------------+---------+---------+------------+-----+----------+----------+------------+

solved 13 / 26 instances in 743.30s
```

Read across the families rather than down the rows. The circuits it was built for fall in
seconds. Reduced-round SHA-256 up to 14 rounds is finished by propagation from the pinned
digest before the sample layer is even consulted — note the zero probe count — while 17 and 20
rounds are past what it reaches in a minute; it gets roughly a third of the way and slows down.
The XOR-heavy circuits split sharply: small ones fall, and from 48 inputs up the parity
structure leaves both propagation and the samples with nothing to intersect. Random 3-SAT is
the acknowledged worst case and behaves like it.

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
| Out of memory | The table is `numVars * sigLen * 8` bytes, twice that while a propagated population is being built | Lower `--siglen` |
| `circuit` says `not recovered` on an instance that is one | Some clauses fall outside the two gate patterns, so the population is propagated instead of executed | Nothing to turn; sampling is slower but the result is the same |

## Limitations

* **Satisfiable instances only.** The solver assigns in place and cannot prove unsatisfiability.
  A run that exhausts its attempts reports `EXHAUSTED`, which says nothing about satisfiability.
* **Statistical, not complete.** A committed batch of literals can be wrong. Restarts are the
  recovery mechanism, and they are not guaranteed to succeed.
* **Needs a driving input set.** On instances with no such structure (random k-SAT) the sample
  population collapses and the solver falls back to propagation with guessing.
* **High variance.** Two runs of the same instance with different seeds can land far apart —
  on the harder benchmark rows, `SOLVED` and `EXHAUSTED` are both ordinary outcomes. Fix
  `--seed` when you need a run to be reproducible.
* **Not competitive with CDCL in general.** It trades completeness for a different kind of
  inference, and only pays off where that inference exists: circuits with a small set of free
  inputs and a pinned output.

## Star history

<a href="https://star-history.com/#komorra/TurboCryptoSAT&Date">
  <img alt="Star history chart" src="https://api.star-history.com/svg?repos=komorra/TurboCryptoSAT&type=Date" width="600">
</a>

## License

[MIT](LICENSE)
