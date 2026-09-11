# PondMerge v2 修复轮交接文档

日期：2026-09-06（收口时状态）
基线提交：`c330db8`（第一轮修复完成时）
收口提交：见 git log（本文档同提交入库）
依据文档：`docs/PondMerge_v1_repair_task.md`（第一轮）、第二轮修复指南（v2，未入库，见 §6）

> **续轮说明（2026-09-11）**：本文档 §2 的未完成项已在 v2 续轮处理完毕，v2 任务书
> 也已入库为 `docs/PondMerge_v2_repair_task.md`。续轮的完整报告、调用方可见的语义
> 变更、以及本轮新修出的 4 个缺陷见 `docs/HANDOVER_v3.md`。以下 §2 保留为收口当时
> 的历史快照。

## 1. 本轮（v2）已完成并验证的修复

以下各项均已实施，且在 Host Debug / Release（-O3 -DNDEBUG -DPM_DEBUG=0）下
全绿（1,006,606 checks, 0 failures，2000 op 压力档）：

| # | 任务书条目 | 修复位置 | 回归测试 |
|---|---|---|---|
| 1 | §4 split 搬移覆盖 | `src/core.cpp` `split()` 执行段：upper 计划改为**向右组（地址序前缀）按源降序 + 向左组按源升序**两趟执行。证明要点：`dst_i - src_i = x - Σholes(前缀)`，向右条目构成前缀；组内降序/升序各自不会触及未搬源；组间写区间被连续打包隔离 | **R13** `test_split_crossing_order`：布局 `[L 16368][C 跨界][hole 24B][u1][u2]`。旧降序执行下 u2 的写区间覆盖 u1 源尾部 16 字节（红测实测 byte 3568 损坏），新顺序全绿。注意：v2 任务书 §4.2 伪代码的"upper 全部升序"在其自身前提下不成立（crossing 主体在边界下方时其后第一个对象向右移动），实际实现为上述两趟方案，证明写在执行段注释里 |
| 2 | §5 merge/split 状态与锁 | `merge()`/`split()`：装备段（有效性/邻接/状态/借用检查 + 置 Merging/Splitting）在 `PM_LOCK` 内；规划失败在锁内恢复 `Running`；最终提交（source 清空 / 双池 Running）在锁内。锁内不做 memmove | 既有 R7/R10/R11/R12 全部覆盖错误路径与状态恢复 |
| 3 | §6 init 生命周期 | `init()`：所有配置校验（含 zone_size 溢出检查）先于任何全局状态修改；已初始化时返回 `Busy`，绝不静默清空 | **R14** `test_init_lifecycle`：未初始化时 5 种非法配置全部拒绝且不留半初始化；live 对象时重复 init → Busy 且对象可访问；deinit→init 后 generation 重置为 1 |
| 4 | §7.1 SL 位图宽度 | `pm_config.h`：`PM_SL_COUNT` 契约收窄为 [2,16]（sl_bitmap 是 uint16_t）；`PM_FL_MAX ≤ 31`、`PM_MAX_SEGMENTS ≤ 0xFFFF` 新增 static_assert | **配置矩阵** `tests/config_matrix.sh`（`run_host.sh` 未接入，见 §3 未完成项）：SL=2/4/8/16 与 FL_MAX=31 编译+smoke 通过；SL=32、FL_MAX=32 编译期拒绝（预期失败实测） |
| 5 | §8 维护前完整预检 | `src/core.cpp` `precheck_pool()`：order 链步数受限防环、descriptor 全字段（state/pool/generation/prev 链/block_size≥MIN/对齐/size≤block-HEADER）、zone offset 整数化范围检查、源块不重叠、物理块头与描述符一致、统计核对。`compact_impl` 与 `split` 的 memmove 前强制执行（Release 也运行）。`get_stats()` 的 bins 遍历加步数上限，损坏时返回 largest=0 而非死循环 | 现有全套测试回归通过；R8 的损坏注入仍有限时返回 |
| 6 | §10 set_destroy_fn 语义 | `set_destroy_fn()` 仅接受 `PM_PINNED` 对象，movable 对象返回 `NotRelocatable` | R6 追加断言：movable 对象 set_destroy_fn → NotRelocatable；pinned 路径（pm_make_pinned）不变 |

## 2. 收口时明确的未完成项（按优先级）

1. **validate() 整数化与块内边界检查（v2 §9.1）——未实施。**
   `validate()` 的 live 块循环仍使用裸指针运算（`d.address - BLOCK_HEADER_SIZE`
   等），损坏元数据下存在理论 UB；且未包含 `block_size ≥ PM_MIN_BLOCK`、
   `block_size 对齐`、`size ≤ block_size - HEADER` 三项关系（precheck_pool
   已有现成写法可搬）。建议：把 `precheck_pool()` 的整数偏移模式套入
   validate 的 live 循环即可，约 30 行改动。

2. **复杂度声明更新（v2 §7.3）——未实施。** README 仍写 alloc O(1)。
   实际：alloc O(bin 内链长，上界 O(zone_size/PM_MIN_BLOCK))；free O(1)；
   validate O(live × free)；get_stats O(free_blocks)（已有步数上限）。
   建议按 v2 任务书表格更新 README 与头文件注释。

