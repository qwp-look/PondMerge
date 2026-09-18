# PondMerge v1

A **user-space managed-memory system for 32-bit MCUs without an MMU**. On a fixed
"Auto Zone" it provides a TLSF-style segregated-fit allocator, stable logical
references (`pm_ptr`), RAII borrows for physical pointers, caller-triggered
pool compaction / merge / split, and a read-only compaction-advice analysis.

C++17 subset: **no exceptions, no RTTI, no dynamic allocation, no dependency on
OS threads.** The core library has no external dependencies.

> **This page is an overview, written to be read once.** The authoritative
> documentation is the Chinese set under [`docs/`](docs/), which carries the
> invariant ledger, the full API contract and the per-round audit reports. This
> file deliberately does not duplicate it, so the two cannot drift apart.

---

## The design stance

Most allocators on an MCU fail *late and silently*: the pool fragments, and one
day a large allocation returns NULL with no explanation. PondMerge takes the
opposite position — **the library only does what it can prove, and everything it
cannot prove becomes an explicit contract on the caller.**

The practical consequence is a trade that is unusual and worth stating plainly:

| The library promises | The library will NOT do |
|---|---|
| A physical address stays valid for the whole of a borrow, proven by dual reference counting (pool-level `borrow_count` + object-level `active_borrows`) | Discover a `T*` you stashed in a struct and silently fix it after a move |
| Relocation happens only inside a caller-triggered window, and only after a read-only plan that has verified everything | Compact in the background, from an ISR, or on its own initiative |
| A maintenance transaction that fails its plan leaves the pool **byte-for-byte unchanged** | Guess whether your DMA transfer has finished |
| `pinned` / `DMA` / `external` objects never move | Provide hardware isolation |

If you need the library to find pointers hidden inside your objects, this is the
wrong library. That is a documented non-goal, not a gap.

## The memory model

- **Chaos Zone / Free Zone** — PondMerge does not touch these. Yours to plan.
- **Auto Zone** — one fixed region registered by `pm::init(Config)` (a linker
  section is a natural source), subdivided into pools by `segment_size`
  (default 4 KiB).
- **Metadata** — all pool and object metadata lives in static storage
  (`internal::g()` plus maintenance-plan scratch). It is never placed in the Auto
  Zone and is never relocated.

Block format: an 8-byte header per block (`[0..4)` own size | free bit,
`[4..8)` previous block size), minimum block 16 bytes, 8-byte alignment,
8-byte-aligned payloads.

### Static RAM budget — the first thing to compute

Metadata is static, so it does not consume the Auto Zone, but it is also **never
freed**. `PM_MAX_OBJECTS` dominates. Measured over six configurations:

```
metadata_bytes = 72 + 476 * PM_MAX_POOLS + 98 * PM_MAX_OBJECTS   (Release)
                 where 98 = 64 (ObjectDesc) + 34 (maintenance scratch)
                 Debug adds 9 bytes (advice owner gate: context id + flags)
```

| `PM_MAX_OBJECTS` | `PM_MAX_POOLS` | `metadata_bytes` | real `.bss` | suits |
|---|---|---|---|---|
| 64 | 2 | 7,296 | 7,296 | very small targets |
| 128 | 4 | 14,520 | 14,528 | small MCU |
| 256 | 4 | 27,064 | 27,072 | small MCU (recommended start) |
| **256** | **16** | **32,776** | **32,768** | **the ESP32-S3 acceptance firmware** |
| 512 | 8 | 54,056 | 54,048 | medium |
| 1024 | 16 | 108,040 | 108,032 | default (host / large-memory targets) |

**The default costs about 105 KiB of static RAM, which most MCUs cannot afford.**
Set `PM_MAX_OBJECTS=256` to bring it to roughly 31 KiB. `metadata_bytes` now
matches the linker's `.bss` to within ±8 bytes; leave a little margin anyway.
(Before v1.0.0 that figure silently omitted the advice cache, under-reporting by
128 B at 2 pools up to 960 B at 16 pools. It is included now.)

## Key semantics

1. Only objects placed in the Auto Zone are managed. Compaction, merge and split
   are all caller-triggered.
