# Changelog

All notable changes to PondMerge are documented here.
This project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **A hypothesis in this file's own history was measured and killed: the ~20x
  device/host per-cycle gap is NOT cache misses.** `bench/RESULTS.md` section 5.3
  offered "memory and code access rather than CPU throughput" as the explanation
  and labelled it a guess. The ESP32-S3's Xtensa performance monitor counts
  ICache-miss and DCache-miss penalty *in cycles*, so the guess was testable, and
  `bench/esp32/main/perfcount.cpp` tested it on the same three workloads as
  `churn_overhead.cpp`, with both counters and `mcycle` sampled from the same
  trial so every share is exact:

  | workload | icache miss penalty | dcache miss penalty | instructions | cycles | CPI |
  |---|---|---|---|---|---|
  | loop + one array load | **0** | **0** | 5.01/op | 5.02/op | 1.00 |
  | **free + alloc** | **0** | **0** | **1,528.4/op** | **2,160.6/op** | **1.41** |

  Both counters read exactly zero, on all three workloads. The cost is
  **instruction count plus pipeline bubbles**: 1,528 instructions per free+alloc
  pair, 24.3% of the interval in hold/bubble cycles, 3.1% in instruction-side
  stalls, 0.04% in data-side stalls, and nothing at all in cache.

  `bench/host_insn.cpp` then measured the same pair on x86-64 with callgrind, as a
  differential between two run lengths so process startup cancels: **757
  instructions per pair**. So the target executes **2.0x the instructions** for
  the same work — that half of the gap is about the instruction set and the 32-bit
  ABI, and it is measured on both sides. The other half is
  cycles-per-instruction, and only the target's is quotable: `perf` events are
  blocked in this VM, and dividing the host's measured time by its nominal clock
  would repeat the exact mistake the instrument section exists to prevent. The
  asymmetry is stated rather than papered over.

  Section 5.3 keeps the refuted text in place, with the refutation next to it,
  because the shape of the error is instructive: the guess was plausible, it was
  repeated in an earlier README, and it took a counter to kill it. Section 6.3's
  cross-chip puzzle is also settled by this — a core that runs a simple loop
  faster while running a long dependency-chained path slower is what an
  instruction-count explanation predicts, not what a memory-latency one does.

- **The benchmarks now run on a second chip, and one conclusion went from "true"
  to "true and quantified".** `bench/esp32/` builds and runs on a classic ESP32
  (ESP32-D0WDQ6, dual-core **LX6**) as well as on the ESP32-S3 (LX7), at the SAME
  clock — 240 MHz, set explicitly because IDF defaults this part to 160, and a
  different clock would have turned every per-cycle comparison into a division
  problem. Every benchmark parameter except the region size is identical on the
  two targets. Full numbers in `bench/RESULTS.md` section 6; the short version:

    * the alloc/free pair is flat in the live count on both cores (**x0.99**);
    * the churn decomposition reproduces: two independent estimates of the
      allocator's own cost agree to 0.6%, and index generation is 0.9% of the pair;
    * `validate` fits exponent **1.70** on both, `get_stats` **0.75** on both;
    * fragmentation: **50 of 50** large demands fail with compaction disabled and
      **1 of 50** with it enabled, on **both** parts — and the independent TLSF
      baseline fails **50 of 50** on both;
    * **the TLSF baseline's sustained-churn count is quotable now.** On the S3 it
      was 0 of 199 but marked `CONFOUNDED` by a single same-size re-allocation
      refusal; on the classic part there were **no** refusals, so the number
      stands: the uncompacted baseline loses **21 of 200** (10%) of its large
      demands while PondMerge with compaction loses none. On the S3 that
      comparison was true but weak; here it is a magnitude.

  One result is new rather than repeated: **a trivial loop is FASTER on the LX6
  (25.0 ns against 29.2 ns) while every allocator operation is SLOWER (1.07x to
  1.14x)**. That argues against the simplest reading of the ~20x device/host gap —
  "the target's CPU is just slow" — because the loop costs *fewer* cycles on the
  part that loses on everything else. It does not prove the memory/code-access
  hypothesis either; it is a second instance of the pattern, not an explanation.