3. **cppcheck 48 条告警未清（v2 §12.1）。** 分类：cstyleCast×15、
   constVariablePointer×12、constVariableReference×11、
   knownConditionTrueFalse×3（两处为 uint16 回绕防御，cppcheck 误报，需
   行内抑制+理由；一处为测试框架误报）、constParameterPointer×3、
   unreadVariable×2（fill() 的写模式与 R4 的 mutated，需抑制+理由）、
   nullPointerRedundantCheck×1（pm_access::operator-> 的 PM_ASSERT 展开，
   需抑制+理由）、constParameterReference×1。无 error 级。复现：
   `tests/run_host.sh --cppcheck`（退出码 2）。

4. **模型测试（v2 §11.2）——未实施。** 建议：独立参考模型（有序 live/free
   intervals）+ 固定 seed 随机 alloc/free/compact/split/merge 对比，失败打印
   seed 与操作序列。

5. **v2 定向测试缺口（v2 §11.1）**：已完成 R13（条目 1）、R14（条目 5）。
   未实施：条目 2（跨界对象位于边界/贴近池尾的边界情形）、条目 3/4（Paused
   状态调 merge/split 的返回值矩阵——实现已统一为要求 Running，测试未固化）、
   条目 8（size > block_size-HEADER 时 resolve 拒绝——注意：borrow/resolve
   路径**尚未**加该项检查，只在 precheck/validate；如需 resolve 层拒绝要改
   check_ref）、条目 9-12 部分覆盖（R8/R6/R12 相关）。

6. **ESP32 未重烧。** 本轮全部 host 验证通过，但设备固件仍是第一轮版本。
   重烧后必须核对启动日志 `App version` 与提交号一致（v2 §3.1 的可信度要求）。
   注意：R1 的批量分配已改为 128 一批（设备 BSS 紧张）；R8 结尾的白盒 power
   cycle（memset 全局）在设备上同样适用。

7. **本轮代码未提交前的完整长压验收**（10000 op × Debug/Release/san）在收口
   时只跑了 2000 op 档；提交前建议补跑：
   `tests/run_host.sh && tests/run_host.sh --release && tests/run_host.sh --san 3000`
   以及 `tests/config_matrix.sh`。

## 3. 已知设计边界（不属缺陷，但调用者必须知道）

- **并发契约**：单所有者 + 静默维护期。merge/split 自身完成状态转换并要求池
  处于 Running、borrow 归零；外部 DMA/ISR/线程持有者由调用方停止。锁只保证
  状态翻转与 borrow 计数的原子性，不使 alloc/free/resolve 多线程安全。
- **事务模型 A**：规划（含 precheck_pool）失败 → 池恢复 Running、零改动；
  执行阶段不可失败的置信度来自 precheck 的全量审计。若未来放宽 precheck，
  方案 A 即失效。
- **precheck 有意不查 prev_size 链**：merge 的中间态（两池边界处）链未重写，
  由 compact 的 finalize 统一重建；validate() 在稳定布局上强制链一致。
- **split 向右组来源**：crossing 对象主体在边界下方时（x>0），其后的普通
  对象可能向右移动至多 x 字节——两趟执行顺序的正确性证明写在
  `split()` 执行段注释，修改搬移逻辑前必须先读它。

## 4. 验收状态快照（收口时）

| 命令 | 结果 |
|---|---|
| `./build/pm_dbg 2000`（Debug 全套 25 组） | 1,006,606 checks, 0 failures |
| `tests/run_host.sh --release 2000` | 1,006,606 checks, 0 failures |
| `tests/config_matrix.sh` | SL 2/4/8/16 + FL 31 通过；SL 32 / FL 32 编译期拒绝 |
| `tests/run_host.sh --cppcheck` | 48 条低级告警（见 §2.3），退出码 2 |
| ASan/UBSan | 本轮改动后未复跑（改动前基线干净） |
| ESP32-S3 | 固件为第一轮版本，本轮未重烧 |

测试组清单：13 组基础（指导书 §18 + typed API）+ R1–R14 + 配置矩阵。

## 5. 关键文件导览

- `src/core.cpp`：split 执行段（搬移顺序证明注释）、precheck_pool、
  init()、merge/split 锁边界、set_destroy_fn
- `include/pondmerge/pm_config.h`：编译期契约 static_assert 群
- `tests/suite.cpp`：R13/R14 在文件尾部；R8 结尾的白盒 power cycle 有注释
- `tests/config_matrix.sh` + `tests/config_smoke.cpp`：配置矩阵
- `docs/PondMerge_v1_repair_task.md`：第一轮任务书与实施对照

## 6. 待办：把 v2 任务书入库

本轮依据的"第二轮修复指南"（含 §4.2 伪代码与 §15 报告模板）尚未入
`docs/`。接手者如持有原文，建议存为 `docs/PondMerge_v2_repair_task.md` 并在
本文档 §2 的未完成项上对照推进。注意其 §4.2 伪代码存在本文档 §1 条目 1
所述的前提缺口，实施时以 `split()` 注释中的两趟方案为准。