2. A physical pointer is usable **only for the duration of a borrow**, and the
   pool cannot be relocated while borrowed (`pm_access` RAII). `resolve` / `peek`
   are non-counting advanced-validation entry points: the raw pointer they return
   is valid only for immediate use inside a quiet period. Normal access goes
   through `try_borrow()` / `pm_access` / `pm_ptr::operator->`.
3. `pm_ptr<T>` is a **logical** reference (object slot + generation + pool hint +
   offset), lazily re-resolved through the descriptor after a move. There is
   **no address cache**.
4. `pm_local_ptr` (the default) is bound to its creating pool and returns
   `PoolChanged` after the object moves pools; `pm_cross_ptr` must be produced
   explicitly. Local binding is enforced **at resolve time**, so a forged pool
   hint is rejected on every borrow/resolve/free.
5. `generation` increments only when a slot is freed and reused (never 0), which
   is the ABA guard. `address_epoch` increments on relocation or pool change.
6. Compaction is address-ordered stable packing: read-only planning first
   (pinned barriers, bounds, alignment all verified), then `memmove` in
   ascending address order, then a single rebuild of headers, free blocks and
   TLSF bins. **Plan failure abandons the whole operation with zero changes; the
   execution phase cannot fail.**
7. `merge` only joins physically adjacent pools; `split` treats the segment
   boundary as a virtual pinned barrier.
8. `PM_DMA` / `PM_EXTERNAL` are upgraded to `PM_PINNED` and never move.
9. Every failure path returns an explicit `Status` and never force-moves
   anything. Only a root reference (`offset == 0`) can free an object.

## Complexity, and what it actually measures

| operation | worst case | note |
|---|---|---|
| `alloc` | **O(SL bin chain length)**, bounded by `zone_size / PM_MIN_BLOCK` | TLSF bitmap locates the bin, first-fit within it, then an **O(1)** append to the live-slot table. Not strictly O(1) — but **independent of the live-object count**. |
| `free` | O(1 + neighbour free-bin chain length) | Read-only proof before merging; `CorruptMetadata` with zero side effects on damage. |
| `pause` / `resume` | O(1) | |
| `compact` / `split` | O(objects + moved bytes), plus O(objects log objects) to restore address order | |
| `merge` | O(objects + free_blocks + moved bytes) | Audits both pools read-only; plan failure leaves both byte-identical. |
| `validate` | O(live_objects × free_blocks) | Diagnostic. Quadratic; see below. |
| `get_stats` | O(free_blocks) | Measured at 2.7 µs with 1536 blocks — **not worth optimising**. |
| `borrow_begin` / `resolve` / `borrow_end` | O(1) | |
| `analyze_compaction` | O(objects log objects + free_blocks) | |

### `alloc` used to be linear in the live count, and no longer is

`alloc` previously inserted each descriptor into an address-ordered list, and
measurement showed that insertion was essentially **100% of alloc's cost**,
growing linearly with the live count. Address order is needed only by the
maintenance paths, so it is now established once per maintenance call on the cold
path (a bounded collection plus an in-place heapsort), and `alloc` does an O(1)
append.

A/B with the **same benchmark source** compiled against both core revisions, so
the only difference is the core:

| live objects | before | after |
|---|---|---|
| 16 | 58.0 ns | 38.0 ns |
| 64 | 83.5 ns | 35.8 ns |
| 256 | 239.2 ns | 36.2 ns |
| 1024 | 844.2 ns | **38.4 ns** (22×) |

Before: cost grows with the live count (×14.6 over a 64× range). After: **flat,
×1.01** within a ±4% run-to-run spread.

The trade, recorded in the invariant ledger: the live-slot list is now an
**unordered bag** rather than an always-sorted table, and **`alloc` no longer
detects damage to it** (it no longer traverses it). That detection moved to every
maintenance entry and to `validate()`, still in bounded time and still with zero
side effects. The public API and the set of status codes are unchanged.

## Does compaction actually help? (the core claim, measured)

Same allocator, same workload, one difference: whether compaction is triggered.
256 KiB region, steep size mix, every 16th object pinned (a relocation barrier —
which is what a DMA buffer is), every 4th movable object released to scatter
holes, then demand 12,288 contiguous bytes.