- **`bench/esp32/` is a two-target project**, using the same two-file
  `SDKCONFIG_DEFAULTS` + `set-target` mechanism as `esp32/`, with its own
  `bench/esp32/sdkconfig.defaults.esp32` (4 MB flash, UART0 console, explicit
  240 MHz). The region size and the population it determines are the only things
  the fork changes: this part has ~200 KiB of static DRAM in total, the S3's
  192 KiB region does not fit, and neither does 128 KiB — the linker rejected that
  with `region 'dram0_0_seg' overflowed by 4208 bytes` — so it is 112 KiB, with
  ~12 KiB of margin. `PM_BENCH_SEGMENTS` is now **derived** from the region size
  rather than being a hand-maintained 48, so a pool whose segment count disagrees
  with its region is no longer expressible.

### Fixed

- **A USB-UART bridge can fail in two ways that both look like broken firmware,
  and both are now handled.** Measured on the classic-ESP32 board's CH340:

    * the bridge can go unresponsive — `/dev/ttyUSB0` stays enumerated and `lsusb`
      still lists the chip, but every read fails with `EIO` while the kernel logs
      `ch341-uart: failed to send control message: -110`. A re-plug clears it;
      there is no software fix over SSH without root. The ESP32 itself is
      unaffected.
    * the **port open** fails intermittently with the same `EIO` — measured
      directly: `serial_cap.py`'s first open raised `OSError [Errno 5]`, and the
      immediate next attempt opened and captured the whole run.

  The second was fixed rather than documented: `tests/serial_cap.py` now retries
  the open (20 attempts, with the evidence in a comment), because one transient
  failure used to abort a capture that would have worked while the traceback
  pointed at the port rather than at the real story. The first is recorded in
  `bench/RESULTS.md` section 6.6 as an operational note, since
  `journalctl -k | grep usb` answers it in one command.

- **The classic-ESP32 benchmark record is three runs, and all three are
  byte-identical** — medians included, not just the minima — so what was written
  up as a single-run limitation is not one, and this part carries **no run-to-run
  spread term at all**. The explanation is mundane: fixed seeds, `-O2`, a
  deterministic flash cache, and a periodic tick at the same phase in every trial
  make the per-interval cycle count a reproducible function of the code path. The
  host cannot do this, which is why its tables quote one run and a ±4% spread.
  Identical runs prove reproducibility and not correctness — a systematic error
  would reproduce just as exactly, and the section says so.

- **A second hardware target, and the reason it is worth having: the dual-core
  lock claim now has evidence from two different cores.** The acceptance firmware
  builds and runs on a classic ESP32 (ESP32-D0WDQ6 v1.1, dual-core **LX6**) as well
  as on the ESP32-S3 (LX7). On the classic part:

  ```
  === suite SKIPPED (not run, not passed) ===
  dual-core concurrency : 28 checks, 0 failures PASSED
  reference model       : 466,859 checks, 0 failures PASSED
  ```

  The project states that SMP/lock semantics can only be proven by the dual-core
  device test, because the host `PM_LOCK` is a no-op. Until now that evidence
  existed on exactly one chip. Two runs from reset produce identical check counts
  while the scheduling-dependent counters differ, and the classic part exercises
  the race harder than the S3 did: 30 of the maintainer's 200 compactions were
  refused because a borrow was live, and both borrowers observed paused windows
  (the S3 run had one borrower at zero paused hits).

  One number is worth singling out: the reference-model differential reports
  **466,859 checks on host, on the S3 and on the classic ESP32** — one
  deterministic differential test, three architectures, the same number of
  judgements.

