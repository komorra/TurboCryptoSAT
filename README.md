<div align="center">

# TurboCryptoSAT

**A signature-based SAT solver for cryptographic instances.**

Instead of searching, it *watches* 65 536 random executions of the encoded circuit at once
and reads off the implications that unit propagation cannot see.

[![build](https://github.com/komorra/TurboCryptoSAT/actions/workflows/ci.yml/badge.svg)](https://github.com/komorra/TurboCryptoSAT/actions/workflows/ci.yml)
[![license](https://img.shields.io/github/license/komorra/TurboCryptoSAT)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![platforms](https://img.shields.io/badge/platforms-Windows%20x64%20%7C%20Linux-informational)](#building)
[![stars](https://img.shields.io/github/stars/komorra/TurboCryptoSAT?style=flat&label=stars)](https://github.com/komorra/TurboCryptoSAT/stargazers)
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

A branch whose sample-derived literals conflict is discarded from the intersection, even when
plain propagation accepts it. Signature implications are accepted as assignments; their errors
can therefore eliminate a viable branch. The user controls this statistical tradeoff through
`focus`, `mink`, `initk` and the other sampling parameters.
The contiguous filter window stops at the end of the ordered assignment, as in the
C# prototype; it does not wrap from the last variable back to the first.

Because step 4 can be wrong, the solver can paint itself into a corner. When every polarity of
a probe is refuted, the attempt is dead and the solver restarts with a fresh sample population
(five attempts by default). This makes it a tool for **satisfiable** instances — reduced-round
hash preimages, circuit inversion, algebraic attacks — and not a decision procedure.

When the probes stop agreeing on anything but nothing has been refuted either, the run is on a
plateau rather than in a corner. The answer to that is first a fresh sample population and then
a **bounded CDCL phase** — see [Plateaus](#plateaus). Nothing about it is a guess: it hands
back only literals it has proved.

The one parameter with no safe default is `initk`, the width of the window in step 3. Each
literal in it roughly halves the surviving sample set: too few literals and nothing looks constant, too
many and merely *biased* variables — an AND deep in a circuit that is almost always 0 — pass
for implied ones and poison the assignment. So the restarts double as a search over it: every
attempt halves `initk`, walking from aggressive to conservative.

```
                     ONCE, BEFORE SOLVING
   instance.cnf   +----------------------------------------------+
   ------------>  |  recover the gates from the clauses          |
                  |  + random values for the free inputs         |
                  |  + one bit-parallel pass, topological order  |
                  |  = signature table: one 65536-bit vector     |
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
                                          |
                     NOTHING COMMITTED    |  for stallLimit rounds running
                     FOR A WHILE          v
                  +----------------------------------------------+
                  |  redraw the samples, and failing that:        |
                  |  CDCL over the same formula, everything       |
                  |  committed so far pinned at level 0           |
                  |    -> a learned unit  = proved, commit it     |
                  |    -> a full model    = solved                |
                  |    -> level 0 conflict = attempt is dead      |
                  |    -> budget spent    = double it, keep going |
                  +----------------------------------------------+
```

### Sampling by executing the circuit

The population is the expensive part of a run: it is built before the first probe, again on
every restart, and again whenever the solver stalls and decides fresh randomness is the
cheapest way out. Built by unit propagation it means sweeping the whole formula until every
lane settles — on 17-round SHA-256 (24 765 variables, 82 564 clauses) about **0.25 s** per
population across 32 threads, which is enough to make redrawing one a decision rather than a
reflex.

A Tseitin-encoded circuit does not have to be propagated, though. It can be *run*. So before
solving, the clauses are matched against the three definition shapes an AND/OR/XOR/NOT encoder
emits:

```
o == x & y     (~o | x), (~o | y), (o | ~x | ~y)
o == ~x        (~o | ~x), (o | x)          -- and o == x, the same shape
o == x ^ y     the four ternary clauses over {o,x,y} that forbid one parity
```

Both polarities of the output are tried, so OR, NAND, NOR and XNOR are these same patterns with
literals negated. The middle one exists because encoders differ on NOT: some fold it into the
literal and never give it a variable (this repository's generator does), others emit it as a
gate of its own — and an unmatched inverter does not just cost its own two clauses, it hides
every gate that is only reachable through it. What is found is then ordered: a variable no
definition claims becomes a free input, and a definition is accepted once all of its inputs are
known, which yields a topological order — and breaks a cycle by freeing a variable rather than
closing it.

Generating a population is then a single forward pass: random words into the free variables,
one `&` or `^` per gate per 64-lane word, nothing revisited. No conflicts, no retry rounds, and
no assigned mask, since executing a circuit leaves nothing undecided — every lane is a valid
sample by construction, and the table needs half the memory.

| `24-sha256-r17-c03.cnf`, 32 threads | propagating | executing |
| --- | --- | --- |
| first population | 0.33 s | 0.08 s |
| every redraw after it | 0.25 s | 0.04 s |
| valid lanes | 65 536 | 65 536 |

The fast path is used only when the recovered network accounts for **every** clause, so that
any valuation of the free variables extends to a satisfying assignment. On top of that, the
first population it produces is checked clause by clause across all 65 536 lanes — that check
is the difference between the two rows above, and it only has to run once, since the
gate list is the same on every redraw. If a single lane fails it, the recovered network is not
the formula after all: the fast path is dropped for the rest of the run and the propagating
generator takes over. It also steps aside when `--outputs` pins a gate output, because
honouring that means constraining the samples rather than merely running the circuit, and on
anything without recognisable gate structure — random 3-SAT — where the clauses left over
disqualify the network immediately.

Either generator can be cut short by `--timeout` or `Ctrl+C`. The lanes finished so far are
kept and the rest simply come back invalid, so a large `--siglen` cannot hold a run past its
deadline. An interrupted check is not read as the network failing — only a check that ran to
the end can retire the fast path.

The summary line reports which of the two ran, and when the network was rejected, by how much:

```
  circuit      24741 gates recovered, samples executed
  circuit      115247 gates recovered but 18749 clauses unexplained, samples built by propagation
  circuit      no gate structure found, samples built by propagation
```

The middle line is the one to read closely. A handful of unexplained clauses on a formula that
is a circuit means one pattern this matcher does not know, and the whole fast path is lost to
it.

### Focusing the population on the target

A free execution of the circuit reproduces the target valuation only by accident, and on a hash
preimage that accident has probability `2^-256`. So the population the filter draws from is, by
default, a set of executions that agree with the target on nothing at all. `--focus <n>` narrows
it: the first `n` literals of the target valuation are imposed on every lane, and a lane that
misses one is thrown back and redrawn until it hits.

```
--focus 0     samples are free executions of the circuit          (the original behaviour)
--focus 8     every sample reproduces 8 bits of the target        (the default)
--focus 24    every sample reproduces 24 bits of the target       (expensive; see below)
```

The loop is the reference implementation's `ComputeEx`, done a word at a time. A lane that has
landed on the target is finished: every later redraw is masked off it, so the population
converges lane by lane rather than being discarded and regenerated whole. Lanes are redrawn 64
at a time to keep the bit parallelism, and a 64-lane word whose every lane has landed drops out
of the loop entirely. What that costs is therefore the expected *worst* of 64 geometric draws
per word — around `4.7 * 2^n` redraws of a word — rather than `2^n` redraws of the whole
population:

| `24-sha256-r17-c03.cnf`, 32 threads | `--focus 0` | `--focus 8` |
| --- | --- | --- |
| population built in | 0.29 s | 17.9 s |
| redraws dispatched | — | 2 720 |
| valid lanes | 65 536 | 65 536 |

That is the shape of the knob: every bit doubles the price. The dashboard grows a `focus` row
while the loop runs, and the phase in the header reads `focusing`, because on a large instance
this is a preprocessing pass long enough to look like a hang otherwise:

```
  focus        8 bits  41216 / 65536 lanes  1184 redraws
  [====================                    ]
```

Two things bound it. A target bit that lands on a *free* variable is not sampled for at all —
the variable is simply pinned, exactly as a unit clause is — so only bits a gate defines reach
the rejection loop. And the loop is interruptible: `--timeout` and `Ctrl+C` stop it, and every
lane that had not landed by then is struck from the valid mask rather than left in the
population unfocused. Asking for more bits than the budget can pay for therefore yields a
smaller population, never a wrong one:

```
  focus        24 target bits, 17 / 65536 lanes reproduce them after 1536 redraws
  samples      17 / 65536 lanes in 14.98s
```

On an instance with no usable gate network the rejection loop has nothing to execute, and the
target bits are handed to the propagating generator as ordinary fixed literals instead: lanes
that cannot be extended to honour them conflict and drop out. That reaches the same population
far less efficiently, and on outputs this hard to hit, an almost empty one — which the lane
count says plainly.

#### What it is worth

Solve counts say nothing here — the shipped suite is mostly instances that finish in under a
second or not within a minute, and both are blind to this. **Time to solve is the measurement**,
three seeds per cell, 60 s cap:

| instance | `--focus 0` | `--focus 8` |
| --- | --- | --- |
| `19-xorcircuit-i96-g2500` | 3.07 s, 7.70 s, 7.00 s | **0.83 s, 1.21 s, 4.86 s** |
| `14-circuit-i128-g6000` | 4.61 s, 23.88 s, 5.61 s | 6.03 s, 7.32 s, 5.11 s |
| `23-sha256-r14-c03` | **0.08 s, 0.08 s, 0.08 s** | 11.46 s, 12.12 s, 11.86 s |
| `20-xorcircuit-i128-g4000` | timeout | timeout |
| `26-sha256-r17-c04` | timeout | timeout |

The XOR circuit family is where it earns its keep: three to six times faster, and the slow seed
comes down with the fast ones. That is the case the mechanism was built for — long parity chains
whose intermediates nothing else pins down, where a population that already agrees with the
target on eight bits carries information a free population does not.

These measurements predate the root-model shortcut. The solver now checks for a complete,
verified model after unit propagation and again after GF(2), before allocating signatures or
focusing the population. `23-sha256-r14-c03` therefore skips sampling entirely. Instances that
still need search pay the focus cost; `--focus 0` disables it.

The `14-circuit` row is the honest middle: no better on average, but the 23.88 s seed is gone.
Narrowing the population trades a chance of a very good draw for a narrower spread of draws.

### Plateaus

A probe is barren when both polarities survive and their outcomes intersect down to nothing new.
That is not a conflict — the assignment may well still be extendable — so restarting would throw
away good work. After `--stall-limit` barren rounds in a row (1000 by default) the solver
responds in two steps.

First it redraws the sample population, which is the cheap and entirely safe move: a different
65 536 samples give the filter different constants to find. That is tried twice, and only while
sampling stays under 15 % of elapsed time and would not overshoot `--timeout` — a redraw is
interruptible only *between* its retry rounds, so its cost has to be budgeted before it starts.

If a fresh population does not help either, the solver runs a **bounded CDCL phase**. A
conventional conflict-driven search — two watched literals, 1UIP learning, VSIDS, phase saving,
Luby restarts — runs over the same formula with every literal the signature loop has committed
pinned at level 0. The phase ends on the first of:

* **a new literal on the level 0 trail** — a learned unit clause. It follows from the formula
  and the pinned assignment alone, so it is committed like any sound inference;
* **a full model** — the instance is finished outright;
* **a conflict at level 0** — the pinned assignment is refutable, so the attempt is dead and the
  solver restarts;
* **the conflict budget** (`--cdcl-conflicts`, 10 000 per phase). Nothing was proved, so the
  budget doubles and the probes carry on. Learned clauses survive between phases within an
  attempt, so the next phase resumes where this one stopped rather than starting cold. They are
  dropped on a restart, since they are implied by the assignment the restart retracts.

This replaces what used to happen here: picking the most lopsided variable in the sample
population and assigning it its majority value. That was a bet the solver could not take back,
and on the instances where it fired most — random k-SAT, where the population carries no signal
at all — it was close to a coin flip. `--no-cdcl` restores the plateau to resampling and further
probing only, which is useful mainly for measuring what the statistical layer does on its own.

### Linear reasoning over the parities

A Tseitin encoder writes `a ^ b ^ c = 1` as four ternary clauses, and unit propagation over
those four is exactly as strong as the constraint: it fires when two of the three are known and
not before. What it cannot do is *add* two constraints together. Chains of parities are the
shape of a hash round function, so `gf2.cpp` recovers every XOR the clauses encode and runs
Gaussian elimination over them. A row that reduces to one variable is a proven literal, one that
reduces to two is an equivalence, and one that reduces to `0 = 1` refutes the assignment. All
three are consequences of the formula — the layer contributes no bets. It runs once before the
search, then on a stride of `--gf2-interval` new assignments and at every plateau; a pass costs
single-digit milliseconds even at 20 rounds.

**What it actually bought, measured.** Not what the idea promises, so the numbers are worth
stating plainly. At 17 rounds the system is real — 8085 equations over 12 752 of the 24 765
variables — and reduces correctly. But it proves **zero units**, and of the 715 equivalences it
derives, **712 are things unit propagation already reaches**:

| instance | equations | equivalences | already reachable by BCP | genuinely new |
| --- | ---: | ---: | ---: | ---: |
| `24-sha256-r17-c03` | 8085 | 715 | 712 | **3** |
| `25-sha256-r20-c03` | 9701 | 619 | 617 | **2** |
| `26-sha256-r17-c04` | 8265 | 601 | 598 | **3** |
| `19-xorcircuit-i96-g2500` | 1773 | 138 | 124 | **14** |

The reason is structural: these encodings already cut every parity into three-variable gates
with explicit intermediate variables, and propagation walks those intermediates perfectly well.
The classic elimination win needs long parity chains whose intermediates nothing else
constrains, which is why the XOR-circuit family is the only one here with a real share of new
facts.

That measurement changed the design. Adding all 1430 clauses for 3 new facts made every probe
propagate over a bigger formula and measurably *slowed* 17-round SHA-256, so each derived clause
is now checked against propagation first and only kept when it cannot be reached — 6 clauses
instead of 1430. With that filter the layer is a wash on SHA-256: variables assigned in 30 s are
identical on seven of nine instance/seed pairs with `--no-gf2` and without. It is kept on by
default because it is now close to free and it is the right tool for a genuinely parity-heavy
instance, not because it moves this benchmark.

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
turbocryptosat selftest [rounds] [--seed <n>]
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
    --initk 6 --mink 640 \          # 640 surviving samples, not 640 lane words
    --focus 8 \                     # every sample reproduces 8 bits of the digest
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
| `--initk <n>` | Literals of the assignment each probe filters the samples with; halved on every restart | `6` |
| `--mink <n>` | Minimum evidence for a signature verdict, in the unit selected below | `640` |
| `--mink-unit <samples\|words>` | Count individual samples, or nonempty 64-bit words | `samples` |
| `--probe-vars <n>` | Variables probed at once, giving `2^n` branches | `1` |
| `--probe-order <ascending\|descending>` | Direction through occurring variables | `ascending` |
| `--focus <n>` | Bits of the target valuation every sample must reproduce; lanes that miss one are redrawn until they hit, at a cost of about `2^n` redraws | `8` |
| `--threads <n>` | Worker threads | hardware threads |
| `--attempts <n>` | Restarts after a conflict | `5` |
| `--stall-limit <n>` | Barren rounds before the samples are redrawn and, failing that, a CDCL phase runs | `1000` |
| `--cdcl-conflicts <n>` | Conflict budget for one CDCL phase, doubled whenever a phase proves nothing; `0` means bounded only by `--timeout` | `10000` |
| `--no-cdcl` | Never run a CDCL phase; answer plateaus by resampling and further probing only | off |
| `--no-gf2` | Do not recover the XOR constraints or reduce them over GF(2) | off |
| `--gf2-interval <n>` | New assignments between elimination passes; a pass also runs at every plateau | `16` |
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
  `--focus <n>` is the dial between those two extremes: it imposes the first `n` bits of the
  target on the population and leaves the rest free. See
  [Focusing the population on the target](#focusing-the-population-on-the-target).

### Stopping a run

`Ctrl+C`, or `ESC` twice within a second. The solver finishes the round in flight, restores
the terminal and prints what it had.

### Exit codes

| Code | Meaning |
| --- | --- |
| `0` | solved, and the assignment was re-verified against the formula |
| `20` | unsatisfiable — proven by propagation, and only ever a statement about the formula |
| `124` | `--timeout` expired |
| `130` | interrupted |
| `10` | anything else, including a malformed input file or an argument that makes no sense |

A bad `--outputs` literal or a truncated DIMACS file is `10`, never `20`: a caller that reads
an input error as a proof of unsatisfiability would be trusting a result nobody produced.

In benchmark mode `0` means every instance in the directory solved, `2` that some did not, and
`130` that the run was interrupted — the instances that never ran are reported rather than
counted as passes.

### The solution file

After a solved instance is re-verified against the formula, the assignment is written next to
the input as `<instance>.solution.cnf` — a DIMACS CNF of unit clauses, one per variable, so it
can be fed straight back into any SAT tool.

## The dashboard

On an interactive terminal the solver draws a live view sized to the console. The left pane is
a map of the variables: variable 1 sits in the bottom left corner, the numbering runs left to
right along each row and upwards from row to row, so the highest index is in the top right and
every cell covers the same band of variables for the whole run. A cell goes from dark to green
as the clauses its variables occur in become satisfied, which shows *where* in the formula the
run is making ground - on a Tseitin-encoded circuit the numbering follows the circuit, so the
map is roughly a picture of it. A blank cell is a band of variables that occur in no clause at
all. The right pane is the run status.

The layout is recomputed on every frame, so resizing the window is picked up within about a
tenth of a second, and the status values switch to compact forms when the pane gets narrow. It is
drawn in the terminal's alternate screen buffer, the way `vim` and `htop` are: the shell's
scrollback is left untouched, the dashboard cannot be scrolled out of place, and only the rows
whose content actually changed are rewritten, which is what keeps it from flickering on
`cmd.exe`. If the console cannot process escape sequences the dashboard turns itself off and
the plain progress log is used instead.

```
 TurboCryptoSAT  |  solving  |  signature propagation
########################################## | STATUS
*##*#*..-+:.:::+#+:::::-#*+--::-########## |
:##+-+---*#############################*** | attempt      1 / 5
####################****#*#++**#+--#*+++-: | variables    7480/24765 30%
####*++**+++++++:.+#-:...:-##--::--####### | [==========                      ]
.:::.....:##-:-:::######################## | clauses sat  26078/82564 32%
:+########*-:::::-########++++-----+---+#. | [==========                      ]
:##-+-+--+++++--+-+--+--...:.......+-::::: |
:-::--:...........:-......:#*#:-:++*+.:::. | elapsed      00:00:11
.:::......#*:.:..:**:.....+#*-------++---: | eta          00:04:17
..........+-----------::::::::::.......... | rate         28.8 v/s
..::::..............................:..... |
...................................:+:.... | probes       330048 ok66 rj0
...................--:......:..:.......... | resamples    5  rst 0
..:*-..................................... | cdcl         1 c10001 i0
.......................................... |
.......................................... | samples      65536/65536
.......................................... | tuning       1024/6/640
.......................................... | focus        8b 65536/65536 r2720
.......................................... | input vars   24
.......................................... | gates        24741 exec
.......................................... |
.......................................... | threads      32
 variable map: index rises left to right, bottom to top; # satisfied  . open
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
| `01`–`08` | Planted random 3-SAT, n = 60…900 at ratio ≈ 4.2 | The worst case for the statistical layer: no input set drives the formula, so the sample population is nearly empty and every one of these is finished by the CDCL phase |
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
| 01-rand3sat-n060.cnf               |      60 |     252 | SOLVED     |   1 |    0.30s |    0.53s |      32032 |
| 02-rand3sat-n100.cnf               |     100 |     420 | SOLVED     |   1 |    0.07s |    0.31s |      32032 |
| 03-rand3sat-n150.cnf               |     150 |     630 | SOLVED     |   1 |    0.28s |    0.52s |      32032 |
| 04-rand3sat-n220.cnf               |     220 |     924 | SOLVED     |   1 |    0.46s |    0.71s |      32032 |
| 05-rand3sat-n320.cnf               |     320 |    1360 | SOLVED     |   1 |    1.06s |    1.37s |      32032 |
| 06-rand3sat-n450.cnf               |     450 |    1912 | SOLVED     |   1 |    1.88s |    2.60s |      64064 |
| 07-rand3sat-n650.cnf               |     650 |    2769 | SOLVED     |   1 |    6.38s |   47.58s |     256256 |
| 08-rand3sat-n900.cnf               |     900 |    3834 | TIMEOUT    |   1 |    5.08s |   60.00s |     224224 |
| 09-circuit-i24-g300.cnf            |     322 |     981 | SOLVED     |   1 |    0.07s |    0.36s |      32032 |
| 10-circuit-i32-g600.cnf            |     625 |    1959 | SOLVED     |   1 |    0.13s |    0.87s |      80384 |
| 11-circuit-i48-g1200.cnf           |    1242 |    3921 | SOLVED     |   1 |    0.28s |    4.41s |     424320 |
| 12-circuit-i64-g2000.cnf           |    2059 |    6571 | SOLVED     |   1 |    0.10s |    3.28s |     309536 |
| 13-circuit-i96-g3500.cnf           |    3594 |   11420 | SOLVED     |   1 |    0.98s |    6.44s |     475488 |
| 14-circuit-i128-g6000.cnf          |    6125 |   19631 | SOLVED     |   1 |    2.33s |    6.22s |     264736 |
| 15-xorcircuit-i24-g200.cnf         |     223 |     757 | SOLVED     |   1 |    0.13s |    0.42s |      32064 |
| 16-xorcircuit-i32-g400.cnf         |     430 |    1502 | SOLVED     |   1 |    0.57s |    0.86s |      32128 |
| 17-xorcircuit-i48-g800.cnf         |     846 |    3000 | SOLVED     |   1 |    0.22s |    0.56s |      32064 |
| 18-xorcircuit-i64-g1500.cnf        |    1562 |    5586 | SOLVED     |   1 |    0.50s |    0.84s |      32704 |
| 19-xorcircuit-i96-g2500.cnf        |    2590 |    9351 | SOLVED     |   1 |    0.72s |    4.62s |     128128 |
| 20-xorcircuit-i128-g4000.cnf       |    4125 |   14908 | TIMEOUT    |   1 |    2.11s |   60.00s |     288288 |
| 21-sha256-r08-c03.cnf              |   10490 |   35148 | SOLVED     |   1 |    4.56s |    4.58s |          0 |
| 22-sha256-r11-c03.cnf              |   15177 |   50723 | SOLVED     |   1 |    8.95s |    8.98s |          0 |
| 23-sha256-r14-c03.cnf              |   19894 |   66389 | SOLVED     |   1 |   13.77s |   13.80s |          0 |
| 24-sha256-r17-c03.cnf              |   24765 |   82564 | TIMEOUT    |   1 |   16.92s |   60.01s |     182720 |
| 25-sha256-r20-c03.cnf              |   29725 |   99060 | TIMEOUT    |   1 |   21.01s |   60.02s |     219968 |
| 26-sha256-r17-c04.cnf              |   25264 |   84217 | TIMEOUT    |   1 |   15.68s |   60.01s |     287712 |
+------------------------------------+---------+---------+------------+-----+----------+----------+------------+

solved 21 / 26 instances in 409.94s
```

Read across the families rather than down the rows. The circuits it was built for fall in
seconds. Reduced-round SHA-256 up to 14 rounds is finished by propagation from the pinned digest
before the sample layer is even consulted — note the zero probe count — while 17 and 20 rounds
are still past what it reaches in a minute; it gets roughly a third of the way and slows down.

Then read the `sample` column against the `probes` column, because on this table the default
`--focus 8` is the most expensive thing in the suite. Rows `21`–`23` solve with **zero probes**
and still spend 4.6 s, 9.0 s and 13.8 s building a population nothing ever reads; rows `24`–`26`
hand 16–21 s of their 60 s budget to the focus pass, which is why `24-sha256-r17-c03` runs
182 720 probes here against 935 424 with `--focus 0`. The pass is unconditional and runs before
the search does, so the solver cannot know yet that it will not need it. On the SHA family
`--focus 0` is the better setting; the default earns itself back on the XOR circuits, where the
population is what carries the run.

The XOR-heavy rows and the random 3-SAT rows are where the plateau handler shows. Both families
leave the probes with nothing to intersect — the first because parity structure is invisible to
unit propagation, the second because there is no driving input set and the population carries no
signal at all — and both used to end in `TIMEOUT` or `EXHAUSTED` from 48 inputs and n = 150
upwards, after a guess that was close to a coin flip. Under the CDCL phase `03`–`06` finish in
a second or two each and `17`–`19` in under three; `07` lands either side of the limit depending
on the seed, and `08` and `20` still do not. Read those rows honestly: they are not evidence for
the signature idea, they are evidence that what happens when it runs out is no longer a gamble.
The suite as a whole went from 13 solved in 709 s to 21 in 341 s, and every gain is in those two
families.

Note the `sample` column on the circuit and SHA-256 rows: those populations are executed rather
than propagated, so building them is no longer a visible share of a run - the seconds against
17 and 20 rounds are the probe loop failing to find agreement, not the sampler. The random
3-SAT rows are the ones that still pay for propagated populations, and there the number counts
every redraw a stalling run asked for. It grows with `n` because a formula with no driving input
set leaves the generator filling in every variable one at a time, propagating after each - which
is what makes the lanes it keeps genuine samples, and on these instances what makes it discover
that almost none of them are.

### Tests

The benchmark is the test suite for whether the solver answers correctly. Every solved instance
is re-checked clause by clause against the file it came from, independently of the solver's own
bookkeeping; an assignment that does not satisfy the formula is reported as `BAD` rather than
`SOLVED`, so a wrong answer fails the run rather than passing quietly.

That leaves one blind spot, and `selftest` fills it. The CDCL phase makes promises to the round
loop that a solved instance cannot show: that a literal on its level 0 trail really is implied
by the formula and the pinned assignment, that a root conflict means those roots really are
refuted, that the search is back at level 0 whatever the phase returned. Break one of those and
the solver quietly loses inference — or, in the other direction, hands the round loop a
decision dressed up as a proof — without any instance coming back wrong. So the mode generates
random formulas small enough that every model of one can be enumerated by brute force, and
checks each claim against that enumeration; the propagator's `undoTo` is checked the same way, since a rollback
that leaves the clause counters off shows up nowhere else either. It reports the seed of the
first failure, and a few thousand rounds take well under a second.

CI builds on Linux (GCC and Clang) and Windows (MSVC), regenerates the whole suite to check the
generator is deterministic, and runs a short benchmark pass. Locally:

```bash
ctest --test-dir build          # smoke tests plus the property tests
turbocryptosat selftest 50000   # the property tests on their own, longer
./benchmark.sh --timeout 30     # the full suite on a short budget
```

## Tuning

`--initk` and `--mink` together decide whether the statistical layer says anything at all, and
they are the two knobs worth sweeping before concluding an instance is out of reach. The
defaults, `6` and `10`, were picked on a 17-round SHA-256 preimage; here is the measurement, so
you can see how narrow the useful band is.

First a grid at `--attempts 1` (so `initk` means one thing — with restarts it halves down the
ladder), two seeds per cell, 45 s each, showing how far the run got:

| `initk` | `mink 8` | `mink 16` | `mink 32` | productive probes |
| --- | --- | --- | --- | --- |
| 4 | 33 %, 33 % | 33 %, SOLVED | 33 %, 33 % | 78–94 |
| 5 | 33 %, 33 % | 33 %, 33 % | 33 %, 33 % | 85–120 |
| **6** | **41 %, 38 %** | 34 %, 34 % | 33 %, 33 % | 129–273 |
| 7 | EXHAUSTED 3 s, 1 s | EXHAUSTED 18 s, 4 s | 33 %, 33 % | 105–327 |
| 8 | EXHAUSTED 0.4 s, 0.7 s | EXHAUSTED 9 s, 4 s | 35 %, 34 % | 100–289 |

Read it as a cliff, not a slope. Below `initk 6` the filter is too wide and every run parks
within fifty variables of the same wall at 33 %. At `initk 6` the productive probe count roughly
doubles and the run starts moving. At `initk 7` the verdicts are still plentiful but they are
*wrong*, and the assignment gets refuted by plain propagation in seconds. `mink 32` — the old
default — suppresses the layer at every `initk`.

**Read those historical `mink` numbers as occupied lane words.** Use `--mink-unit words`
to select that interpretation. The default remains `--mink-unit samples --mink 640` for
compatibility, but **ten occupied words are not equivalent to 640 surviving samples**.
Ten occupied words may contain anywhere from ten to 640 surviving samples. Multiplying the
prototype's parameter by 64 can therefore suppress useful statistical implications.

Both units are statistical controls, not correctness guarantees. For independent unbiased
samples a variable is accidentally constant with probability `2^-(n-1)`, but circuit variables
and rejection-sampled populations need not satisfy those assumptions. Parameter quality must
be judged by actual progress and verified solutions on the target instance family.

Fixing the unit in samples makes `mink` an *absolute* evidence threshold, which ties it to the
other two knobs: each of the `initk` filter literals roughly halves the population, so a probe
has about `siglen * 64 / 2^initk` survivors to show. Keep

```
siglen * 64  >=  mink * 2^initk
```

or the layer spends its time bailing and the run is mostly propagation plus CDCL. The defaults
sit just inside it — `65536` against `640 * 64 = 40960`. Measured on
`19-xorcircuit-i96-g2500`, dropping to `--siglen 256` without lowering `mink` cuts the verdicts
from 80 829 to 26 134 and raises the bails from 16 865 to 48 638: not off, but suppressed to
about a third.

Then the solve rate on 17-round SHA-256 (`--attempts 5`, 120 s, six seeds). **These numbers
predate the unit change**, so read every `mink` in them as lane words and select
`--mink-unit words` when reproducing them:

| setting | solved | when it did not |
| --- | --- | --- |
| `--initk 6 --mink 10 --mink-unit words` | **2 / 6** (71 s, 99 s) | 38–43 % |
| `--initk 6 --mink 8` words | 0 / 6 | 34–50 % |
| `--initk 8 --mink 32` words (the default before that) | 0 / 6 | 39–45 % |

Two solves out of six is not a significant result on its own, and it is worth being blunt about
that: the case for the winning pair is the grid above, plus the fact that the shipped benchmark
suite is indifferent to the change (21 of 26 either way). What the numbers do rule out is the
oldest default, which never once broke the wall.

Note also what was *not* the lever. Across those runs the CDCL phase proved anywhere between 0
and 267 literals with no relation to whether the run finished, while `resamples` sat at 22–56
every time — on this family the redraws and the probe loop do the work, and the plateau handler
is there to keep a stall from turning into a bad guess, not to solve the instance.

### Tuning it automatically

Doing that sweep by hand is what `tune` mode is for. Give it an instance and a solution and it
searches for the settings that suit the family:

```bash
turbocryptosat tune instance.cnf                      # uses instance.solution.cnf
turbocryptosat tune instance.cnf solution.cnf         # or an explicit one
turbocryptosat tune benchmark/ --preset thorough      # every instance that has a solution
```

The solution is what makes the search affordable. The solver assigns in place and never
backtracks, so a setting that commits a wrong literal does not announce itself — the run just
wanders off and burns the whole budget before reporting a timeout. With the solution to check
against, the first literal committed against it ends the trial instead, usually in well under a
second, so ruling a bad setting out is nearly free. It is only ever a check: nothing in the
search reads it to decide anything, which is what makes the resulting parameters mean something
on an instance nobody has solved yet.

Settings are ranked so that more variables correctly assigned wins, and among settings that
finish the instance outright, the faster one wins. Two refinements keep the search off the noise
floor. Time is not a tie-break between partial results — rewarding a trial that reached 40 % and
died after two seconds over one still going at the timeout would select for settings that fail
fast. And when neither setting finishes and their progress is within two points of each other,
the one that went wrong fewer times wins: a run that poisons the assignment can never solve the
instance however long it is given, while one that merely stalls still might.

The search is coordinate descent over `initk`, `mink`, `siglen`, `stall-limit`, `probe-vars` and
`sample-rounds`, in that order, one axis at a time against the best found so far. A full grid
would be thousands of trials; this is a few dozen, and once `initk` and `mink` are in their band
the axes barely interact.

| Option | Meaning | Default |
| --- | --- | --- |
| `--preset <name>` | `quick` (3 axes, one pass), `balanced`, `thorough` | `balanced` |
| `--tune-timeout <sec>` | Budget for one trial | `30` |
| `--tune-budget <sec>` | Budget for the whole search, a hard deadline: it also caps each remaining trial | unlimited |
| `--tune-seeds <n>` | Runs per setting, averaged — results are seed-noisy | `3` |

Output ends with a command line you can paste:

```
  best         --siglen 1024 --initk 6 --mink 32 --stall-limit 1000 --probe-vars 1 --sample-rounds 12
  result       SOLVED on every run (2/2), 0.56s average
```

**Set `--tune-timeout` above the time a successful run actually takes.** This is the one way to
get a meaningless answer out of the mode. If no trial ever finishes, every setting is ranked on
partial progress, and on a hard instance that spread is almost entirely seed noise — in one
measured sweep of a 17-round SHA-256 preimage at a 40 s trial budget, all twenty-five settings
landed between 31.0 % and 33.4 %, and the "winner" led by 0.3 points. The mode prints a warning
when nothing solved; take it seriously rather than pasting the command line it suggests.

**A run that assigns every variable counts as solved even if it is a different solution.** The
oracle exists to cut short a run that has wandered off, not to reject an answer, so a complete
satisfying assignment is never scored as wrong. What it does still reject is a *partial* run that
has diverged, and on an instance with several solutions — circuit inversion where the output
layer has more than one preimage — some of those divergences were heading somewhere perfectly
valid. Watch the `wrong` column: if it stays high across every setting while runs do finish, the
ranking is measuring agreement with one particular solution more than it is measuring difficulty.

The summary after every run is meant to be read as a diagnosis.

| Symptom | What it means | Knob |
| --- | --- | --- |
| `samples` shows far fewer valid lanes than total | Random values for the input set conflict on most lanes | `--inputs`, or raise `--sample-rounds` |
| `input variables` is a large fraction of all variables | Auto-detection found no small driving set | `--inputs` |
| Many probes, few productive, high `probes short of samples` | The filter is too narrow and keeps bailing out | Lower `--initk` or raise `--siglen` |
| Many probes, few productive, low `probes short of samples` | The filter is too wide: thousands of samples survive and nothing looks constant | Raise `--initk` |
| Restarts early and often | Statistical verdicts are firing on biased variables | Raise `--siglen`, or start from a lower `--initk` |
| Progress stalls, `cdcl` phases climbing | Nothing left for the probes to find; the run is riding on the CDCL phase | Raise `--probe-vars` to 2, or lower `--stall-limit` |
| `cdcl` shows many conflicts and no literals proved | The formula is out of reach of unit learning under this assignment | Raise `--cdcl-conflicts`, or lower `--stall-limit` so phases start sooner |
| Sampling takes most of the run, phase stuck on `focusing` | The target bits cost `2^n` redraws each and the budget cannot pay for them | Lower `--focus`, or set it to `0` |
| `focus` reports far fewer lanes than `--siglen` allows | The rejection loop ran out of time and the lanes that never landed were struck out | Lower `--focus`, or raise `--timeout` |
| Out of memory | The table is `numVars * sigLen * 8` bytes, twice that while a propagated population is being built | Lower `--siglen` |
| `circuit` reports unexplained clauses on an instance that is a circuit | Some clauses fall outside the three gate patterns, so the population is propagated instead of executed | Nothing to turn; sampling is slower but the result is the same |

## Limitations

* **Satisfiable instances only.** The solver assigns in place and cannot prove unsatisfiability.
  A run that exhausts its attempts reports `EXHAUSTED`, which says nothing about satisfiability.
* **Statistical, not complete.** A committed batch of literals can be wrong. Restarts are the
  recovery mechanism, and they are not guaranteed to succeed.
* **Needs a driving input set.** On instances with no such structure (random k-SAT) the sample
  population collapses, the probes find nothing, and the run is carried entirely by the CDCL
  phase — correct, but no better than the CDCL phase alone.
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
