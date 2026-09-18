# PondMerge benchmark results

All figures on this page were produced by the programs in `bench/`, built from
the committed source, on an idle machine, and each run prints the identity and
the measured cost of its own timing instrument. Nothing here is transcribed from
a design document.

**Host:** AMD Ryzen 5 5600X, 3693 MHz, Ubuntu, `g++` 14, `-O2 -DNDEBUG
-DPM_DEBUG=0`. The host is a VMware guest; see the instrument section — that
matters more than the CPU does.

**Device:** ESP32-S3 (n16r8), 240 MHz, `-O2`, `PM_DEBUG=0` (Release semantics),
192 KiB region as 48 x 4 KiB segments, `PM_MAX_OBJECTS=256`, `PM_MAX_POOLS=16`,
`PM_SL_COUNT=4`. Firmware is `bench/esp32/`, a separate IDF project that compiles
the **same** benchmark sources as the host build; only the sizing macros differ.
Section 5 is the device run. The instrument is a different one there, and it says
so itself: `esp_cpu_get_cycle_count()` costs 59 ns per read against the host's
9,592 ns, so per-operation timings are usable on the target and are not on the
host.

**Superseded:** an earlier revision of this file reported numbers produced before
the instrument problem below was found. Those figures were not wrong in
direction but their error budget was unknown and in some cases the instrument
was the dominant term. They have been re-measured and are replaced here. In
particular the compaction time, the `PM_SL_COUNT` comparison and the
`free`/`alloc` split all changed materially.

---

## 0. The instrument: how the original numbers were wrong, and why

`std::chrono::steady_clock::now()` on this host costs **~9,400 ns per call**.
The operations under test cost 36–90 ns. Every per-operation timing taken this
way was measuring the clock.

The cause is that the guest's `RDTSC` is trapped: the timestamp *value* is
correct — a 200 ms wait measures 200.024 ms — but reading it costs a VM exit.
Accuracy of the value says nothing about the cost of reading it, which is
exactly why the defect was invisible for as long as it was.

Measured with a loop that calls each variant 200,000 times inside one long
interval, so the measurement of the instrument cannot itself be contaminated:

| what | cost |
|---|---|
| empty loop body | 0.01 ns/iteration |
| `g_sink += 1` | 0.44 ns/iteration |
| `g_sink += fn_const()` | 0.28 ns/iteration |
| 64-bit LCG, dependent chain | 1.01 ns/iteration (≈3 cycles, i.e. full clock speed) |
| `__rdtsc()` | **10,244 ns/iteration** |
| `lfence; __rdtsc()` | 9,968 ns/iteration |
| `__rdtscp()` | 9,936 ns/iteration |
| `clock_gettime(CLOCK_MONOTONIC)` | 9,341 ns/call |
| raw `syscall(clock_gettime)` | 19,530 ns/call — so the vDSO *is* in use; the vDSO itself is slow here |
| `clock()` (`CLOCK_PROCESS_CPUTIME_ID`) | **20,900 ns/call** |

The empty loop and the LCG prove the machine is otherwise healthy: the CPU is
not throttled and the cost really is inside the counter instruction. A
consequence worth stating plainly: **on this host no cycle-counter instruction is
usable**, because `RDTSC` itself is the trap. Substituting `rdtscp`, or adding
`lfence`, or reading the TSC directly all cost the same ~10 µs.

### What is done instead

`bench/bench_timer.h` provides two things and forbids the third:

1. **Batched timing.** An interval contains N operations between two clock
   reads, so the instrument contributes `2 * call_cost / N` per operation
   instead of `2 * call_cost`. At N = 16,384 that is ~1 ns/op instead of
   ~18,800 ns/op. This is machine-independent, which is the point — the same
   source gives valid numbers here, on a normal host, and on the device.
2. **A self-report.** Every benchmark prints its backend, the measured clock
   cost, and a verdict. A run that cannot support per-operation numbers says so.
3. **No per-operation timings**, except where the operation is long enough to
   survive the instrument, and then only with the bias stated.

An interval carries one bias, not two, and the bias is bigger in context than in
a tight loop. Calibrated with a real workload (a `memmove` of the same 184,296 B
that compaction moves):

