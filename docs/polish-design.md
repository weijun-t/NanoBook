# Phase 7 design: polish, so a stranger can check every claim

> **Status:** approved (2026-10-25): every decision in §8 as proposed, plus a final project
> report (§5.1, step 8b), the user's addition. Changes from here on are made by an edit plus a
> note in the commit message. Step 2: `reproduce.sh` (quick level 60 s on WSL2, full level 393 s)
> and `fetch_itch.sh`. Step 3: the flame graph script, revised (§6.1), and `plot_compare.py`.
> Step 4: libFuzzer target with a structured mode (§2), a planted bug caught in 0.4 s. Step 5:
> interview notes. Step 5b: the release session as one script (§3.4). Step 6 found the synthetic
> `match` 8–17% slower since E9b; checked on WSL2 and published as measured, the user's decision
> (perf log 2026-10-25): the README says so in one line. Step 7: the README front page, from the
> release session's numbers and charts (`docs/img/release_*`); the plan moved to `docs/plan.md`,
> and "README rule N" / "README §N" became "plan rule N" / "plan §N" everywhere. Step 8: every
> Markdown link, image and backticked repo path checked (four stale paths fixed); the design
> notes' status headers say each phase is complete; the interview notes use the release numbers.

Phases 0–6 built, tested, measured and tuned NanoBook. The work is real, but a stranger landing on
the repo sees a **plan** (the README is still the project's rule book) and a pile of reports.
Phase 7 turns it into something a reviewer can judge in minutes. The README's exit criterion:
**a stranger can build, test and reproduce the numbers in under 10 minutes.**

---

## 0. Background in plain words

### 0.1 Who the reader is
An interviewer or a quant dev looking at the repo for 5–10 minutes. They want to know, in order:
1. What is it? (one paragraph, one diagram)
2. Is it fast, and how do you know? (one table, two charts, the machine they came from)
3. Is it correct? (how it's tested, in three lines)
4. Can I run it myself? (copy-paste commands that work)
5. What did you learn, and what would you do differently? (the trade-offs)

Everything else (the rules, the phase reports, the perf log) is depth for the curious, linked
from the front page.

### 0.2 What "reproduce" can honestly mean
Measured 2026-10-25: a **fresh clone, configure, build and test** (`cmake --workflow --preset
gcc-release`) takes **47 s** on this laptop (24 threads), including downloading GoogleTest and
Google Benchmark. So building and testing fit easily.

The real Nasdaq day doesn't: it's a **4.76 GB download** that unpacks to 11.2 GB. That depends on
the reader's internet, not on us. So "reproduce" has two levels:

| Level | What the stranger gets | Needs | Time |
|---|---|---|---|
| **Quick** | Build, every test, a short fuzz run, the per-operation latency table (phase 3) | A compiler | Under 10 minutes |
| **Full** | The real day's throughput and latency (phase 4), the pipeline (phase 5) | The day file | Download time, then ~1 minute per run |

The README will say this plainly, not hide the download inside "10 minutes".

### 0.3 A problem found while planning: the published numbers are one commit behind
The bare-metal session ran phases 3, 4 and 5 at commit `943fe70`. **After** those runs, E9b
(8-byte ID entries, 7% faster on the day) was added back at `1754cdf`. So today's code is faster
than the published phase 3–5 numbers, and a results table built from them would describe code
that's no longer there. Rule 19 says every number states its commit, so mixing them is allowed
but confusing ("75 ns per message" in one place, "68 ns" in another). And the phase 6 A/B files
carry the `NOT FOR PUBLISHING` banner, because "after" is always an uncommitted working tree (by
design, optimise design §3.1).

The clean fix is **one last bare-metal session at the release commit** (§3, Q3).

---

## 1. Goal and exit criteria

Phase 7 is done when:
1. The README opens with what NanoBook is, headline results (bare metal, one commit, environment
   stated), how to run it, and the trade-offs; the old plan text moves to `docs/plan.md`.
2. `scripts/reproduce.sh` takes a fresh clone to the quick results in under 10 minutes, and has
   been **timed from a fresh clone** on a clean environment (the USB stick).
3. A flame graph of the real day is in the README.
4. `docs/interview-notes.md` covers each major trade-off: what was chosen, what else was tried or
   considered, why, and the evidence.
5. Every link in the README and `docs/` resolves; each design note's status header is current.
6. `docs/final-report.md` explains every test in plain words and ends with a conclusion (§5.1).
7. CI is green, with every preset, formatting and clang-tidy.

---

## 2. What's missing today (plan §6 and §7 against the repo)

| Promised in the README | State | Proposal |
|---|---|---|
| README results with latency and depth plots | Charts exist in `docs/img/`, not on the front page | New front page (§4) |
| Interview-notes section | First draft in phase 6 report §8 | `docs/interview-notes.md` (§5) |
| Flame graphs (§7.3) | Never made (phase 6 profiled with `perf` + `addr2line` into a table) | `scripts/flamegraph.sh` (§6) |
| `plot_compare.py`, before/after curves (§7.5) | Never built; the perf log used tables | Build it for one chart: phase 5's end against the release (§6) |
| libFuzzer on the ITCH parser (§7.1, optional) | Not done | Q6. Built (step 4) with two modes: raw bytes, and a list of operations the test encoder writes as valid ITCH (raw alone reached 179 branches in 60 s and never a book; with both, 915) |
| A stranger reproduces in < 10 min | No single command; steps scattered over reports | `scripts/reproduce.sh` (§3) |
| Live view (§1, optional, after phase 7) | Not done | Out of scope (§9) |

---

## 3. Reproducing

### 3.1 `scripts/reproduce.sh`
One script, run from a fresh clone:
```
scripts/reproduce.sh            # quick: build, test, fuzz, latency table
scripts/reproduce.sh --itch F   # also the real day (throughput + latency) and the pipeline
```
What it does, in order: checks the compiler versions (GCC ≥ 14, CMake, Ninja) and says what to
install if one is missing; `cmake --workflow --preset gcc-release`; a fuzz run (10⁶ ops, 3
seeds); `nanobook_bench` on one pinned core; prints the result next to the published numbers,
with a reminder that their machine differs and WSL2 or a laptop on battery will be slower. It
writes everything to `repro/` (git-ignored). No `sudo`, nothing installed by the script.

### 3.2 `scripts/fetch_itch.sh`
Downloads `01302019.NASDAQ_ITCH50.gz` from Nasdaq's public site, checks the size (4,764,426,091
bytes), and unpacks it to `data/itch/`. It exists so the full level is one command too.

### 3.3 The timed check
The exit criterion is checked, not assumed: on the USB stick (a clean Ubuntu, not this WSL2
setup), clone from GitHub into an empty folder, follow the README's quickstart literally, and time
it. Ideally a friend does it once on their machine too (Q8).

### 3.4 The last bare-metal session (Q3)
One boot of the stick, at the release commit (code frozen; only docs change after it):
1. The timed fresh-clone check (§3.3).
2. `scripts/run_latency_session.sh` (phase 3), the day replay twice (phase 4),
   `scripts/run_pipeline_session.sh` (phase 5): the same commands as the bare-metal report, so
   every README number comes from **one commit**.
3. The flame graph (§6.1).
4. One A/B: phase 5's last commit against the release, on the real day, 2 rounds. It answers
   "what did phase 6 buy, in total?" with committed builds on both sides, so no
   `NOT FOR PUBLISHING` banner (§6.2).

About an hour of runs. Same boot options and preparation as the bare-metal report §2. Built as
one command (step 5b): `scripts/run_release_session.sh ITCH_FILE OUT_DIR` runs all four parts
with the existing scripts, unchanged; the stranger test clones this repository at this commit
into an empty folder (so it doesn't depend on GitHub access) and builds from nothing.

---

## 4. The README

### 4.1 Shape
```
NanoBook                                   (badge, one-paragraph pitch)
  Results           table + 2 charts, machine + commit under them
  Quickstart        the 3 commands, and the "full" level
  How it works      the pipeline diagram, the book's data structures, 5 lines each
  How it's tested   reference book, differential fuzz, real day, sanitizers, 0 allocations
  Trade-offs        6 bullets, link to docs/interview-notes.md
  Read more         every doc, in reading order, one line each
  Status            phases 0–7, one line each
```
About 150 lines, against 223 today.

### 4.2 The headline table (from §3.4, one commit, bare metal, isolated core)
Rows: per-operation p50 / p99 / p99.99 for add, cancel, execute, match (phase 3); the real day's
ns per message, p50, p99, p99.99 (phase 4); the pipeline's p99 at 1, 2 and 4 M messages a second
(phase 5). Below it: CPU, cores, kernel, compiler, flags, isolation, dataset, commit (rule 19).

### 4.3 Charts
The phase 3 percentile curves, the phase 5 latency against rate, the flame graph, and the AAPL
depth chart (real data looks right). Regenerated by the existing `scripts/plot_*.py` from the
§3.4 data.

### 4.4 The plan moves out
Sections 1–9 of today's README (scope, language standard, **rules**, fact checks, architecture
sketch, roadmap, tooling, learning path, references) move to `docs/plan.md` unchanged except
links. `CLAUDE.md` says "the Rules in `README.md` §3", so it changes to `docs/plan.md` §3. The
front page keeps a one-line link: "how this project was run: the rules and roadmap".

---

## 5. Interview notes (`docs/interview-notes.md`)
One entry per decision, each with the same four parts: **chose**, **instead of**, **because**
(with the number and a link to where it was measured), **would revisit if**. Planned entries:

1. C++23, not 20 or 26
2. Sorted array of levels (best at the end) against a tick-indexed array (phase 2, and why the
   tick array can't hold the real day: phase 4)
3. Direct-indexed ID table against a hash map; then 8-byte entries (E9b)
4. Intrusive FIFO lists in a pool, `uint32_t` indices instead of pointers
5. `-fno-exceptions` and `std::expected`
6. The SPSC ring: acquire/release, why not `seq_cst`; 128-byte padding against 64 (E5)
7. Three threads slower than one: why, and when a pipeline is still right
8. The huge-page arena, and keeping the touching loop against the rule (B5-E2b)
9. What didn't work: `-march=native`, PGO, prefetching, the moving window, batched ring 2
10. Measurement: own `rdtsc` harness, coordinated omission, why WSL2 numbers aren't published

Then a short list of **likely questions** with two-line answers (e.g. "why is your p99.99 ten
times your p50?", "how do you know the fast book is correct?"). Phase 6 report §8 is reused.

### 5.1 The final report (`docs/final-report.md` + PDF), added at review
The interview notes are short answers to questions. The final report is the whole story, for a
reader with no background, in plain words with examples. For **every test and experiment** in
phases 1–7 (correctness tests, benchmarks, the real day, the pipeline, every phase 6 experiment,
the bare-metal checks, phase 7's own checks), it gives the same four parts:
1. **The test:** what we ran, in plain words.
2. **Why we expected it to win:** the reasoning before the data.
3. **What it told us:** the numbers, and whether the guess was right.
4. **So what:** what it means for the engine, and for someone building one.

It ends with a **conclusion**: what NanoBook is, how fast and how correct it is, what was
learned, and what would come next. Written after the last bare-metal session, so its numbers
are the release commit's. The phase reports stay as the detailed record; this one links to them.

---

## 6. Charts and profiles

### 6.1 Flame graph (`scripts/flamegraph.sh`)
Revised while building it (step 3), because two plans failed on contact:
- **Not the `gcc-profile` preset.** It is `-O2`, which phase 6 measured 2.4× slower: a
  different program. The graph profiles `gcc-release`'s program plus `-g`, and the script checks
  that the instructions are the same (disassemblies compared with addresses masked).
- **Not plain frame pointers.** GCC 14 gives functions that call nothing no frame, even with
  `-mno-omit-leaf-frame-pointer` (checked on a 3-line example; Clang does give one). The hottest
  function, `IdTable::find`, is such a leaf, so every sample in it lost its caller.

So the script picks the first stack method this machine's perf can do: **LBR** (the CPU's own
record of calls and returns, Intel, bare metal), then **DWARF** unwinding (needs a perf built with
libdw, as Ubuntu's is), then frame pointers (WSL2: enough to test the script). WSL2's perf has no
libelf or libdw, so it can't name functions at all; `scripts/fold_stacks.py` does that with
`addr2line`, and expands each address into its inlined functions (in a `-O3` build most of the
call stack is inlined). Drawn with Brendan Gregg's FlameGraph, cloned at a pinned commit into the
build folder (not copied into the repo). SVG in `docs/img/`; the published one comes from §3.4.

### 6.2 `scripts/plot_compare.py`
Reads two CSVs from the same tool and draws their percentile curves on one chart (plan §7.5).
Used once, for "phase 5's end against the release" from §3.4.

---

## 7. Plan, one commit per step

| Step | What | Needs |
|---|---|---|
| 1 | This design note, approved | — |
| 2 | `scripts/reproduce.sh`, `scripts/fetch_itch.sh`, `repro/` git-ignored; tried on WSL2 | — |
| 3 | `scripts/flamegraph.sh` (tried on WSL2), `scripts/plot_compare.py` (tried on old A/B data) | — |
| 4 | Q6 if yes: libFuzzer target for the parser, a Clang preset, a 60 s CI run | — |
| 5 | `docs/interview-notes.md` | — |
| 5b | `scripts/run_release_session.sh`: step 6 as one command | — |
| 6 | **Code freeze**, then the last bare-metal session (§3.4); data in `docs/data/bare_metal/release/` | The stick |
| 7 | README front page with the §3.4 numbers and charts; plan to `docs/plan.md`; `CLAUDE.md` pointer | Step 6 |
| 8 | Tidy: link check over README and `docs/`, design-note status headers, stale statements | — |
| 8b | `docs/final-report.md` + PDF (§5.1): every test, why, result, so what; conclusion | Step 6 |
| 9 | README status: phase 7 complete | Steps 1–8b |

---

## 8. Decisions (approved 2026-10-25)

| # | Question | Proposal | Why |
|---|---|---|---|
| Q1 | README shape | Front page (§4.1); the plan moves to `docs/plan.md` | A reviewer reads the top 50 lines; today those are rules, not results |
| Q2 | What "reproduce in 10 minutes" covers | Quick level (§0.2); the full level stated with its download | Honest: the download isn't ours to speed up |
| Q3 | Last bare-metal session at the release commit | **Yes** (§3.4), about an hour on the stick | One commit for every README number; also the clean-room timing and the flame graph |
| Q4 | Flame graph source | Bare metal (cycles), script tested on WSL2 first | Same standard as every published number |
| Q5 | `plot_compare.py` | Build it, for one chart (phase 6's total) | Promised in plan §7.5; cheap; the chart is a strong talking point |
| Q6 | libFuzzer on the ITCH parser | **Yes**, small: one target, a `clang-fuzz` preset, 60 s in CI | A feed handler must survive malformed bytes; about 40 lines |
| Q7 | Interview notes | Separate `docs/interview-notes.md`, 6 bullets on the front page | Too long for the README; the bullets make the link worth clicking |
| Q8 | A real stranger | Ask one friend to follow the quickstart once, after step 7 | The only true test of the exit criterion; optional |

---

## 9. Out of scope for phase 7
- **New optimisations.** Phase 6 is closed; the code freezes at step 6.
- **The live view** (plan §1): optional, after phase 7, a separate process.
- **C++26 reflection** for tooling (plan §2): a separate, deliberate step, if ever.
- **Docker image.** Would make "a stranger" easier, but adds a toolchain to maintain; the CI
  workflow already proves a clean Ubuntu 24.04 can build from scratch.
