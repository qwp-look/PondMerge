# PondMerge benchmark results

All figures on this page were produced by the programs in `bench/`, built from
the committed source, on an idle machine, and each run prints the identity and
the measured cost of its own timing instrument. Nothing here is transcribed from
a design document.

**Host:** AMD Ryzen 5 5600X, 3693 MHz, Ubuntu, `g++` 14, `-O2 -DNDEBUG
-DPM_DEBUG=0`. The host is a VMware guest; see the instrument section — that
matters more than the CPU does.

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

## 5. What these numbers do not say

- **No device numbers.** Everything here is x86-64; the *shapes* are
  architecture-independent, the absolute values are not. `bench/esp32/` builds
  the same sources for the ESP32-S3 (verified: the project configures, compiles
  and links, `pondmerge_bench.elf` 3.7 MB, DIRAM 204,361 / 341,760 B = 59.8%),
  but **the board was not attached when the run was attempted**, so nothing was
  flashed and no console output was captured. This is an outstanding step, not a
  negative result: the kernel log records
  `usb 1-2.1: USB disconnect` immediately before the attempt and `lsusb` shows no
  Espressif device. Three things become possible there that are *not* possible
  here, which is why it matters: absolute per-operation costs, compaction-window
  percentiles (`esp_cpu_get_cycle_count()` is a free register read, whereas one
  clock read here has been seen to take 2.7 ms), and an independent baseline.
- **No independent allocator baseline yet, on purpose.** The comparison shipped
  here is compaction on vs off within one allocator — the question that can be
  answered without a straw man. Comparing against FreeRTOS `heap_4` belongs on
  the device, using `heap_caps_add_region` to put it in its own pool, not against
  something written for the occasion.
- **Run-to-run spread is ±4%** on the pair metric and larger on anything
  per-event. Differences smaller than that are not meaningful in these tables.
- **The host's virtualisation changed during this work** (the TSC trap appeared
  after a guest reboot/resume). Absolute per-event figures taken before and after
  that change are not comparable, which is a further reason the headline claims
  rest on counts and on the pair metric rather than on per-event times.