- **`esp32/` is now a two-target project**, and the reason it has to be explicit
  is recorded rather than left to the reader. ESP-IDF does not read
  `sdkconfig.defaults.<target>` on its own (verified in `tools/cmake/project.cmake`),
  and a second file in `SDKCONFIG_DEFAULTS` can override every key *except* the
  target — IDF guesses the target with a first-match rule. So switching targets
  takes `idf.py set-target`, and the classic ESP32 has its own
  `esp32/sdkconfig.defaults.esp32` (4 MB flash, UART0 console, 8 KiB main stack).
  Applying those overrides to an `esp32s3` build breaks it, which is what a
  wrong-target build looks like and is why the flow is documented in both files.

### Fixed

- **The acceptance suite cannot run on a small-DRAM part, and nothing said so.**
  `tests/suite.cpp` pins its own zone to 64 segments inside its assertions: test
  [10] asserts that 16 pools x 4 segments fills the zone, and test [2] creates a
  32-segment pool. So the zone size is part of what those tests assert, not a
  parameter they tolerate, and the suite is a 256 KiB-zone artifact by
  construction. A classic ESP32 has ~200 KiB of static DRAM in total and the link
  fails with `region 'dram0_0_seg' overflowed by 126,744 bytes`. The suite is now
  excluded per target, the firmware announces the skip loudly instead of letting a
  reader mistake it for a pass, and `tests/suite.cpp` carries the reason next to
  the buffer so nobody shortens it and wonders why tests fail.

- **`pondmerge_run_model()` clamped its zone argument to a hard-coded 256 KiB**
  while borrowing a buffer whose size it did not know. That was harmless while
  every device target had a 256 KiB buffer; on a part with a smaller one the clamp
  would have allowed an overrun. It now clamps against the array's own size, which
  requires declaring the array with its size — hence the shared
  `PM_TEST_ZONE_BYTES` macro.

- **`README.md` claimed the current-commit device acceptance had not been run**
  ("board disconnected"). It had been, in the previous round, and the numbers were
  already in `CHANGELOG.md`. Both READMEs now carry the real on-hardware rows.

- **On-target benchmark results, and an independent allocator baseline.**
  `bench/esp32/` runs four benchmarks — alloc latency, a churn-harness
  decomposition, the fragmentation A/B, and validate/`get_stats` scaling — plus a
  fifth section that measures ESP-IDF's own allocator over the same region, on
  the same workload, with the same constants (passed to both from one CMake
  variable so they cannot drift). The baseline is **TLSF**: IDF v6.0.2 has no
  heap_4, `components/heap/tlsf/tlsf.c` is the allocator. The first on-target run
  identified it unprompted by tripping a `tlsf_free` assertion — which was a real
  bug in the baseline harness (a refused same-size re-allocation left a dangling
  pointer, and the next visit to that slot double-freed); fixed, and the refusal
  is now counted and the run flagged.

  Headline: with compaction disabled, PondMerge and TLSF **both fail 50 of 50**
  large demands in the fragmented state; with compaction triggered on failure,
  PondMerge fails 1 of 50. The library's core claim is now measured against a
  mature third-party implementation of the same segregated-fit family rather than
  only against itself. Cost on the target: one compaction, **7.583 ms**, moving
  141 objects / 138,856 B (about 18 MB/s on a 240 MHz MCU).

- **`bench/churn_overhead.cpp`**, which decomposes the measured churn pair into
  allocator cost and harness cost. The churn loop chooses its victim slot with a
  64-bit LCG and a 64-bit modulo *inside the timed interval*, and on a 32-bit
  target both are software routines — so before the ~9 us free+alloc measured on
  the device could be blamed on the allocator, the harness had to be ruled out.
  Two independent estimates of the allocator's cost agree to 0.4% and index
  generation accounts for 0.8% of the pair. The benchmark also refuses to quote
  its own direct `alloc`/`free` rows when the platform's clock is too expensive
  for them, which is the case on the measurement host and is printed as such.

- **Device acceptance re-verified after the allocator change.** The ESP32-S3
  build of the acceptance suite runs **1,120,457 checks, 0 failures**, plus 28
  concurrency checks and 466,859 reference-model checks, and two runs from reset
  produce identical counts. The suite count moved by exactly **+1** from
  1,120,456, matching the host suite's +1 for the same reason — R29(5) is a
  rewritten test carrying one extra assertion — which is an independent check
  that the two builds are measuring the same code.

