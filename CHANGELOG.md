# Changelog

All notable changes to PondMerge are documented here.
This project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- **`alloc` is no longer linear in the live-object count.** It used to insert
  each new descriptor into an address-ordered list, and that insertion measured
  as essentially 100% of alloc's cost, growing linearly with the live count
  (232 ns at 256 objects, 799 ns at 1024). Address order is needed only by the
  maintenance paths, so it is now re-established once per maintenance call on
  the cold path: alloc does an O(1) append, and `collect_live_sorted()` performs
  a bounded collection plus an in-place heapsort. Measured after the change:
  alloc is ~86-88 ns during fill and **37.6 ns steady state at 1024 objects**,
  with **no dependence on the live count** (see `bench/RESULTS.md`). `free` and
  `validate` show no regression, and the fragmentation benchmark's A/B results
  are byte-identical, so only the cost moved, not the behaviour.

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
- `tests/consumer_smoke.sh`, an end-to-end proof of the install + `find_package`
  consumption path.

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
