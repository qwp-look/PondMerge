# PondMerge v1

面向无 MMU 的 32 位 MCU（C++17 子集，无异常 / 无 RTTI / 无动态分配 / 不依赖 OS 线程）的
用户态托管内存系统。固定 Auto Zone 上实现：TLSF 风格分离适配分配器、稳定对象编号
（`pm_ptr` 逻辑引用）、RAII 物理指针借用、显式触发的池内压缩 / 池合并 / 池拆分。

实现依据《PondMerge v1 代码指导书》与《PondMerge v1 架构说明》。

## 目录结构

```
include/pondmerge/
    pondmerge.hpp   公共 API（Status / RawRef / pm_ptr / pm_access / pm_make ...）
    pm_config.h     编译期参数 + 编译期契约 static_assert
    pm_port.h       平台层：临界区 + 微秒计时（host / PM_ESP32）
src/
    internal.h      内部结构（Pool / ObjectDesc / TlsfBins / FreeBlock）
    core.cpp        核心实现（单一翻译单元）
tests/
    suite.cpp       验收测试套件（与平台无关，host 与 ESP32 共用）
    main.cpp        主机 runner
    run_host.sh     构建 + 运行（--release / --san / --cppcheck）
esp32/             ESP32-S3 (n16r8) IDF 工程（v6.0.2，组件强制 gnu++17）
docs/              设计文档与修复任务书
```

## 内存布局（文档 §2）

- **Chaos Zone** / **Free Zone**：PondMerge 不触碰，由调用方自行规划。
- **Auto Zone**：由 `pm::init(Config)` 注册的一段固定内存（建议链接脚本提供），
  内部按 `segment_size`（默认 4 KiB）划分给各个 Pool。
- **Metadata**：所有池/对象元数据位于静态存储（`internal::g()` + 压缩计划 scratch），
  永不放入 Auto Zone，也永不参与搬迁。

块格式：每块 8 字节头（`[0..4)` 自身大小 | 空闲位，`[4..8)` 前块大小），
最小块 16 字节，对齐 8 字节，负载 8 字节对齐。

## 关键语义（文档 §1 + 修复任务书）

1. 只有放入 Auto Zone 的对象被管理；整理 / 合并 / 拆分全部由调用方显式触发。
2. 普通物理指针只能在借用期间使用；借用期间相关池不可搬迁（`pm_access` RAII，
   池级 `borrow_count` + 对象级 `active_borrows` 双计数）。
3. `pm_ptr<T>` 是逻辑引用（对象槽位 + generation + pool_hint + offset），搬迁后
   下次使用时经描述符惰性重解析。**没有地址缓存**：每次借用做完整 O(1) 校验
   （范围 / generation / 状态 / 池）加一次描述符读取，缓存无收益——这是正式
   性能模型（修复任务书 §5）。
4. `pm_local_ptr`（默认）绑定创建时的池，对象换池后解析返回 `PoolChanged`；
   `pm_cross_ptr` 必须通过 `pm_as_cross()` / `pm_cross_ref()` 显式生成。从
   `CROSS_HINT` 原始引用构造 local 指针会被构造函数无效化（不可绕过，R4 测试）。
5. 对象生命周期 generation 只在释放并复用槽位时递增（跳过 0），防 ABA；
   `address_epoch` 在搬迁 / 换池时递增（跳过 0）。分配失败回滚不触碰 generation。
6. 压缩为“地址顺序稳定打包”：先只读规划（pinned 屏障、越界、对齐全部校验），
   再按地址升序 `memmove`，最后一次性重建头 / 空闲块 / TLSF bins。**采用事务
   方案 A：规划失败整体放弃（池保持 Running、零改动），执行阶段不可失败；
   `finalize_layout()` 内部为断言式检查**（修复任务书 §3.6）。
7. 合并只处理物理相邻池，源池 segment 所有权转移给目标池后统一压缩重建；
   拆分把 segment 对齐边界作为虚拟 pinned 屏障，两侧独立打包、事务式提交，
   规划失败时恢复 Running 且零改动。
8. `PM_DMA` / `PM_EXTERNAL` 自动升级为 `PM_PINNED`，永不搬迁。
9. 所有失败路径返回明确 `Status`（`Busy` / `NoSpace` / `InvalidRef` /
   `PoolChanged` / `PinnedConflict` / `CorruptMetadata` ...），绝不强行搬迁。
   只有根引用（offset == 0）可以释放对象；`pm_ptr::at()` 子对象视图的
   free/destroy 一律拒绝（`pm_destroy` 失败时不清空指针，`PoolChanged` 表示
   对象仍存活、只是 local 绑定失效）。

### 并发契约：单所有者 + 静默维护期（修复任务书 §4）

- 普通 alloc / 访问 / free 由**一个所有者上下文**执行，不承诺多线程并发安全。
- `borrow_begin/borrow_end` 与池状态翻转（`pause`/`resume`/`compact` 入口）内部
  同步，暂停不会与新的借用产生检查-翻转竞态。
- 维护（compact/merge/split）前：`pause` → 等待借用计数归零 → 操作 → `resume`。
  DMA、ISR、其他线程持有的物理指针必须由调用方先行停止——PondMerge 不发现
  外部持有者。