| workload | batched (true) | per-event | in-context bias |
|---|---|---|---|
| memmove 184,296 B | 2,780 ns (66 GB/s) | 14,749 ns | ~11,969 ns |
| memmove 128 B | 9.8 ns | 8,745 ns | ~8,735 ns — **890x the work** |
| the tight-loop probe in `bench_timer.h` | — | — | 7,521 ns (an *under*estimate) |

So the corrected per-event figures below use the tight-loop bias and are
therefore still conservative, i.e. still slightly too large.

### Two consequences for the library

**(a) The host port was instrumenting itself.** `pm_port_ticks_us()` — used to
fill in `compact_time_us`, the library's only performance statistic — was
implemented with `clock()`, i.e. `CLOCK_PROCESS_CPUTIME_ID`: a syscall costing
20.9 µs here, called twice per compaction. That added ~42 µs of *self-measurement*
to every compaction, about 15x the cost of the memory it moved. It also made
`compact_time_us` a CPU-time figure rather than an elapsed-time one, so a
maintenance window that was preempted was under-reported. The host branch now
uses `CLOCK_MONOTONIC`. Effect on the measurement below: compaction went from
80 µs to 61 µs of raw wall time purely from removing the library's own clock
reads. The ESP32 branch was already correct (`esp_timer_get_time()`).

**(b) `get_stats` is not worth optimising, and this is now well founded.**
See section 3.

---

## 1. `alloc` / `free`: the address-order walk is gone

**Question.** `alloc` was documented as `O(bin chain + live objects)`. Which term
is real?

**Method.** Churn at a *constant* live count: free an object and reallocate the
same size at a random position — random deliberately, because an insertion walk's
cost depends on where the new element lands and a fixed position would understate
it. The interval holds 16,384 pairs, so it lasts ~1.3 ms and the instrument bias
is 0.7% of it. Eight trials, minimum kept; the median is printed beside it as a
read-out of the run's noise. The same benchmark source was compiled against two
core revisions, so the only difference between the two halves of the table is the
core.

| live | before Plan A | after Plan A | speed-up |
|---|---|---|---|
| 16 | 58.0 ns | 38.0 ns | 1.5x |
| 64 | 83.5 ns | 35.8 ns | 2.3x |
| 256 | 239.2 ns | 36.2 ns | 6.6x |
| 1024 | **844.2 ns** | **38.4 ns** | **22.0x** |

- Before: cost grows with the live count, x14.6 over a 64x range of live counts.
- After: **flat, x1.01**. Three consecutive runs of the current build gave ratios
  of 1.044, 0.970 and 1.010, so the flatness is within a ±4% run-to-run spread.
- The speed-up is proportional to the live count, which is the signature of a
  removed O(n) term rather than of a constant-factor improvement.

`free` is unchanged by Plan A and the pair figure tracks it: the pair cost *is*
the cost of one free plus one alloc at the same live count.

### Absolute figures for `free` and `alloc` separately

Attribution needs the differential of two long intervals (see
`measure_split()` in `bench/alloc_latency.cpp`). That differential is always L
operations while the interval is 2RL operations, so raising R lengthens the
interval but does not enlarge the signal — the absolute jitter of the clock is
what limits it. At the realistic 256 KiB / 1024-object configuration the
differential sits below that floor and the benchmark prints `n/a` rather than a
number, which is the honest output.

Attribution becomes possible at large L. In an 8 MiB / 16,384-object build:

| live | pair | free | alloc | cross-check |
|---|---|---|---|---|
| 8192 | 68.4 ns | — | — | **REJECTED by the cross-check** (gap 38.8 ns) |
| 16384 | 73.4 ns | 32.7 ns | 42.4 ns | accepted (gap 1.7 ns, 2.4%) |

The benchmark validates itself: `pair` is measured directly by churn while
`free`/`alloc` come from the interval differentials, so they are independent
measurements of the same quantity and a disagreement marks the row unreliable.
That is what happened at live = 8192, and reporting the rejected row is part of
the output rather than something hidden.