|  | fragmented state, no churn | under sustained churn |
|---|---|---|
| compaction **disabled** | **50 of 50 demands FAILED** | 3 of 500 failed |
| compaction **on failure** | **1 of 50 failed** | 0 of 500 failed |

Cost: one compaction, moving 189 objects / 184,296 bytes, **~53 µs**.

Two things this does *not* say, and both are in the results file: the gentler
phase-2 numbers are real (first-fit drift lets an uncompacted pool partly heal
itself, so "an uncompacted pool always fails" would be a false claim), and the
absolute per-operation timings need the device — a single clock read on the
measurement host has been observed at 2.7 ms. Full method, fairness rules and
negative results: [`bench/RESULTS.md`](bench/RESULTS.md).

**Negative results kept on purpose:** raising `PM_SL_COUNT` does nothing
(40.7 / 37.8 / 40.8 ns for 4 / 8 / 16, at rising metadata cost), and `get_stats`
costs 2.7 µs at its worst — both excluded from ever being "optimised".

## Integration

Four consumption paths, **one source of truth, nothing copied** — the root
`CMakeLists.txt` is a dual-form entry point (branching on `ESP_PLATFORM` into an
IDF component registration or a plain CMake library).

**1. ESP-IDF component** (the MCU path)

```sh
cd <your-project>/components
git clone https://github.com/qwp-look/PondMerge.git pondmerge
```

> **The directory must be lowercase `pondmerge`.** IDF takes the component name
> from the directory name and the repository is `PondMerge`, so cloning under its
> own name makes `REQUIRES pondmerge` fail with `unknown name` — measured, not
> assumed.

```cmake
idf_component_register(SRCS "main.cpp" REQUIRES pondmerge)
```

```cmake
set(PM_MAX_OBJECTS 256)   # ~105 KiB -> ~31 KiB of static RAM
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(your_app)
```

Verified to build on **ESP-IDF v6.0.2 for both `esp32s3` and `esp32`**, with the
`PM_MAX_OBJECTS` override confirmed to reach the compiler. Component names come
from the **directory** name, so a clone must live in a lower-case `pondmerge`
directory or `REQUIRES pondmerge` reports an unknown name.

**2. CMake subdirectory**

```cmake
add_subdirectory(external/PondMerge)
target_link_libraries(your_app PRIVATE pondmerge::pondmerge)
```

**3. Install, then `find_package`**

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build && cmake --install build
```

```cmake
find_package(pondmerge 1.0 REQUIRED)
target_link_libraries(your_app PRIVATE pondmerge::pondmerge)
```

Paths 2 and 3 are verified end-to-end (configure → build → install →
`find_package` → link → run) by `tests/consumer_smoke.sh`, which CI runs.

**4. Manual compilation** — `g++ -std=c++17 -Iinclude your_app.cpp src/core.cpp`

> **Not published to the Espressif Component Registry.** `idf_component.yml` is in
> place, but publishing requires the repository owner's Espressif account and
> token (`compote component upload`), so it is deliberately outside automation.

## API at a glance

```cpp
pm::init(cfg);  pm::deinit();
pm::create_pool(id, segments);  pm::destroy_pool(id);
pm::pause(id);  pm::resume(id);  pm::compact(id);
pm::merge(source, target);      pm::split(source, new_segments, out);

pm::alloc(pool, size, align, flags, tag, ref);  pm::free(ref);
pm::borrow_begin(ref, size, align, ptr);        pm::borrow_end(ref);

auto pod  = pm::pm_make<Pod>(pool);                    // movable (needs opt-in trait)
auto pin  = pm::pm_make_pinned<Widget>(pool, args...); // non-trivial, with destructor
auto buf  = pm::pm_alloc_buffer(pool, n, pm::PM_ZERO_INIT);
auto acc  = pod.try_borrow();  pod->field = x;         // RAII borrow for the expression
auto cross = pm::pm_as_cross(pod);
pm::pm_destroy(pod);

pm::get_stats(pool);  pm::validate(pool);