### Fixed

- **The `metadata_bytes` sizing claim was wrong about being platform-independent.**
  `README.md` published `72 + 476 x POOLS + 98 x OBJ` and stated that it held for
  host and ESP32 alike. It is an **x86-64** closed form: `ObjectDesc` contains a
  pointer, so on a 32-bit target it is 52 B rather than 64 B and the per-object
  term is ~82 B rather than 98 B. Measured on the ESP32-S3 at 256 objects and 16
  pools: **28,672 B**, against the closed form's 32,776 B. `metadata_bytes`
  itself is exact on every ABI, and both `README.md` and `docs/USAGE_GUIDE.md` now
  say to read it at runtime for a RAM budget instead of extrapolating. The device
  benchmark prints the target ABI's `sizeof` values on every run, which is how
  this was found.

- **Two host-only statements were being printed unchanged on the device.** The
  "per-event WORST is not reported, percentiles belong on the device" note is
  false where the cycle counter is a register read and a per-event figure is
  legitimate; it is now emitted only where the clock is expensive enough for it to
  be true, and the opposite case says so instead. Separately, the baseline
  harness's accounting cross-check compared two readings taken at different
  moments and so reported a mismatch that meant nothing; both readings are now
  taken at one instant.

- **The host port was instrumenting itself, at ~15x the cost of the work it was
  measuring.** `pm_port_ticks_us()`, which fills in `compact_time_us` — the
  library's only performance statistic — was implemented with `clock()`, i.e.
  `CLOCK_PROCESS_CPUTIME_ID`. On glibc that is a real syscall, measured at
  **20.9 us per call**, and `compact()` calls it twice. That added ~42 us of
  self-measurement to every compaction, against 2.8 us for the 184 KB copy it
  actually performs. It also made `compact_time_us` a *CPU-time* figure, so a
  maintenance window that was preempted was under-reported and could not be
  compared with any wall-clock budget.

  The host branch now uses `CLOCK_MONOTONIC`, which is the right quantity for a
  maintenance window and an order of magnitude cheaper. Measured effect: the
  fragmentation benchmark's compaction falls from 80 us to 61 us of raw wall
  time purely from removing the library's own clock reads. The ESP32 branch is
  unchanged — `esp_timer_get_time()` was already correct and is not a syscall.

  No test asserted on the value, so only the reported magnitude changes.

### Changed

- **`bench/esp32/` is reproducible from the repository.** Its generated
  `sdkconfig` is no longer committed; `sdkconfig.defaults` replaces it and pins
  the measurement configuration: `-O2` at 240 MHz (a benchmark should not be
  measured through a debug build), the USB-Serial-JTAG console, a 16 KiB main-task
  stack, and **the task and interrupt watchdogs off**. A churn interval is a
  long CPU-bound loop with no blocking call in it, which is exactly what the task
  watchdog exists to catch, and its output over USB-Serial-JTAG was injecting
  hundreds of microseconds into the intervals being measured — the first run
  tripped it three times and every `free`/`alloc` attribution was rejected.

- **Every absolute figure in `bench/RESULTS.md` has been re-measured**, after
  discovering that the timing instrument on the measurement host was broken. A
  VMware guest with a trapped `RDTSC` makes
  `std::chrono::steady_clock::now()` cost ~9,400 ns against operations of 36-90
  ns: the instrument was 100-250x the thing it measured, and the resulting
  numbers looked plausible while being dominated by the clock. The *shapes* of
  every conclusion survived; the absolute values did not.

  `bench/bench_timer.h` now enforces the rule that replaces per-operation timing:
  an interval contains N operations between two clock reads, so the instrument
  contributes `2 * call_cost / N` instead of `2 * call_cost`; every benchmark
  self-reports its backend, the measured clock cost and whether per-operation
  figures are supportable; and no benchmark emits a per-operation timing unless
  the operation is far longer than the clock, in which case the bias is stated.
  This is machine-independent, so the same sources give valid numbers on a host,
  in a VM and on the device.