**Caveat on those two numbers:** at 16,384 objects the descriptor table alone is
1 MB, so the working set exceeds L2 and the figures include cache-miss traffic
that the 256 KiB configuration does not have. Read them as the *split* (alloc
costs about 30% more than free), not as the per-operation cost at 1024 objects.
The per-operation cost is what the pair column gives.

---

## 2. `validate` is quadratic; that is measured, not restated

`validate()` is documented `O((live + free)^2)`. It is read-only and idempotent,
so it can simply be called K times per interval with no state to restore; K is
chosen from a single probe call so each row costs about the same wall time.

| live | blocks | validate | per block | `get_stats` |
|---|---|---|---|---|
| 64 | 96 | 11.0 µs | 115.0 ns | 105.3 ns |
| 128 | 192 | 34.4 µs | 179.2 ns | 231.3 ns |
| 256 | 384 | 117.4 µs | 305.8 ns | 473.8 ns |
| 512 | 768 | 442.4 µs | 576.0 ns | 1,019.6 ns |
| 1024 | 1536 | **1,744.6 µs** | 1,135.8 ns | 2,682.5 ns |

Fitted exponent over a 16x range of block counts: **1.83** (a second run gave
1.79). Quadratic confirmed. This is a diagnostic path: it does not gate
throughput and is not in the maintenance transaction. Whether it is worth
linearising depends entirely on whether it is called from a health monitor, and
that is a decision for whoever integrates the library, not a default.

## 3. `get_stats` — a negative result, on purpose

Exponent 1.17, i.e. linear in the number of free blocks, at 1.75 ns per block.
At the largest size measured it costs **2.7 µs**. Polled at 10 Hz that is
0.003% of a core. There is no version of this that is worth changing an audited
function for, and the value of the measurement is that it prevents the change
rather than motivating one.

The same goes for `PM_SL_COUNT`. Three builds, same workload, live = 1024:

| `PM_SL_COUNT` | pair |
|---|---|
| 4 (default) | 40.7 ns |
| 8 | 37.8 ns |
| 16 | 40.8 ns |

No benefit, while the metadata cost grows (107 → 112 → 122 KB at 1024 objects).
**The default of 4 is correct; this should not be revisited.** Note that an
earlier run reported 48.0 ns for SL8 — a single noisy run. It is recorded here
because a reader comparing the two revisions of this file will see the number
change, and the reason is instrument noise, not a code change.

---

## 4. Fragmentation: the core claim of the library, A/B on one allocator

**Design.** Both variants use the same allocator and differ in exactly one
thing: whether compaction is triggered. Fairness rules are enforced by the
harness and stated in `bench/README.md` — compaction is never triggered from the
churn path (or the two variants would stop sharing a workload), both draw the
same positions from one fixed-seed PRNG, and the churn reallocates the size it
just freed so it cannot be refused.

**Workload.** 256 KiB region; fill with a steep size mix; every 16th object
pinned (a relocation barrier, which is what a DMA buffer actually is); release
every 4th movable object to scatter holes fenced in by live and pinned
neighbours. Then demand 12,288 contiguous bytes.

|  | Phase 1 — fragmented state, no churn | Phase 2 — under sustained churn |
|---|---|---|
| A: compaction disabled | **50 of 50 demands FAILED** | 3 of 500 failed |
| B: compaction on failure | **1 of 50 failed** | 0 of 500 failed |

Largest free block: minimum 2,056 B in both variants; final 76,288 B in both.
Structure audit OK, zero churn refusals (so neither run is confounded), in both.

**Cost.** One compaction, moving 189 objects / 184,296 bytes:

```
compact time : 0.061 ms raw, 0.053 ms corrected for the instrument bias
               (1 x 7,521 ns) => 53.2 us per compaction
```

Two honesty notes on that number:

- **The per-event maximum is deliberately not reported.** One clock read on this
  host has been measured at 2.7 ms. A "worst compaction" figure here would be an
  artefact of the clock, not of the allocator. Percentiles need the device, where
  the cycle counter is free — that is what `bench/esp32/` exists for.
- **~12 µs of the residual is still instrument** if the in-context bias applies,
  so the true value is around 40–53 µs. The figure is bounded, not exact.

