# PondMerge benchmark results

All figures on this page were produced by the programs in `bench/`, built from
the committed source, on an idle machine, and each run prints the identity and
the measured cost of its own timing instrument. Nothing here is transcribed from
a design document.

**Host:** AMD Ryzen 5 5600X, 3693 MHz, Ubuntu, `g++` 14, `-O2 -DNDEBUG
-DPM_DEBUG=0`. The host is a VMware guest; see the instrument section — that
matters more than the CPU does.

**Devices:** two, and they are not interchangeable.

| | ESP32-S3 (n16r8) | classic ESP32 (D0WDQ6 v1.1) |
|---|---|---|
| core | Xtensa **LX7**, 2 cores | Xtensa **LX6**, 2 cores |
| clock | 240 MHz | 240 MHz (set explicitly; IDF's default here is 160) |
| static DRAM | 512 KiB class | ~200 KiB total (`dram0_0_seg`) |
| shared region | 192 KiB, 48 segments | **112 KiB**, 28 segments (forced by DRAM) |
| console | USB-Serial-JTAG | UART0 behind a CH340 |
| section | 5 | **6** |

Firmware for both is `bench/esp32/`, one IDF project that compiles the **same**
benchmark sources as the host build; only the sizing macros and the target
defaults differ, and the one parameter that changes between the two chips is the
region size. The instrument is free enough on both for per-operation timing and
says so itself: 59 ns per clock read on the S3, 72 ns on the classic part, against
9,592 ns on the host — which is why the host numbers are batched and these are not.

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
| free + alloc | ~34 ns (~100 cycles) | 9,008 ns (~2,160 cycles) | **~20x** |

A trivial loop costs about the same number of *cycles* on both platforms, so the
CPU's basic throughput is not the difference: something about the allocator's own
operations costs ~20x more cycles on the target.

**The explanation first offered here was "memory and code access rather than CPU
throughput" — and section 5.7 measured it directly and found it wrong.** Both
cache-miss counters read **exactly zero**. The cost is instruction count and a
dependency-limited pipeline, not a stalled memory hierarchy. The guess is left in
place rather than deleted because the shape of the error is instructive: it was
plausible, it was repeated in an earlier README, and it took a counter to kill it.

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

### 5.7 Where the cycles actually go: not the cache

Section 5.3's guess was testable, so it was tested. The ESP32-S3's Xtensa
performance monitor counts **ICache-miss and DCache-miss penalty in cycles**, so
the share of an interval that is memory stall can be read off rather than
inferred, and the instruction counter gives CPI. `bench/esp32/main/perfcount.cpp`
runs the same three workloads as `churn_overhead.cpp`, with counter 0, counter 1
and `mcycle` sampled from the **same trial**, so every share below is exact rather
than an average of two runs.

| workload | icache miss penalty | dcache miss penalty | instructions | cycles | CPI |
|---|---|---|---|---|---|
| loop + one array load | **0** | **0** | 5.01/op | 5.02/op | 1.00 |
| address generation only | **0** | **0** | 12.01/op | 12.02/op | 1.00 |
| **free + alloc (index ring)** | **0** | **0** | **1,528.4/op** | **2,160.6/op** | **1.41** |

**Both cache-miss counters read exactly zero on all three workloads.** The
hypothesis in section 5.3 is dead: on this target the allocator does not stall on
cache misses at all. With hindsight it should not be surprising — the descriptor
table is 256 x 52 B, the bins are 364 B, and the churn keeps its blocks packed
into the low part of the region, so the working set fits.

Where the 2,160 cycles do go, from the same counters:

| | per op | share of the interval |
|---|---|---|
| instructions retired | 1,528.4 | (CPI 1.41) |
| **hold / bubble cycles** | **524.0** | **24.3%** |
| instruction-related stalls | 66.7 | 3.1% |
| data-related stalls | 0.8 | 0.04% |
| cache-miss penalty | 0 | 0% |

So the cost is **instruction count plus pipeline bubbles**: the target executes
1,528 instructions for one free+alloc and spends a quarter of its time waiting on
dependencies inside them. Nothing in the profile points at memory.

**The same code on x86-64 executes 757 instructions for the same pair**, measured
with callgrind (`bench/host_insn.cpp`; the counts are taken as a differential
between two run lengths so process startup cancels — the same trick the library's
own `free`/`alloc` attribution uses, for the same reason). So:

- **the target executes 2.0x the instructions per pair** — 1,528 against 757. That
  is the part of the gap which is about the instruction set and the 32-bit ABI
  rather than about the two chips;
- the remainder is cycles-per-instruction. On the target that is 1.41, with the
  bubbles above as the mechanism. The host's equivalent is **not** quoted here:
  `perf` events are blocked in this VM (`perf_event_paranoid`), and dividing the
  host's measured time by its nominal clock would repeat exactly the mistake
  section 0 exists to prevent — the host's real clock under a hypervisor is not
  the number `lscpu` prints. The honest position is therefore asymmetric and is
  stated as such: **the instruction-count half of the gap is measured on both
  sides, the CPI half only on the target.**

What this changes: the "memory and code access" story should not be repeated, here
or anywhere else. The target's disadvantage is that this code compiles to ~2x the
instructions *and* that the core retires them far less efficiently — both
properties of the core and the code, not of where the bytes live. It also
retro-explains section 6.3: a second chip with a *faster* trivial loop but *slower*
allocator operations is what an instruction-count-and-dependency story predicts,
and not what a memory-latency one does.

Caveat on the method: the perfmon has two counters, so `perfcount.cpp` runs three
passes (cache misses; instructions and data stalls; instruction stalls and
bubbles) instead of reading everything at once. Each pass is internally exact, but
the three are three executions. On this device that costs nothing, because the
runs are byte-identical to one another (section 6.6).

### 5.8 The compaction window as a distribution (512 events, percentiles)

Every compaction-cost figure above this section is ONE event: section 4's 53 us
on the host, section 5's 7.583/7.587 ms here. `bench/compaction_window.cpp` makes
the event repeatable: it reproduces this section's fill/scatter state (see the
pinned correction in bench/README.md section 4 -- the scatter releases the
pinned posts too, which a probe of the exact sequence confirms), takes the
recorded consolidation as an untimed settle plus a refill to full, and then
cycles: free K=14..20 random movable objects, demand 10 KiB (fails), read-only
advice, ONE timed `compact()`, retry, refill the exact freed sizes in order.
512 events per run.

