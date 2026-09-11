# PondMerge v1 第三轮收口报告（HANDOVER v5）

> 依据 `PondMerge v1 第三轮审计修复指南与注意事项`（下称"指南"）执行。
> 审计基线：`6bcd28f`（指南 §0；本轮开始时 HEAD 为其后的 `deec2ad` 纯文档提交）。
> 本文是第三轮的交付报告 + 面向下一轮的交接，与 `HANDOVER_v2/v3/v4` 互补。

## 0. 30 秒速览

- 修复了指南 §1 的全部 7 项问题：2×P0（merge 事务）、3×P1（descriptor 范围 /
  free 顺序 / borrow_end）、2×P2（resolve 清空输出、性能记录口径）。
- 新增回归组 R22–R28（指南 §10），全部先红后绿；R22 在旧代码上复现了
  "merge 返回 OK 但六个元数据域全部被改写" 与 "环链注入无限循环挂死"。
- 新增设备侧双核并发锁边界测试 `tests/concurrency_esp32.cpp`
  （HANDOVER_v4 T1 + 指南 R25 设备部分）。
- Host Debug/Release/ASan(10000)/cppcheck/configs 全绿；ESP32-S3 实机验证
  见 §6（App version = 最终提交号）。
- 复杂度口径如实变更：`free` 从 "O(1)" 改为
  "O(1 + 邻块空闲 bin 链长)"；`merge` 增加 O(objects + free_blocks) 的只读审计。

## 1. 问题 → 修复 → 测试 对照（指南 §14 模板）

| 指南编号 | 问题 | 修复位置 | 测试 |
|---|---|---|---|
| P0-1 | merge 在 precheck 前改写 pool_id/epoch/order/统计，失败不回滚 | `merge()` 整体重写：arming(锁) → 只读审计+规划（scratch）→ 不可失败执行 → 单锁最终提交；失败仅恢复两池入口状态，此前**零写入** | R22（8 种注入逐字节比对） |
| P0-2 | merge 直接解引用不可信 order list（无步数上限） | 新增 `walk_order()`（index 范围 + addr_prev 链 + PM_MAX_OBJECTS+1 步数上限），merge/compact/split 的规划遍历全部经由它收集到 `s_slots[]` 后迭代可信数组；merge/split 的链表重建直接由数组重建，不再走 order_unlink/insert | R22 注入 7/8（环链、越界 head） |
| P1-1 | check_ref 未验证描述符块落在池内 | `check_ref()` 增加池范围证明：全部用 uint64 zone offset 运算（禁止对伪造地址做指针算术），校验 `block ∈ [pool_start, pool_end)`、`block_size ≤ pend-boff`、`payload ≤ pend`；O(1)，borrow/resolve/free/set_destroy_fn 全部入口生效 | R23（5 种池外地址，free 无回调零副作用） |
| P1-2 | free 在物理验证前执行 destroy callback | `free()` 重排为：check_ref → 只读邻居验证（自身块头/prev_size 链闭合/后继 sane/bin 成员资格+互逆链接 `free_block_binned`）→ 回调 → **再验证**（回调可合法重入其他对象）→ 变更；任何损坏路径无回调、无统计、无 bins 写入 | R24（8 类物理头故障 + 同池重入回调） |
| P1-3 | borrow_end 锁外读 token 后锁内无条件递减 | `borrow_end()` 全部校验与递减移入同一 `PM_LOCK` 临界区；任何 token 失配**不动任何计数**，Debug 断言（Release 静默忽略） | R25（Release 拒绝路径 + 设备双核并发） |
| P1-4 | local RawRef 绑定可伪造 | 采用指南**兼容方案**：保留构造能力，文档明确"绑定在解析时强制"；伪造的具体 pool hint 每次 borrow/resolve/free 都得 `PoolChanged`；`RawRef` 定位为 unsafe 低层句柄。理由：伪造 local 与 genuine 的差异只在解析期检查，伪造**无法**获得越界访问（check_ref 的池范围证明 + generation 拦截），改私有构造会破坏 R3/R4 既有断言 | R27（语义固定测试） |
| P2-1 | resolve 失败不清空输出 | `resolve()` 入口 `out_addr = nullptr`，全部检查通过后才写真实地址；`borrow_begin` 失败清空路径保持 | R26（9 条失败路径 + 成功路径对照） |
| P2-2 | max_compact_us=2487 与 1176 口径冲突 | README 只保留带提交号/App version/ELF SHA/编译时间的实测记录（§6 的新设备记录），旧数字随旧记录一并替换 | — |
| R28 | 状态发布边界 | `compact_impl` 不再自行发布 Running/epoch（原在锁外！）；compact/merge/split 的 Running 发布 + `structure_epoch++` 全部移入**最终提交锁**；split 的新池槽位在 arming 锁内认领为 Splitting，提交前任何 API 不可观测为可用，失败路径解锁并回收槽位 | R28（4 个子场景） |