**What the result supports.** Compaction converts a *completely unserviceable*
fragmented state (50/50 large demands fail) into a serviceable one (1/50, the
single failure being the one that triggered it), for the cost of one 53 µs
operation. It does not support "an uncompacted pool will always fail over time":
phase 2 shows the failure *rate* is low (3 of 500) because first-fit drift lets a
pool partly heal itself. Both phases are reported because they answer different
questions, and neither is a generalisation.

### Why the workload had to be shaped this way

Recorded because two earlier shapes produced misleading results:

1. **A narrow size mix gave zero failures.** `free()` coalesces adjacent blocks,
   so without a size mismatch there is no fragmentation to measure.
2. **A steep mix alone healed itself.** First-fit drift pushes live objects down
   and free space up, so variant A recovered on its own over a few hundred steps.
   This is a real property of the allocator, not an artefact.

What sustains fragmentation is **pinned** objects: they are relocation barriers,
so the free space between them can never be consolidated — not by `free()`, and
eventually not by compaction either. That is not a trick invented for the
benchmark; pinned is what DMA buffers are.

---

## 5. On the device: ESP32-S3 (n16r8)

The instrument here is not the host's, and `bench_timer.h` switches it at compile
time:

| | host (x86-64, VMware guest) | device (ESP32-S3) |
|---|---|---|
| backend | `std::chrono::steady_clock` | `esp_cpu_get_cycle_count` (`mcycle`) |
| clock call cost | 9,592 ns | **59 ns** |
| interval cost | 7,553 ns | **50 ns** |
| verdict printed | DO NOT QUOTE PER-OPERATION TIMINGS | per-operation timings are usable |

Everything below is therefore a real per-operation measurement, except where a
row is marked otherwise.

### 5.1 `alloc` + `free` is flat in the live count on the device too

| live | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|
| pair, ns | 9,252.9 | 9,215.2 | 9,193.0 | 9,183.9 | 9,180.7 |

Over a 16x range of live counts the pair moves by **x0.99**. Plan A's claim — the
address-ordered insertion walk was the whole cost of `alloc` — transfers to the
target.

The `free`/`alloc` split rows in this benchmark are all **REJECTED by the
cross-check**, at every size. That is the host-designed construction failing
rather than the allocator misbehaving: it divides the difference of two long
intervals by L, and it correctly refuses to print a figure it cannot support.
Section 5.2 measures the split directly instead, because on this platform that is
legitimate.

### 5.2 What the pair actually contains

Why this had to be checked before anything was concluded: ~9 µs for one
free+alloc is ~2,160 cycles at 240 MHz, which is ~20x more per cycle than the
host's ~34 ns (~100 cycles at 3.7 GHz). The churn harness picks its victim slot
with `rng() % g_live` — a 64-bit LCG and a 64-bit modulo, both of which are
software routines on a 32-bit target and both of which sit *inside the timed
interval*. Before blaming the allocator, the instrument had to be ruled out.

`bench/churn_overhead.cpp` separates them: row A takes its indices from a
pre-generated ring (so the address sequence is still random, but its generation
is outside the interval), row C times the generation alone, and row B is the
shape `alloc_latency.cpp` uses.

| component | ns/interval | ns/op |
|---|---|---|
| D loop + one array load (measurement floor) | 119,562 | 29.2 |
| C address generation only | 256,129 | 62.5 |
| A free+alloc, generation EXCLUDED | 36,896,567 | **9,008.0** |
| B free+alloc, generation INCLUDED | 37,189,637 | 9,079.5 |
| E alloc only, fill pattern | 1,011,046 | **3,949.4** |
| F free only, random order | 2,042,016 | **7,976.6** |

```
allocator cost, generation excluded (A - D) : 8,978.8 ns
allocator cost, differenced     (B - C)     : 9,017.0 ns
address generation cost         (C - D)     :    33.3 ns
generation share of B                       :     0.8 %
cross-check: the two allocator estimates differ by 38 ns (0%)
```