Setup: this chip at 240 MHz, 192 KiB region, `PM_MAX_OBJECTS=256`, `PM_DEBUG=0`,
watchdogs off. App version `v1.0.0-13-g315fce7`, two independent captures.

Probe outcomes per run: pre-compact ok 0, rescued by compact **512**, still
failed 0; refill refusals 0; structure audit OK. The window (one `compact()`
call, i.e. the whole pause a caller budgets for), nearest-rank percentiles in
microseconds:

| run | min | p50 | p75 | p90 | p95 | p99 | max | mean |
|---|---|---|---|---|---|---|---|---|
| 1 | 6,541.4 | 8,977.65 | 9,178.98 | 9,350.57 | 9,431.49 | 9,558.16 | 9,743.42 | 8,864.05 |
| 2 | 6,546.26 | 8,977.78 | 9,179.13 | 9,346.95 | 9,430.75 | 9,562.96 | 9,743.58 | 8,864.15 |

Moved bytes per event: min 118,216 / mean 164,182 / max 181,392, **identical in
both runs** -- the workload is state-deterministic (fixed seed), and every
state-level aggregate reproduces byte-for-byte. The timing layer does not:
p50, p75, p95 and the mean differ between the two runs by <= 0.008%, p90 and
p99 by 0.04-0.05%, the min (a single event) by 0.07%, with a plausible cause
(the periodic tick's phase relative to each trial). The earlier
"three runs byte-identical" claim (section 6.6) is therefore build-scoped, not a
law: a rebuild moved the single-event figure of section 5.6 from 7,583 us
(the recorded build) to 7,589.9 us (this round's build, byte-identical in both
of its runs; an intermediate build gave 7,587.3) -- ~0.1% of build-to-build
code-layout sensitivity, on top of the tick-phase spread already recorded
there.

The window rises with the work, as it should -- mean window per moved-bytes
quintile of the window itself (run 1; run 2 agrees to the fourth digit):

| quintile | mean window (us) | moved bytes |
|---|---|---|
| Q1 (shortest) | 8,128.4 | 118,216..157,200 |
| Q2 | 8,702.5 | 157,152..164,392 |
| Q3 | 8,976.2 | 164,160..168,512 |
| Q4 | 9,141.4 | 168,264..171,128 |
| Q5 (longest) | 9,365.6 | 171,128..181,392 |

At the means this is 8.864 ms / 164,182 B = **54.0 ns/B = 18.5 MB/s**, against
the single event's 18.3 MB/s in section 5.6 -- the distribution and the
one-point record agree.

Two cross-checks are printed by the benchmark and were checked on both runs:

- **The advice's move estimate equalled what compact actually moved on 512 of
  512 events** (max deviation 0 objects / 0 bytes, 0 UNKNOWN). The R35 claim
  that the estimate simulates the exact packing rules has been fault-injection
  tested; this is the first time it has been checked on hardware across a few
  hundred different fragmentation patterns. CI now asserts the host copy of
  this line.
- **The library's own esp_timer microseconds vs the mcycle figure**: max
  deviation 9-10 us over 512 events -- the two independent clocks agree to
  within the esp_timer tick.

Instrument: the interval bias is 50 ns per event, five orders of magnitude
below the median window, so these percentiles are quotable in a way no host
per-event figure can be (section 7).

What this section does NOT establish:

- **One regime, one part.** The distribution is for a full, pin-free pool with
  14-20 dispersed holes at 100% occupancy. The classic ESP32's percentiles are
  not taken (the board was offline this round; its bench firmware build records
  128 events for .bss reasons). And a pinned regime is NOT part of the record:
  a development build that kept the pins alive during the cycles (barrier every
  16 objects) saw compact() strand the free space below each barrier into ~1 KiB
  pockets, and the probe retry failed after EVERY one of 512 compactions
  (rescued=0). That is a real property of dense barriers --
  COMPACTION_POLICY.md section 7 predicts it qualitatively -- and it means
  "compact to satisfy a demand" can structurally fail with pins intact even
  when total free is sufficient. It is recorded here as an observation because
  it is a single dev-build run, not a measured distribution.
- **No soak, still.** The 512 events span ~4.5 s of compaction inside a ~30 s
  firmware; nothing here is an endurance result.

---

## 6. A second device: classic ESP32 (ESP32-D0WDQ6, LX6)

Section 5 is one part, which makes "the device" and "this ESP32-S3" the same
thing. This section separates them: a different Xtensa core, a different cache,
and a different amount of DRAM.

**Platform.** ESP32-D0WDQ6 revision v1.1, dual-core **LX6**, 240 MHz — the *same*
clock as the S3 run, set explicitly because IDF's default for this target is
160 MHz and a different clock would have turned every per-cycle comparison into a
division problem. `-O2`, `PM_DEBUG=0`, 4 MB flash, UART0 console behind a CH340
bridge, **112 KiB shared region** as 28 x 4 KiB segments, `PM_MAX_OBJECTS=256`,
`PM_MAX_POOLS=16`, `PM_SL_COUNT=4`.

**The region is the only thing that had to change, and it changed because of DRAM.**
This part has ~200 KiB of static DRAM in total (`dram0_0_seg` = 204,800 B). The
S3's 192 KiB region does not fit — and neither does 128 KiB, which the linker
rejected with `region 'dram0_0_seg' overflowed by 4208 bytes`. 112 KiB leaves
~12 KiB of margin. **Every other benchmark parameter is identical on both targets**
(live counts, churn intervals, probe size, pin cadence, scatter cadence), because
a second chip running a differently-shaped experiment would not be a second data
point. The instrument reports itself here too: 72 ns per clock read against the
S3's 59 ns, both far below the operations being timed.

### 6.1 The flatness reproduces, on a different core

| live | 16 | 32 | 64 | 128 | 256 |
|---|---|---|---|---|---|
| ESP32-S3 (LX7) | 9,253.1 | 9,215.0 | 9,193.0 | 9,184.0 | 9,180.5 ns |
| classic ESP32 (LX6) | 10,092.4 | 10,044.6 | 10,018.2 | 10,007.6 | 10,003.4 ns |

Both **x0.99** over a 16x range of live counts. Plan A's result — the address-ordered
insertion walk was the whole cost of `alloc` — holds on two different cores.

### 6.2 The harness decomposition reproduces

| component | S3 ns/op | LX6 ns/op |
|---|---|---|
| D loop + one array load (floor) | 29.2 | 25.0 |
| C address generation only | 62.5 | 62.8 |
| A free+alloc, generation excluded | 9,007.8 | 9,804.2 |
| B free+alloc, generation included | 9,079.6 | 9,897.8 |
| E alloc only, fill pattern | 3,949.4 | 4,216.9 |
| F free only, random order | 7,976.5 | 9,071.4 |

On the LX6: allocator cost `A - D` = 9,779.1 ns against the differenced estimate
`B - C` = 9,835.0 ns — two independent estimates agreeing to 0.6%, with index
generation at 0.9% of the pair. The "the harness is measuring itself" worry was
answered the same way on both parts.

### 6.3 The two devices agree on the shape and disagree on the constant

| | ESP32-S3 (LX7) | classic (LX6) | LX6 / S3 |
|---|---|---|---|
| loop + one array load | 29.2 ns | **25.0 ns** | **0.86** |
| allocator cost (A - D) | 8,978.6 ns | 9,779.1 ns | 1.09 |
| `alloc` only (E) | 3,949.4 ns | 4,216.9 ns | 1.07 |
| `free` only (F) | 7,976.5 ns | 9,071.4 ns | 1.14 |
| compaction, per byte | 18.3 MB/s | **22.3 MB/s** | 1.22 |
| `validate`, 384 blocks | 20.10 ms | 24.47 ms | 1.22 |
| `get_stats`, per block | 112.6 ns | 127.0 ns | 1.13 |

**The trivial loop is FASTER on the LX6 while every allocator operation is
SLOWER.** That is evidence against the simplest reading of section 5.3 — "the
target's CPU is just slow" — because the loop costs *fewer* cycles on the part
that loses on everything else. It does not prove the memory/code-access
hypothesis either: the two cores differ in cache, in pipeline, and in how the
flash cache is wired, so this is a second instance of the pattern rather than an
explanation of it. What it does establish is that the ~20x device/host gap is not
a uniform clock effect.

Section 5.7 was written after this and settles it: the cache-miss counters on
the S3 read zero, so the memory story is refuted and this table's pattern is what
an instruction-count-and-dependency explanation predicts — a core that runs a
simple loop faster can still run a long dependency-chained path slower.

### 6.4 Fragmentation: same conclusion, larger magnitude, and now quotable

| | filled | live | Phase 1 | Phase 2 | largest free (min) |
|---|---|---|---|---|---|
| PondMerge, compaction disabled | 112 | 84 | **50 of 50 FAILED** | 1 of 200 | 2,056 B |
| PondMerge, compaction on failure | 112 | 84 | **1 of 50 failed** | 0 of 200 | 2,056 B |
| IDF heap (TLSF) | 107 | 81 | **50 of 50 FAILED** | **21 of 200 FAILED** | 2,048 B |

Two things are better here than on the S3:

- **The baseline's phase-2 count is quotable this time.** The S3 run had 1 churn
  refusal in 199 probes and was marked `CONFOUNDED`; this one had **zero**, so the
  number stands. An uncompacted pool loses **~10% of its large demands under
  sustained churn**, and PondMerge with compaction loses none. On the S3 the same
  comparison was 0 of 199, which was true but weak; here it is a magnitude.
- `filled` and `live` are smaller because the region is: 112 objects against 191.
  That is a consequence of the DRAM budget, not of the allocator, which is why the
  counts are reported beside each other rather than normalised.

Compaction cost: **3.5388 ms moving 81 objects / 78,984 B** — 22.3 MB/s against the
S3's 18.3 MB/s, so on this part the maintenance window is both smaller and faster
per byte moved.

### 6.5 `validate` and `get_stats`

| live | blocks | `validate` | per block | `get_stats` |
|---|---|---|---|---|
| 64 | 96 | 2,325.0 µs | 24,218.4 ns | 17,154 ns |
| 128 | 192 | 7,165.4 µs | 37,320.0 ns | 27,687.5 ns |
| 256 | 384 | **24,474.4 µs** | 63,735.4 ns | 48,754 ns |

Fitted exponent **1.70** — the same figure the S3 gave, from a different core.
`get_stats` is 0.75 on both and 127.0 ns/block here, so the negative result
transfers unchanged.

### 6.6 What this section does not establish

- **Three runs, and all three are byte-identical.** Every figure in this section —
  including the *median* columns, not just the minima — is identical across three
  separate power-on runs of the same firmware, so this is not a single-run record
  after all and there is no run-to-run spread to quote for this part. That is a
  stronger statement than the host can make (see section 7) and it has a mundane
  explanation: the workload is fully determined by fixed seeds, the code is `-O2`,
  the flash cache is deterministic, and the periodic tick lands at the same phase
  in each trial, so the per-interval cycle count is a reproducible function of the
  code path. Two runs were captured with `tests/serial_cap.py`; the third, after a
  re-plug, with the same tool plus a retry on the open. The run's own self-checks
  all passed: structure audit OK on both fragmentation variants, the IDF-heap
  accounting consistent, `validate` OK, and the pair/free+alloc cross-check in
  section 6.2 agreeing to 0.6%.

  What identical runs do *not* establish is that the figures are right — only that
  they are reproducible. A systematic error would reproduce just as exactly.
- **A different region size**, so `filled`, `live` and the compaction's byte count
  are not comparable across the two chips. The live counts (16–256), the block
  counts (96–384) and the probe size (10 KiB) are, and those are what the tables
  above compare.
- **Two points is not a trend.** Section 6.3's per-cycle difference is measured;
  its cause is not.
- **Device-side operational note, recorded because it cost two attempts.** The
  CH340 bridge on this board can go unresponsive: `/dev/ttyUSB0` stays enumerated
  and `lsusb` still lists the chip, but every read fails with EIO while the kernel
  logs `ch341-uart: failed to send control message: -110` (ETIMEDOUT on a control
  transfer — retried by the driver, and not fatal to the device). A re-plug clears
  it; there is no software fix over SSH without root. **The ESP32 itself is
  unaffected, so check the kernel log before suspecting the firmware.**

  The same chip also fails the *port open* intermittently — measured directly: the
  first open raised `OSError [Errno 5]` and the immediate next one succeeded and
  captured the whole run. `tests/serial_cap.py` now retries the open up to 20
  times, which is what turned a confusing traceback into this section's third run.

---

## 7. What these numbers do not say

- **Two devices, two configurations.** Section 5 is an ESP32-S3 at 240 MHz with a
  192 KiB region; section 6 is a classic ESP32 at the same clock with a 112 KiB
  region. Both are Release-semantics builds (`PM_DEBUG=0`). The shapes transfer;
  the absolutes are those two parts'.
- **Compaction-window percentiles exist for ONE regime and ONE part** (section
  5.8: S3, full pin-free pool, 14-20 dispersed holes). The classic part's
  percentiles are not taken, a pinned-barrier distribution was never recorded
  (only the dev-build observation in 5.8), and no other regime has been swept.
- **No soak.** The device benchmark firmware is ~28 s end to end, and the
  acceptance suite was run twice from reset on each of two parts. Nothing here is
  a multi-hour endurance result, and nothing here is a substitute for one.
- **Section 5.3's explanation is no longer open** -- section 5.7 measured it and
  it is dead (both miss counters exactly zero; 1,528 instructions at CPI 1.41).
  What remains un-measured is the second chip's own decomposition: section 6.3's
  per-cycle difference is a fact whose per-chip cause nobody has taken apart
  (section 6.6's "two points is not a trend").
- **Run-to-run spread is ±4% on the host** on the pair metric, and larger on
  anything per-event. That is a property of the measurement host (a VM with a
  trapped `RDTSC`), not of the library, and it is the reason the host tables quote
  one representative run. **The device record is not like this**: section 6's three
  runs are byte-identical including the medians, so its figures carry no spread
  term. Differences smaller than the host's ±4% are therefore not meaningful *in
  the host tables* — but they would be in section 6's.
- **The host's virtualisation has changed more than once during this work** (the
  TSC trap appeared after a guest reboot/resume, and has since disappeared again:
  2026-09-18 the instrument self-reports a 19-26 ns clock cost where section 0
  records ~9,400 ns). There are therefore three instrument regimes across this
  file's host history, and absolute per-event host figures taken under different
  regimes are not comparable -- which is a further reason the headline claims
  rest on counts and on the pair metric rather than on per-event host times.
  Check the timer's self-report line at the top of a host run before quoting
  anything per-event from it.