pm::CompactionRequest req{2000, 8, 0, 0};
pm::CompactionAdvice a = pm::analyze_compaction(pool, &req);
if (a.verdict == pm::CompactionVerdict::COMPACT_RECOMMENDED) { /* open a quiet window */ }
pm::set_compaction_thresholds({100, 512});
```

Movability is an **explicit opt-in**: `pm_is_relocatable<T>` defaults to `false`
and is unrelated to `std::is_trivially_copyable`. Types holding Auto Zone raw
pointers, DMA or register addresses, self-references or external ownership are
forbidden from moving until explicitly specialised.

## Concurrency: single owner + a quiet maintenance window

| operation | concurrency promise |
|---|---|
| `alloc` / `free` / `resolve` / `get_stats` / `validate` | **single owner context**; no multi-thread guarantee |
| `borrow_begin` / `borrow_end`, `pause` / `resume` | internal lock + token protection |
| `compact` / `merge` / `split` | single owner, serialised across pools too; lock held at entry and at the final commit |
| advice (`analyze` / `poll` / threshold accessors) | owner-context API (Debug builds assert on a cross-context call) |
| DMA / ISR / other tasks / external libraries | **the caller** must stop and drain them first — the library cannot discover external holders |

**`PM_LOCK` is compiled to a no-op in host builds.** Host runs therefore prove
nothing about lock semantics; SMP evidence comes only from the dual-core device
test `tests/concurrency_esp32.cpp`, which is never compiled on the host.

## Verification status

| gate | result |
|---|---|
| Host Debug, 10000 ops | 5,409,619 checks, 0 failures |
| Host Release, 10000 ops | 5,409,626 checks, 0 failures |
| ASan + UBSan | 1,508,630 checks, 0 failures |
| Reference-model differential (independent oracle, fixed seed) | 466,859 checks, 0 failures |
| cppcheck (warning/style/performance) | exit 0 |
| TLSF configuration matrix | PASSED |
| Demo protocol / HTTP smoke | 95 / 15 checks, 0 failures |
| Coverage of `src/core.cpp` | 96.47% of lines, 98.75% of branches executed (floor enforced) |
| libFuzzer, bounded run | no crash, no sanitizer finding |
| **ESP32-S3 (n16r8) on hardware** | suite **1,120,457** + dual-core concurrency 28 + model 466,859 checks, **all 0 failures**; two runs from reset, identical counts |
| **Classic ESP32 (D0WDQ6 v1.1) on hardware** | dual-core concurrency 28 + model 466,859 checks, 0 failures; two runs from reset, identical counts. **The suite is not run** — see below |

`tests/run_host.sh --release | --san | --cppcheck | --configs | --coverage | --fuzz`
runs the host gates. CI executes all of them on every push, plus a consumer smoke
test, the two demo protocol smoke tests, and a benchmarks job.

### Two targets, and they do not run the same groups

The two device rows above are deliberately not presented as one number, because
the acceptance suite cannot run on the second part and the firmware says so out
loud rather than reporting a pass.

The suite pins its own zone to 64 segments inside its assertions — test [10]
asserts that 16 pools x 4 segments fills the zone and test [2] creates a
32-segment pool — so the zone size is part of what those tests assert rather than
a parameter they tolerate. A classic ESP32 has ~200 KiB of static DRAM in total
and the 256 KiB zone does not fit: measured, the link fails with
`region 'dram0_0_seg' overflowed by 126,744 bytes`. That target therefore compiles
without the suite, runs the concurrency and model groups (8 KiB and 64 KiB), and
prints `=== suite SKIPPED (not run, not passed) ===` on the console. The fork is
driven by `CONFIG_IDF_TARGET` in `esp32/main/CMakeLists.txt`.

The reason for adding the second target at all is the lock claim: the host
`PM_LOCK` is a no-op, so SMP evidence can only come from the dual-core device
test — and until now that evidence existed on exactly one chip. The classic ESP32
is a **dual-core LX6** (the S3 is LX7) with a different `portMUX` implementation
and cache, and the same `tests/concurrency_esp32.cpp` passes on it. Its counters
also show the race being exercised harder than on the S3: 30 of the maintainer's
200 compactions were refused because a borrow was live, and both borrowers
observed paused windows.

One number is worth singling out: the reference-model differential reports
**466,859 checks on host, on the S3, and on the classic ESP32** — one
deterministic differential test walking the same number of judgements on three
architectures.

Device-side notes: **the host must keep reading the port** — if it stops, the
transmit queue fills and `printf` blocks, which looks like a hang after a few test
groups. The S3 console is `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (`/dev/ttyACM0`);
the classic ESP32 has no such peripheral, so its console is UART0 behind a
USB-UART bridge (`/dev/ttyUSB0`, needs the user in `dialout`).
`tests/serial_cap.py` was verified on both, uses a normal-boot reset (IO0 high, EN
pulsed) that works for both, and takes an optional stop marker because the
benchmark firmware ends on a different line than the acceptance firmware.