**The harness is not the explanation.** Two independent estimates of the
allocator's cost agree to 0.4%, and index generation accounts for 0.8% of the
pair. 9.0 µs per free+alloc is the allocator, measured.

Rows E and F are measured directly because on this platform per-operation numbers
are legitimate (bias 0.195 ns per op). **They are not the additive decomposition
of the pair:** E runs a fill — an allocation's address is chosen by first fit, so
a run of allocations necessarily walks up the arena and there is no way to
allocate "at a random address" — and F runs a full random-order drain. So
E + F = 11.9 µs against A = 9.0 µs is expected rather than a contradiction. What
they establish is the scale of each operation on this target: **`alloc` ≈ 3.9 µs
in a fill, `free` ≈ 8.0 µs in a random-order drain.**

### 5.3 Why ~20x per cycle, when a trivial loop is not

| | host | device | per-cycle ratio |
|---|---|---|---|
| loop + one array load | 2.1 ns (~6 cycles at 3.7 GHz) | 29.2 ns (~7 cycles at 240 MHz) | ~1.2x |
| free + alloc | ~34 ns (~100 cycles) | 9,008 ns (~2,160 cycles) | **~21x** |

A trivial loop costs about the same number of *cycles* on both platforms, so the
CPU's basic throughput is not the difference. The allocator's operations cost
~20x more cycles on the target, which points at memory and code access rather
than at the core: the host keeps the whole working set resident, while the target
fetches the core's code through the flash cache and walks descriptor and bin
structures that are wide relative to its cache line.

**This explanation is a hypothesis and is labelled as one.** It is recorded as
the next thing to investigate, and none of the numbers above depend on it.

### 5.4 Fragmentation, and an independent baseline

Same workload shape and the same constants as section 4, scaled to the region
(192 KiB, 10 KiB probe). Fairness rules are the section-4 ones, and they come
from the same CMake variables so the two builds cannot drift.

The baseline is **ESP-IDF's own heap — and it is not heap_4.** IDF v6.0.2 has no
heap_4 at all; `components/heap/tlsf/tlsf.c` is the allocator, i.e. TLSF. The
first on-target run of the baseline identified it without being asked, by
tripping `assert failed: tlsf_free tlsf.c:630 (!block_is_free(block) && "block
already marked as free")`. That assertion was a real bug in the baseline harness
(a failed same-size re-allocation used to leave a dangling pointer behind, which
the next visit to that slot double-freed); it is fixed, and it is also the
identification. TLSF being the baseline makes the comparison harder rather than
easier: it is a mature, widely deployed implementation of the same
segregated-fit family PondMerge belongs to, not something written to lose.

| | filled | live after scatter | Phase 1 | Phase 2 | largest free block (min) |
|---|---|---|---|---|---|
| PondMerge, compaction disabled | 191 | 144 | **50 of 50 FAILED** | 2 of 200 | 2,056 B |
| PondMerge, compaction on failure | 191 | 144 | **1 of 50 failed** | 0 of 200 | 2,056 B |
| IDF heap (TLSF); no compaction exists | 185 | 139 | **50 of 50 FAILED** | 0 of 199 | 2,048 B |

**This is the headline of section 5.** An independent, mature implementation
behaves exactly like PondMerge with compaction disabled — 50 of 50 large demands
fail — and PondMerge's compaction converts that completely unserviceable state
into a serviceable one. The core value claim of the library is now measured
against a third-party allocator rather than only against itself.

Both PondMerge variants report structure audit OK and zero churn refusals. Cost
of the compaction on the device:

```
compact time : 7.583 ms raw, 7.583 ms corrected (1 x 50 ns bias)
               => 7,583 us per compaction, moving 141 objects / 138,856 B
```

