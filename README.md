# PA8 Predictive Pager

`pager-predict.c` implements `pageit()` for the paging simulator. It watches each process's program counter, works out which program the process is running, and pages in what that program is about to execute before it needs it.

**Result:** across 150 random seeds the blocked/compute ratio averages about **0.0031** (worst seed about 0.0047). Everything below 0.005 earns full marks.

---

## 1. The problem

The simulator runs up to 20 processes at once. Each process has 20 virtual pages of 128 instructions each, but there are only **100 physical frames** shared by everyone. One instruction takes one tick. Every tick, `pageit()` is called with each process's PC and which of its pages are resident.

The rules that shape the design:

| Rule | Consequence |
|---|---|
| `pagein` takes 100 ticks to complete | A page must be requested at least ~100 ticks before it is executed. |
| `pageout` takes 100 ticks to free its frame | Frames are not available immediately after evicting. |
| A page cannot be paged in until its pageout has *finished* | Evicting then re-fetching a page costs about 200 ticks. |
| A page being paged in or out shows as "not resident" in the `pages[]` array | The pager cannot tell "coming in" from "gone", so it must not rely on that array for in-flight state. |
| `pagein`/`pageout` return 1 if the operation is already in progress | It is safe to call `pagein` for the same page every tick. There is no bandwidth limit on concurrent operations. |
| A blocked tick is counted whenever a process's current page is not resident | Score = blocked ticks / compute ticks. |

Because 20 processes run in parallel and each needs only a couple of pages at a time, 100 frames are enough **if** the right pages are loaded just in time and the wrong ones are evicted. The pager is therefore about timing, not about squeezing memory.

---

## 2. What the programs look like

The simulator draws its processes from 10 program types. Their control flow is fixed (loop counts and IF outcomes are random, but where branches are and where they go is not). The pager hardcodes this skeleton in the `progs[]` table:

| Program | Size (pages) | Branches (from → to) | Behaviour |
|---|---|---|---|
| 0 | 1435 (12) | 500→1402 IF, 1301→1433 GOTO, 1433→0 FOR | Optionally skips from page 3 to page 10, then loops 10–30 times |
| 1 | 2535 (20) | 1500→2402 IF, 2401→2533 GOTO, 2533→0 FOR | Same shape as 0, larger, 15–20 loops |
| 2 | 1431 (12) | 1429→0 FOR | Straight line, loops 20–50 times |
| 3 | 2131 (17) | 2129→0 FOR | Straight line, loops 20–30 times |
| 4 | 1285 (11) | 1282→1066 FOR, 1283→0 FOR | Inner loop over pages 8–10, then outer loop |
| 5 | 2485 (20) | 2482→2166 FOR, 2483→0 FOR | Inner loop over pages 16–19, then outer loop |
| 6 | 912 (8) | none | Runs straight through once |
| 7 | 2500 (20) | none | Runs straight through once |
| 8 | 705 (6) | 700→703 IF, 701→0 GOTO, 702→703 GOTO, 703→0 FOR | Short loop with a small branch near the end |
| 9 | 1505 (12) | 1500→1503 IF, 1501→0 GOTO, 1502→1503 GOTO, 1503→0 FOR | Same shape as 8, larger |

Branch types: **GOTO** is always taken. **FOR** is taken a random number of times and then falls through to the exit. **IF** is taken or not at random. The pager treats FOR and IF as "either outcome is possible", since it cannot know the random choice, and treats GOTO as "always taken".

Each program has exactly one exit PC, reached by falling through after the last loop.

---

## 3. How the pager works

Every call to `pageit()` does the same four steps for each active process.

### Step 1: Identify the program (`update_mask`)

Each process has a bitmask of *candidate programs*, initially all 10. Every tick the pager compares the process's PC with its PC on the previous tick:

* **PC unchanged**: the process was blocked. No information.
* **PC advanced by 1**: a sequential step. This rules out any program that has a GOTO at the previous PC pointing somewhere other than the next instruction (it would have jumped), and any program whose exit PC is the previous PC (it would have exited).
* **Any other change (a jump)**: keep only programs that have a branch at the previous PC whose target is the new PC.

If this ever eliminates every candidate, the pager resets to all 10 programs rather than trusting a bad trace. This also handles a **new process** being loaded into a slot: a new process starts at PC 0 coming from an exit PC, which no program explains as a branch, so the mask resets. A slot that was inactive on the previous tick resets as well.

In practice the candidate set narrows quickly. Only the very start of a process, before it reaches distinguishing branch addresses, is ambiguous, and during that stretch all candidates agree on what to fetch next (the following page).

### Step 2: Know what could execute soon (`dist` table)