- **`alloc` is no longer linear in the live-object count.** It used to insert
  each new descriptor into an address-ordered list, and that insertion measured
  as essentially 100% of alloc's cost, growing linearly with the live count.
  Re-measured A/B with the same benchmark source compiled against both core
  revisions: **844.2 ns before, 38.4 ns after**, at 1024 live objects (22x);
  58.0 ns to 38.0 ns at 16 live objects. Address order is needed only by the
  maintenance paths, so it is now re-established once per maintenance call on
  the cold path: alloc does an O(1) append, and `collect_live_sorted()` performs
  a bounded collection plus an in-place heapsort. With `PM_MAX_OBJECTS=256` the
  pair cost is 36.7 ns and equally flat. `free` and `validate` show no
  regression, and the fragmentation benchmark's A/B results are identical, so
  only the cost moved, not the behaviour.

  The honest complexity statement changes accordingly: `alloc` is now
  O(bin chain length) rather than O(bin chain length + live objects);
  `compact` / `split` / `merge` / `analyze_compaction` gain an
  O(objects log objects) sort term, which is negligible next to their memmove.

  **One detection boundary moves with it.** Because alloc no longer traverses
  the live-slot list, it no longer reports `CorruptMetadata` for a damaged one
  (cyclic or truncated). That detection now belongs to every maintenance entry
  and to `validate()`, still in bounded time and still with zero side effects.
  The API and the set of status codes are unchanged. This is recorded as an
  accepted boundary in `docs/AUDIT_LEDGER.md` section 6.8.

### Added

- `bench/` with three reproducible benchmarks and their measured results
  (`bench/RESULTS.md`), so that statements about performance are measurements
  rather than guesses: alloc latency against live count, `validate` / `get_stats`
  scaling, and a fragmentation A/B (compaction on vs off, same allocator).
- `bench/bench_timer.h`, the shared measurement instrument. It batches, it
  self-reports, and it refuses to emit per-operation timings the host cannot
  support. On the device it switches to `esp_cpu_get_cycle_count()` (mcycle),
  which is a free register read — per-operation timing and percentiles are
  available there and are not available here. It deliberately avoids
  `__uint128_t`, which the Xtensa toolchain does not have.
- `bench/esp32/`, an ESP-IDF project that builds **the same** benchmark sources
  for the ESP32-S3, separate from the acceptance firmware so that measuring never
  disturbs the acceptance build. **Verified to configure, compile and link**
  (elf 3.7 MB, DIRAM 204,361 / 341,760 B = 59.8%). It has **not** been run on
  hardware: the board was disconnected when the attempt was made (kernel log
  `usb 1-2.1: USB disconnect`; `lsusb` shows no Espressif device). No device
  numbers exist yet, and none are claimed.
- `tests/consumer_smoke.sh`, an end-to-end proof of the install + `find_package`
  consumption path.
- `tests/run_host.sh --coverage`: line and branch coverage of the core over the
  acceptance suite, with an enforced floor. Measured at this commit: **96.47% of
  1302 lines, 98.75% of branches executed, 77.09% of branches taken**.
- `tests/run_host.sh --fuzz` plus `fuzz/fuzz_pm.cpp`: a structure-aware libFuzzer
  target that replays byte-derived operation sequences against the public API
  under ASan + UBSan, looking for the crashes, hangs and UB that a curated suite
  cannot reasonably reach. Run with `PM_DEBUG=0` so that a caller bug (which
  Debug aborts on by design) is not mistaken for a crash. Bounded runs of 60 s
  executed 31-61 million inputs with no crash and no sanitizer finding. Its
  limitation is recorded in the source: edge coverage plateaus quickly
  (~400-450 edges), so its value is the volume of random sequences rather than
  deep exploration.

## [1.0.0] - 2026-09-12

First released version. The library is feature-complete for its stated scope and
has passed its full acceptance gates on both host and a real ESP32-S3.

### Added