(~18 MB/s of relocation on a 240 MHz MCU, and that is the maintenance window a
caller has to budget for. Two independent runs gave 7.583 ms and 7.587 ms — a
0.05% spread, so this figure is reproducible in a way the host's is not.) It is
~143x the host's 53 µs, consistent with the per-cycle effect in section 5.3.

Three honesty notes on this table:

- **The baseline filled 185 objects where PondMerge filled 191** in the same
  region, because per-allocation overhead differs. The two are not
  byte-comparable, so the counts are reported rather than normalised away.
- **The baseline's churn refused one same-size re-allocation** (1 refusal, 199
  probes), so it carries the `CONFOUNDED` marker and its phase-2 count is not
  quotable. That is worth more than the count it perturbs: "a same-size
  re-allocation cannot be refused" is a fairness rule `fragmentation.cpp` relies
  on, and it held there (0 refusals in both variants) — but it is a property of
  a particular allocator, not a law. The baseline harness drops the slot on
  refusal and counts it, which is what turned a double-free abort into a number.
- **The baseline gets an accounting cross-check in place of `validate()`:**
  free 61,996 + allocated 131,584 = 193,580 of a 195,072 B region (1,492 B of
  heap metadata), and the allocator's own largest-free-block reading agrees with
  the one measured through the API. Getting that check right took two attempts —
  the first compared readings taken at different moments and reported a mismatch
  that meant nothing.

### 5.5 `validate` and `get_stats` on the device

| live | blocks | `validate` | per block | `get_stats` |
|---|---|---|---|---|
| 64 | 96 | 1,907.9 µs | 19,873.8 ns | 15,253 ns |
| 128 | 192 | 5,883.3 µs | 30,642.2 ns | 24,586 ns |
| 256 | 384 | **20,099.9 µs** | 52,343.4 ns | 43,253 ns |

Fitted exponent **1.70** — quadratic confirmed again, on the target.

The negative result transfers with a caveat: `get_stats` costs 43 µs at 384
blocks (112.6 ns/block), which is still only ~0.04% of a core at 10 Hz, so it is
**not worth optimising** — but that is 71x the host's per-block cost, not the
12.5x the clock difference alone would explain. Same effect as section 5.3.

`validate` at 20 ms for 384 blocks is a real constraint for anything that wants
to call it from a health monitor on an MCU. It is a diagnostic path; this is the
number that says so.

### 5.6 Metadata sizing does not transfer across pointer widths

| | x86-64 | ESP32-S3 |
|---|---|---|
| `sizeof(void*)` | 8 | **4** |
| `sizeof(ObjectDesc)` | 64 | **52** |
| `sizeof(Pool)` | 416 | 416 |
| `sizeof(TlsfBins)` | 364 | 364 |
| `metadata_bytes`, 256 objects / 16 pools | 32,776 (closed form) | **28,672 measured** |
| `metadata_bytes`, 1024 objects / 16 pools | 108,040 measured | not measured |

`ObjectDesc` holds a pointer, so the closed form published in `README.md`
(`72 + 476 x POOLS + 98 x OBJ`) is an **x86-64** form. On the target the
per-object term is smaller — 52 B of descriptor plus 30 B of plan scratch, i.e.
82 B per object against 98 B on x86-64. The exact figure for any configuration is
available on the target as `global_stats().metadata_bytes`, which is what a RAM
budget should use; the closed form is an illustration, not a contract.

---

## 6. What these numbers do not say

- **One device, one configuration.** Section 5 is a single ESP32-S3 at 240 MHz
  with a 192 KiB region and 256 objects, on a Release-semantics build
  (`PM_DEBUG=0`). The shapes should transfer; the absolutes are one part's.
- **No compaction-window percentiles.** The device clock is free, so the raw
  material exists, but the run above contains *one* compaction. A percentile
  needs many events, which needs a workload with many failed probes over a long
  run. Not measured.
- **No soak.** The device benchmark firmware is ~25 s end to end, and the
  acceptance suite was run twice from reset. Nothing here is a multi-hour
  endurance result, and nothing here is a substitute for one.
- **Section 5.3's explanation is a hypothesis.** The 20x per-cycle gap is
  measured; the reason offered for it is not.
- **Run-to-run spread is ±4%** on the pair metric and larger on anything
  per-event. Differences smaller than that are not meaningful in these tables.
- **The host's virtualisation changed during this work** (the TSC trap appeared
  after a guest reboot/resume). Absolute per-event host figures taken before and
  after that change are not comparable, which is a further reason the headline
  claims rest on counts and on the pair metric rather than on per-event host
  times.