- 构造函数与销毁回调不得对“正在构造/销毁的对象”重入分配器；对其他对象的
  重入允许但顺序敏感。

### 可搬移类型是显式 opt-in（修复任务书 §3.5）

`pm_is_relocatable<T>` 默认为 `false`，与 `std::is_trivially_copyable` 无关：
它是项目定义的“可整体 `memmove` 且语义不变”契约。含 Auto Zone 裸指针、DMA /
寄存器地址、自引用、外部所有权的类型默认禁止搬移，必须显式特化注册后才能
`pm_make`。`set_destroy_fn()` 只提供销毁，不会让类型变得可搬移。绕过 trait 的
低层 `alloc(PM_MOVABLE)` 由调用方自行承担审计责任。

## 公共 API 速览

```cpp
pm::init(cfg);  pm::deinit();
pm::create_pool(id, segments);  pm::destroy_pool(id);
pm::pause(id);  pm::resume(id);  pm::compact(id);
pm::merge(source, target);      pm::split(source, new_segments, out);

pm::alloc(pool, size, align, flags, tag, ref);  pm::free(ref);
pm::borrow_begin(ref, size, align, ptr);        pm::borrow_end(ref);

auto pod = pm::pm_make<Pod>(pool);                     // 可搬迁（需 trivially relocatable）
auto pin = pm::pm_make_pinned<Widget>(pool, args...);  // 非平凡类型，带析构
auto buf = pm::pm_alloc_buffer(pool, n, pm::PM_ZERO_INIT);
auto acc = pod.try_borrow();                // Result<pm_access<T>>
pod->field = x;                             // operator-> 持有表达式级借用
auto cross = pm::pm_as_cross(pod);
pm::pm_destroy(pod);

pm::get_stats(pool);  pm::validate(pool);   // Debug 完整性检查
```

## 主机测试

```sh
tests/run_host.sh              # Host Debug（默认，-O1 -g，PM_DEBUG=1）
tests/run_host.sh 10000        # 指定压力次数
tests/run_host.sh --release    # Host Release（-O3 -DNDEBUG -DPM_DEBUG=0）
tests/run_host.sh --san        # ASan + UBSan
tests/run_host.sh --cppcheck   # cppcheck 静态检查
```

基础测试组与《指导书》§18 对应：[1] 连续分配释放复用 · [2] 随机 10000 次 + 整理 ·
[3] 整理后逻辑引用不变 · [4] 活跃借用使整理返回 Busy · [5] generation / double
free / 越界捕获 · [6] pinned 屏障 · [7] DMA / 外部对象不搬迁 · [8] 合并后引用可
解析 · [9] 拆分后跨界引用可解析 · [10] Zone / 池 / segment 耗尽 · [11] 大小与
对齐边界 · [12] `pm_validate()` 与损坏注入 · [13] 类型化 C++ API（`pm_make` /
`pm_make_pinned` / `pm_as_cross` / 偏移子对象视图）。

修复回归组（`docs/PondMerge_v1_repair_task.md` §8）：R1 失败分配槽位回滚 ·
R2 TLSF 同 bin first-fit · R3 `pm_destroy` 失败保留指针 · R4 CROSS_HINT 构造
local 拒绝 · R5 可搬移 opt-in（编译期 + 运行期） · R6 子对象不可释放 ·
R7 维护失败路径零改动 · R8 validate 环/损坏有限时返回 · R9 整数上限拒绝 ·
R10 静默期契约 · R11 拆分布局（pinned 不动 + 两侧重排） · R12
generation/epoch/hint 组合矩阵。

验收结果（全部 0 failures）：Host Debug `5,271,298` 项检查 · Host Release 同
套件 · ASan/UBSan 干净 · cppcheck 0 告警。

## ESP32-S3（n16r8）上机

```sh
source ~/esp/activate-idf.sh        # ESP-IDF v6.0.2
cd esp32
idf.py -B build set-target esp32s3  # 首次
idf.py -B build build
idf.py -B build -p /dev/ttyACM0 flash monitor
```

设备端跑同一套验收套件（压力 2000 次，Auto Zone 256 KiB @ 内部 SRAM，
`PM_MAX_OBJECTS=256` 以收紧元数据）。实测输出：

```
=== PondMerge v1 ESP32-S3 acceptance suite ===
chip: model=9 rev=0.2 cores=2
[1]..[13] 全部 PASS
830,988 checks, 0 failures
=== suite PASSED (rc=0) ===
```

压力统计（设备）：`max_compact_us=1176`，元数据 27 KB。

## Debug / Release

`PM_DEBUG=1`（默认）启用 `PM_ASSERT` 与激进校验；`PM_DEBUG=0` 时仍保留
generation、状态与边界检查（文档 §15）。Release 构建可关闭 `pm_validate()` 的
全量校验调用。

## v1 非目标（文档 §20）

不修复任意裸指针、不在 ISR / 后台自动整理、不自动判断 DMA 完成、不自动分析对象
内部指针、不扩大 Auto Zone、不提供硬件隔离、不保证压缩硬实时。