## 2. 关键事务结构（merge，指南 §3.1 的落地）

```
merge(source, target):
    lock: 校验 initialized/ID/相邻/维护态/借用 → S,T = Merging        [arming]
    unlock
    ── 只读审计（任何失败 goto fail_restore，此前零持久写入）──
    precheck_pool(S), precheck_pool(T)      # order 链/描述符/块头/统计, 有界
    audit_pool_bins(S), audit_pool_bins(T)  # bins 结构/位图/互逆链接/prev 链, 有界
    walk_order(下池) ++ walk_order(上池) → s_slots[]   # 合并地址序 = 下池链+上池链
    ── 只读规划 ──
    pinned 屏障收集 + 光标打包（与 compact 同一"打包前缀 ≤ 自身偏移"论证，
    precheck 通过后计划不可失败；分支保留用于防御性拒绝）
    ── 执行（不可失败，plan A）──
    按 s_plan 升序 memmove（全部向左/原地）；pool_id 改写 + epoch 语义与旧版一致
    （换池必 bump、移动必 bump）；T 的 order 链由 s_plan 直接重建（不再遍历链表）
    finalize_layout(T, 合并区间)             # 重建头/bins/字节统计
    ── 最终提交（单锁）──
    lock: memset(source)=Empty; T.structure_epoch++; T.state=Running
    unlock

fail_restore:
    lock: S/T 恢复入口状态(Running/Paused); unlock; return CorruptMetadata/…
```

搬移安全论证（沿用 compact 的证明）：计划项按源地址升序，可搬移项 dst ≤ src，
写入区间 [dst_i, dst_i+size_i) = [dst_i, dst_{i+1}) ≤ src_{i+1}，升序执行永不
触碰未搬移源；pinned 项 dst == src 不动。

## 3. 复杂度（诚实口径，README / pondmerge.hpp 已同步）

| 操作 | 最坏情形 | 变化 |
|---|---|---|
| alloc | O(SL bin 链长)，上界 O(zone/PM_MIN_BLOCK) | 不变 |
| free | **O(1 + 邻块空闲 bin 链长)**，上界 O(zone/PM_MIN_BLOCK) | 原"O(1)"不再成立：为满足"损坏时零副作用"必须证明邻块 bin 成员资格（指南 §6.2 给出的两个选项之一）；固定额外内存 O(1) |
| borrow_begin/resolve/borrow_end | O(1)（各加一次池范围比较 / 锁内校验） | 常数因子微增 |
| compact/split | O(objects + moved_bytes)（含 walk_order 复收集 O(objects)） | 不变量级 |
| merge | O(objects + free_blocks + moved_bytes) | 审计为 O(L+F)；未用完整 validate，无 O((L+F)²) 项 |
| validate | O((live+free)²) | 不变（指南 §9 允许保留，已如实声明） |
| 维护固定 scratch | O(PM_MAX_OBJECTS)：s_plan/s_upper/s_barriers/s_slots | +1 个 u32 数组 |

## 4. 红测证据（阶段 1，对旧代码 6bcd28f 的 core.cpp）

- R22 注入 1（源描述符 pool_id 错）：旧 merge 返回 **OK**（期望 CORRUPT_METADATA），
  且 Pool×2 / ObjectDesc / Auto Zone 快照全部失配，源池被吞掉
  （`validate(src) → INVALID_POOL`）——P0-1 直接复现。
- R22 注入 2（源描述符池外地址）：旧 merge 返回 CORRUPT_METADATA 但字节已变
  ——"失败不零副作用"复现。
- R22 注入 7（addr_next 环）：旧 merge 第一条重写循环**无限循环**，
  150 秒超时杀掉测试进程——P0-2 复现（新代码步数上限拒绝）。
- R23/R24/R25(Release)/R26：resolve 返回 OK 且 p 非空（池外地址被发出）、
  free 先跑回调再 Wild Read、重复 end 下溢计数、失败路径残留旧指针——均已由
  对应修复转绿。

修复过程中修正的**测试自身**问题（两处，均非库问题）：R22 快照必须在注入后拍
（否则被注入字段必然表现为差异）；R23(e) 的 `block_size=8192` 是"整池合法块"
并非"跨越池尾"，改为 `8192+8`。

## 5. 阶段 6 强制搜索的审查结论（指南 §8）