And `bench/esp32/` — the benchmark sources compiled for the device — now runs on
**both** parts too, from one set of sources with one set of parameters: the only
difference is the shared region, 192 KiB on the S3 against 112 KiB here, because
this part has ~200 KiB of static DRAM in total and the link failed at 128 KiB with
`dram0_0_seg overflowed by 4208 bytes`. `bench/RESULTS.md` section 6 is the second
device's record. What it adds:

- the `alloc`/`free` pair is flat in the live count on both cores (**x0.99** on
  each), and the churn decomposition reproduces (two independent estimates of the
  allocator's own cost agree to 0.6%);
- fragmentation repeats exactly: **50 of 50** large demands fail with compaction
  disabled, **1 of 50** with it enabled, on both parts;
- the independent TLSF baseline's sustained-churn failure count becomes
  **quotable** here: the S3's 0 of 199 was marked `CONFOUNDED` by a single
  re-allocation refusal, but the classic part had no refusals, so its **21 of 200**
  (10%) stands against PondMerge's 0;
- and one new result: a trivial loop is **faster** on the LX6 (25.0 ns against
  29.2 ns) while every allocator operation is slower. That argues against reading
  the ~20x device/host gap as "the target's CPU is simply slow" — and
  `bench/RESULTS.md` section 5.7 has since measured where the gap does go: the
  S3's cache-miss counters read **exactly zero** for the same churn, so the
  memory story is refuted. The target executes **1,528 instructions per
  free+alloc pair against 757 on x86-64** (callgrind) and runs them at CPI 1.41
  with 24% dependency bubbles: an instruction-count explanation, not a
  memory-latency one.


## Debug vs Release

`PM_DEBUG=1` (the default) enables `PM_ASSERT` and the advice owner gate.
`PM_DEBUG=0` compiles the assertions out, but generation, state and bounds
validation are all retained. Corrupt metadata returns `CorruptMetadata` in every
build.

Debug costs about **10%** over Release on the measured hot path (882 vs 799 ns in
the pre-Plan-A measurement), which is why a small amount of runtime checking on
the hot path is affordable.

## Non-goals for v1

Not fixed: arbitrary raw pointers; background or ISR-driven compaction; DMA
completion detection; analysis of pointers inside objects; growing the Auto Zone;
hardware isolation; hard real-time compaction guarantees.

## Where to read more

| document | contents |
|---|---|
| [QUICKSTART.md](QUICKSTART.md) | 5-minute first example → full compaction flow → common mistakes *(Chinese)* |
| [docs/USAGE_GUIDE.md](docs/USAGE_GUIDE.md) | Three-zone model, API contract tables, concurrency, error codes, complexity *(Chinese)* |
| [docs/COMPACTION_POLICY.md](docs/COMPACTION_POLICY.md) | Advice semantics, decision matrix, thresholds, the standard flow *(Chinese)* |
| [docs/AUDIT_LEDGER.md](docs/AUDIT_LEDGER.md) | The invariant ledger: invariant → code location → proof → test *(Chinese)* |
| [docs/DEMO_REQUIREMENTS.md](docs/DEMO_REQUIREMENTS.md) | Demo composition, snapshot protocol v1.1, JSON subset, acceptance *(Chinese)* |
| [bench/RESULTS.md](bench/RESULTS.md) | Every measured number, the instrument's limits, and the negative results |
| [CHANGELOG.md](CHANGELOG.md) | Release notes and known boundaries |
| docs/HANDOVER_v2–v12.md | Per-round audit reports |

## Licence

Apache-2.0. See [LICENSE](LICENSE).