At startup, `build_tables()` runs a breadth-first search from every PC of every program and records `dist[program][pc][page]`: **the fewest ticks until that page could be executed**, considering every branch outcome. GOTO has one successor; FOR and IF have two. The search stops at the exit PC (the process leaves) and is capped at `HORIZON` ticks.

This turns prediction into a table lookup at runtime. For each process the pager takes, for each page, the minimum `dist` over all remaining candidate programs. It always treats the current page as distance 0 and the next sequential page as distance "ticks to the page boundary", which keeps behaviour sensible even if the candidate set is wrong.

### Step 3: Page in what is needed soon

Any non-resident page with distance ≤ `LOOKAHEAD` (104 ticks) is queued for page-in. All requests across all processes are sorted by distance, so the most urgent pages are requested first, and `pagein()` is called for each. If frames run out, the least urgent requests are the ones that fail, and they are retried the next tick.

`LOOKAHEAD` is just above `PAGEWAIT` (100). Going lower caused misses (0.009 at 99). The few extra ticks are slack against off-by-one timing in the simulator. Because both outcomes of an IF are prefetched, some pages are loaded that turn out not to be used. Frames are plentiful enough that this costs nothing measurable.

### Step 4: Evict only what will not be needed soon

A resident page is paged out only if its distance exceeds `KEEP` (215 ticks). This is the most important tuning choice in the design.

Evicting a page and then needing it again is expensive: the pageout must finish (100 ticks) before the page-in can even start (another 100 ticks). A plain "evict as soon as the PC leaves the page" policy produced a ratio of about **0.05**, because programs 4 and 5 loop over a few pages and revisit them within about 200 ticks. Holding a page whenever it will be needed again within `KEEP` ticks removes those misses and brought the score to about **0.003**.

Pages for the long straight-line loops (programs 2, 3, 0, 1) are not held, because the loop is much longer than 215 ticks, so each process only holds the page it is on plus the next one or two. This keeps total frame use well under 100.

---

## 4. Why the score is not zero

Nearly all remaining blocked time is the first page of each process. The simulator loads a process and steps it in the same tick, and `pagein` is only allowed on an active process, so every process spends about 100 ticks blocked waiting for page 0. With 40 processes that is about 4000 blocked ticks, against about 2,000,000 compute ticks, which gives roughly 0.002. This cannot be avoided by any pager, so the achieved 0.0031 is close to the floor.

---

## 5. Parameters

| Constant | Value | Meaning |
|---|---|---|
| `LOOKAHEAD` | 104 | Request a page this many ticks before it could be executed. Must exceed 100. |
| `KEEP` | 215 | Do not evict a page needed again within this many ticks (about 2 × `PAGEWAIT` plus slack). |
| `HORIZON` | `KEEP + 1` | How far ahead the BFS looks when building `dist`. |

Both can be overridden at compile time for experiments, for example `gcc -DLOOKAHEAD=108 -DKEEP=230 ...`.

---

## 6. Code map

| Item | Role |
|---|---|
| `progs[]` | Hardcoded control-flow skeleton of the 10 programs. |
| `brat[k][pc]` | Which branch (if any) sits at a given PC for program `k`. |
| `dist[k][pc][page]` | Minimum ticks until `page` could execute, starting from `pc`. |
| `build_tables()` | Fills `brat` and `dist` on the first call. |
| `update_mask()` | Narrows the candidate programs from an observed PC transition. |
| `pageit()` | Per tick: classify each process, compute needed pages, evict, then page in by urgency. |
| `Req` / `req_cmp` | A page-in request and its ordering by urgency. |

---

## 7. Building and testing

```
make                          # builds test-predict
./test-predict -seed 5        # run one simulation; prints the blocked/compute ratio
./test-predict -seed 5 -csv   # also writes output.csv and pages.csv for see.R
make submit                   # packages pager-predict.c as PA8-<username>.txt
```

The reported ratio is the line `ratio blocked/compute=...`. The grader averages several seeds, so check more than one.

---

## 8. Limitations

* **The program table is hardcoded** to match the supplied `programs.o`. If the grader's version has different sizes or branch addresses, the table is wrong. The pager still works, because it falls back to all candidates plus sequential prefetch and always pages in the current page, but it would score worse. It does not link against the simulator's `programs` array because the assignment marks `programs.o` as for simulator use only.
* **Prediction is conservative, not exact.** IF and FOR outcomes are random, so both paths are prefetched. This is deliberate: a wasted page-in costs a frame for a short time, while a missed one costs 100 blocked ticks.
* **Tuned for the default constants** (20 processes, 100 frames, 100-tick `PAGEWAIT`). Other values would need `LOOKAHEAD` and `KEEP` retuned.