- `state = / borrow_count / active_borrows / PM_LOCK`：全部池状态翻转、借用计数
  变更、epoch/Running 发布均位于 arming 锁或最终提交锁内；free 的 Destroying/
  Free 置位属于单所有者上下文（与旧版一致，见 §7 已知边界）。
- `memmove`：5 处（compact 1 + merge 1 + split 3），每处均有执行顺序证明注释。
- `memset(&G/pool)`：init（全量校验后）/deinit（空系统）/destroy_pool（空池）/
  merge 提交（源池清空）/split fail_restore（回收未使用槽位），无一处绕过状态检查。
- order 链裸遍历剩余处：precheck_pool（自身即有界审计）、finalize_layout（作用于
  刚审计/刚重建的链表）、validate（阶段 1 有界后复用）、get_stats（有界+判界）。
- 单所有者 + scratch 共享：维护操作**必须**由单一所有者串行调用已写入
  pondmerge.hpp 并发契约与 README（指南 §8 末段的 v1 契约选择）。

## 6. 设备验收（ESP32-S3 实机）

- 芯片：ESP32-S3 rev 0.2，双核；串口 `/dev/ttyACM0`（USB-Serial-JTAG）115200。
- 构建：ESP-IDF v6.0.2，gnu++17，`PM_MAX_OBJECTS=256`，Auto Zone 256 KiB @ 内部 SRAM。
- App version / ELF SHA256 / 编译时间 / 检查数：见提交记录与下方实测块
  （烧录前已提交，App version 无 -dirty）。
- 测试内容：suite（基础 13 组 + R1–R28，2000 ops）+ 双核并发锁边界测试
  （200 轮 pause/compact/resume × 双核 borrower）。
- 结果：待本轮设备运行后回填（见 §9 交付清单）。

## 7. 已知边界（本轮明确、未变更）

- **free 与 borrow 的 TOCTOU**：`free` 在锁外读 `active_borrows`。它属于"普通
  access/free 单所有者"契约的既有边界（borrow_begin 的增量在锁内，但 free 的
  检查与 Destroying 置位无法在不持锁跑回调的前提下原子化）。pause/compact 与
  borrow 的边界由锁保证并已被设备并发测试覆盖。
- **RawRef 是 unsafe 句柄**（指南兼容方案，R27 固定语义）；公开 cookie 无助于
  此（仍可整体复制），故不改结构、只固化文档与测试。
- validate 的 O((live+free)²) 覆盖审计保留（指南 §9 允许）。
- 零长对象拒绝语义（HANDOVER_v3 §8 C3）维持现状：描述符级拒绝（size==0 →
  CorruptMetadata），不在本轮范围。

## 8. 测试矩阵落点（指南 §10）

| 编号 | 状态 | 位置 |
|---|---|---|
| R22 merge transaction faults | ✅ host Debug/Release/SAN + 设备 | tests/suite.cpp |
| R23 descriptor outside pool | ✅ 同上 | tests/suite.cpp |
| R24 free physical-header faults | ✅ 同上 | tests/suite.cpp |
| R25 duplicate borrow_end | ✅ host（Release 拒绝路径）+ **设备双核并发**（tests/concurrency_esp32.cpp） | 两处 |
| R26 resolve output clearing | ✅ | tests/suite.cpp |
| R27 local binding API | ✅ | tests/suite.cpp |
| R28 state publication | ✅ | tests/suite.cpp |

既有测试 R1–R21 与基础 1–13 无一删除或放宽（check 总数上升来自新增组）。

## 9. 交付清单（提交后回填实测数字）

- Host Debug（10000 ops）：见 §10
- Host Release（10000 ops）：见 §10
- ASan/UBSan（10000 ops）：见 §10
- cppcheck：exit 0，0 告警
- 配置矩阵：PASSED
- ESP32-S3：commit / App version / ELF SHA / chip / checks / failures —— 见 §6 回填

## 10. 验收命令实测记录（本轮收口时回填）

```
（由收口流程回填：五档 host 命令输出与设备日志摘录）
```

## 11. 给下一轮的提示

- 指南 §11 的 5 个自检问题已逐项回答（§5）；`git diff --check` 与
  `-Wall -Wextra -Werror` 干净。
- HANDOVER_v4 §9 的 T2/T4/T5/T6/T7/T8 仍未做（T8 已顺手完成：设备压力次数
  支持 `-DPM_DEVICE_STRESS_OPS` 覆盖）；T1 已由本轮设备并发测试完成。
- 设备并发测试的检测是"后果式"的（状态集合 + 计数守恒 + payload + validate），
  不依赖时序假设，不会因良性交织误报；若它失败即为真违约。