- **Managed memory over a fixed Auto Zone.** The zone is divided into segments
  (default 4 KiB) and combined into pools; all pool and object metadata lives in
  static storage and never moves.
- **TLSF-style segregated-fit allocator** with two-level bitmaps, first-fit
  inside a bin, immediate coalescing with physical neighbours, an 8-byte block
  header (own size + free bit, predecessor size), a 16-byte minimum block, and
  8-byte alignment throughout.
- **Stable logical references.** `pm_local_ptr<T>` is bound to the pool it was
  created in and reports `PoolChanged` if the object moves elsewhere;
  `pm_cross_ptr<T>` follows the object across pools. Binding is enforced at
  *resolution* time, so a forged pool hint is refused on every access rather
  than being trusted at construction.
- **RAII physical-pointer borrows.** `pm_access<T>` / `operator->` hold a
  pool-level and object-level borrow, so a pool can never be relocated while a
  raw pointer derived from it is live. `resolve()` / `peek()` are documented
  advanced interfaces that do *not* take a borrow.
- **Explicit, caller-triggered maintenance.** `compact`, `merge` and `split`
  share one four-phase transaction: locked arming, read-only audit and planning,
  an execution phase that cannot fail, and a single locked commit. A planning
  failure restores the entry state with zero bytes changed.
- **Relocatability as an explicit opt-in.** `pm_is_relocatable<T>` defaults to
  false and is deliberately independent of `std::is_trivially_copyable`:
  "relocatable" is a project contract about byte-wise `memmove` preserving
  semantics, which no type trait can decide.
- **Read-only compaction advice.** `analyze_compaction` answers "is compaction
  worth trying now" without moving anything, with a five-value verdict plus
  `INVALID_REQUEST` to keep malformed caller input distinct from metadata
  damage. `poll_compaction_advice` adds repeat-prompt suppression.
- **Host and on-target acceptance suite.** Base groups 1-13, regression groups
  R1-R35, a reference-model differential with an independent oracle, a
  dual-core SMP borrow/pause/compact test (device only), a TLSF configuration
  matrix, and protocol/HTTP smoke tests for the demo layer.
- **Build integration.** Dual-form `CMakeLists.txt` (plain CMake library with
  `find_package` support, or ESP-IDF component from the same root),
  `idf_component.yml`, and `tests/consumer_smoke.sh` to prove the install and
  `find_package` path end to end.

### Verification at release

| Gate | Result |
|---|---|
| Host Debug, 10000 ops | 5,409,618 checks, 0 failures |
| Host Release, 10000 ops | 5,409,625 checks, 0 failures |
| ASan + UBSan, 3000 ops | 1,508,629 checks, 0 failures |
| Reference-model differential | 466,859 checks, 0 failures |
| cppcheck (warning/style/performance) | exit 0, 0/0/0 |
| TLSF configuration matrix | PASSED |
| Demo protocol / HTTP smoke | 95 / 15 checks, 0 failures |
| ESP32-S3 (n16r8), 2000 ops | suite 1,120,456 + dual-core 28 + model 466,859 checks, 0 failures |

### Known boundaries

These are deliberate decisions, not omissions — see `docs/AUDIT_LEDGER.md` section 6.

- Plain `T*` pointers do not follow relocation; DMA, ISR and external holders
  cannot be discovered by the library and must be drained by the caller before
  maintenance. The quiescence flag in the advice output is a reminder of that
  obligation, never a certificate from the library.
- `alloc` is **not** O(1): the TLSF bitmap locates a bin, and the descriptor is
  linked into an address-ordered list. The honest bound is
  O(bin chain + live objects).
- `validate` is O((live + free)^2).
- `PM_LOCK` compiles to nothing on host, so host runs prove none of the lock
  semantics; SMP evidence comes only from the dual-core device test.
- No ESP32 bidirectional command channel: the on-target demo is a fixed script,
  display-only.
- Concurrently consistent snapshots and true SMP-safe reads/writes are out of
  scope for 1.x.

[1.0.0]: https://github.com/qwp-look/PondMerge/releases/tag/v1.0.0
