# PondMerge v1 审计账本（第四轮任务书 §3）

> 维护规则：每次修改都必须为受影响的行补充证据；测试通过不能替代证明。
> "证明"列给出论证要点；"测试"列给出钉住该不变量的回归组。
> 状态：✅ 已确认（有代码位置 + 证明 + 测试）／⚠️ 已接受的边界（见说明）。

## 1. 核心不变量账本

| 项目 | 不变量 | 代码位置 | 证明 | 测试 | 状态 |
|---|---|---|---|---|---|
| descriptor | Live 时 generation 非 0；size/block_size/alignment 自洽；size ≥ 1 | `core.cpp` `desc_block_consistent()`；`check_ref()` | 每次解析先过描述符本地检查；0 长与未对齐在 O(1) 内拒绝 | R5、R12、R17 | ✅ |
| descriptor | 块必须整体落在其所属池内（payload 与 block 双界） | `check_ref()` 池范围证明段 | uint64 zone-offset 运算，禁指针算术；`boff∈[pstart,pend)`、`block_size ≤ pend-boff`、`payload ≤ pend` | R23（5 类池外地址） | ✅ |
| block | 头部 `[size|free]` 与描述符一致；prev_size 链闭合（0 = 池头/slack） | `free()` `verify_neighbours()`；`prev_link_ok()`；`validate()` 阶段 1 | free/validate 双入口核对 header 与 prev_size；alloc/finalize 维护不变式 | R8、R21、R24 | ✅ |
| block | 块间与池尾 slack 首 4 字节 poison 为 0 | `finalize_layout()` | free 的前向合并只读 free 位，slack 头为 0 时不会误合并 | R15(c)、R20 | ✅ |
| free-list | 每个 free block 恰好属于其 size class 选中的 bin，双向链接互逆 | `audit_pool_bins()`（validate 阶段 2 共用） | 位图/链一致 + fl/sl 类匹配 + prev/next 互逆 + 有界步数 | R18、R21、R29(2) | ✅ |
| free-list | free 合并前证明邻块成员资格与互逆链接 | `free_block_binned()` | 有界遍历找到节点后核对其 prev/next 指回自身 | R24、R29(1) | ✅ |
| order-list | 地址有序、双向、有限、无重复、全为对应池的 Live 描述符 | `walk_order()`、`precheck_pool()`、`validate()` 阶段 1 | 步数上限 PM_MAX_OBJECTS+1 + index 界 + addr_prev 链核对 | R8、R21、R22(7)(8) | ✅ |
| order-list | alloc 自己的插入走链有防环上限，环链时有界拒绝且零副作用 | `order_insert_sorted()`（返回 bool）+ `alloc()` 先链后改块 | 步数上限在改任何链接前触发；alloc 回滚只释放槽位（第四轮红测探针：旧实现挂死） | R29(5) | ✅（第四轮修复） |
| state | 池状态机 Entry(Running/Paused) → Merging/Splitting/Compacting → 提交发布；失败恢复入口状态 | `compact()`/`merge()`/`split()` 的 arming 与最终提交锁段 | 所有发布在锁内一次性完成；计划失败仅恢复状态、其余字节不动 | R7、R16、R22、R28 | ✅ |
| state | 新池在最终提交前不可观测为可用 | `split()` arming 段（认领为 Splitting） | state ∉ {Running} 时 alloc/borrow 全部 Busy | R16(C)、R28(c) | ✅ |
| borrow | begin/end 一一对应；token（index+generation+pool）在锁内校验；任何失配不动计数 | `borrow_begin()`/`borrow_end()` | 全部校验与递减在同一 PM_LOCK 内；active_borrows/borrow_count 双计数下溢不可能 | R10、R16、R25、设备并发测试 | ✅ |
| object lifetime | 构造（placement-new）、析构（destroy thunk）恰好一次；movable 不带析构 | `pm_make`/`pm_make_pinned`/`detail::destroy_thunk`/`set_destroy_fn()` | movable 拒绝 destroy_fn（NotRelocatable）；pinned 析构在 free 验证后恰好跑一次 | R6、R19、R24 | ✅ |
| object lifetime | 可搬移类型显式 opt-in 且经审计（不含 Auto Zone 自指针/DMA/同步原语） | `pm_is_relocatable` 特化（tests/suite.cpp 顶部） | 测试中注册的 5 个类型均为 POD；含裸指针的 RawHolder 特化被 static_assert 拒绝 | R5 | ✅ |
| 统计 | used/free/fragment/live 与物理布局精确互锁 | `validate()` 第三段覆盖审计 | live+free+slack==capacity 且 free_total==free_bytes-fragment_bytes | R21、模型对拍 | ✅ |
| 失败输出 | 任何公共入口失败时输出引用/指针必为无效（generation 0 / nullptr），旧值绝不残留 | `alloc()` 首语句清空（第五轮）、`resolve()` 入口、`borrow_begin()` 失败分支 | 失败在 `out` 赋值前返回或显式清空；槽位 generation 不动（R1 语义保持） | R26、R29(5)、R30 | ✅（第五轮补齐 alloc） |
| 整理建议 | analyze/poll 严格只读：Pool/ObjectDesc/Auto Zone 逐字节不变；verdict 五值；估算不伪造 | `analyze_compaction()`/`poll_compaction_advice()`（建议缓存为独立建议态，非分配器元数据） | 快照比对 + walk_order/get_stats 有界审计 | R31 | ✅（第六轮新增） |

## 2. 遍历有限性清单（第四轮任务书 §7 全量搜索结论）

| 遍历 | 步数上限 / 防护 | 位置 | 备注 |
|---|---|---|---|
| bins_find | max_hops = zone_size/PM_MIN_BLOCK+1 + 游标判界 | core.cpp | ✅ |
| precheck_pool | PM_MAX_OBJECTS+1 + index 界 + prev 链 | core.cpp | ✅ |
| walk_order | 同上（merge/split/compact 规划共用） | core.cpp | ✅ |
| audit_pool_bins | capacity/PM_MIN_BLOCK+1 + head_ok + 互逆链接 | core.cpp | ✅ |
| validate 阶段 1 / 阶段 2 / 覆盖段 | PM_MAX_OBJECTS+1 / capacity 上限 / 阶段 1 已证无环后复用 | core.cpp | ✅ |
| get_stats | capacity 上限 + 越界判界，拒绝时 valid=0 | core.cpp | ✅ |
| free_block_binned | capacity 上限 + 越界判界 | core.cpp | ✅ |
| **order_insert_sorted（alloc 路径）** | **PM_MAX_OBJECTS+1（第四轮新增）** | core.cpp | 修复前无上限：环链挂死（红测探针 2 证实）；修复后有界拒绝且零副作用 |
| order_unlink | O(1)，无遍历 | core.cpp | ✅ |
| finalize_layout | 执行阶段走链，前置已审计/刚重建；Debug 步数断言（第四轮新增） | core.cpp | 计划 A 无失败分支，断言为"不可能性"守卫 |
| 模型/测试侧遍历 | 不触及库内部（模型仅用公共 API） | tests/model.cpp | ✅ |

## 3. 故障注入矩阵覆盖（第四轮任务书 §11）

| 字段组 | 零值 | 最大值 | 未对齐 | 池外 | 环 | 重复 | 溢出 |
|---|---|---|---|---|---|---|---|
| descriptor index/generation | R18✝/R26 | R25 | — | — | — | — | R9 |
| descriptor pool/address/size/block_size | R17(size=0) | R23(e) | R17/R20 | R23(a-d) | — | — | R9/R17 |
| addr_prev/next/order_head | R21(5) | R22(8) | — | — | R8、R22(7)、R29(5) | R21(5) | R22(8) |
| bin head/prev/next/位图 | R21(6)(7) | R18(b) | — | R18(b)、R21(9) | R8(2)、R18(a) | R29(2) | — |
| 互逆链接 | — | — | — | R29(1) | — | — | — |
| block header/prev_size | R8(3) | R21(8) | R20 | R7 | — | — | R24(3)(7) |
| used/free/fragment/live | — | R21(1-4) | — | — | — | — | — |
| segment_first/count | — | R29(3) | — | R29(3) | — | — | — |
| state/borrow_count/active_borrows | R29(4)（安全拒绝语义） | — | — | — | — | — | — |

✝ R18 的零值 generation 注入在 R12（generation=0 保留值）。

## 4. 并发承诺表（第四轮任务书 §9）

| 操作 | 并发承诺 | 证据 |
|---|---|---|
| alloc/free | 单 owner | pondmerge.hpp 并发契约；README |
| resolve/get_stats/validate | 单 owner（后两者只读，仍不承诺并发） | pondmerge.hpp 并发契约（第五轮明确列出）；README 并发表 |
| resolve/get_stats | 单 owner 或 quiescent | pondmerge.hpp（resolve 注释）；get_stats 为只读统计 |
| borrow_begin/end | 内部锁 + token | 同一 PM_LOCK 内校验并递减；设备双核测试 |
| pause/resume | 内部锁 | 状态翻转在 PM_LOCK 内 |
| compact/merge/split | 单 owner 串行（scratch 共享，跨池并发亦禁止）；入口与最终提交持锁；搬移在锁外，安全性由单 owner + 外部静默契约承担 | pondmerge.hpp 并发契约（含 PM_LOCK 范围声明）；README 并发表；设备并发测试 |
| 重复 end / 错误 token | 任何失配不动计数 | host Release R25；设备固件为 Debug 构建，重复 end 属调用方 bug 会断言——**已接受边界**（见 §6） |

## 5. 复杂度账本（第四轮任务书 §12，与真实循环一一对应）

| 操作 | 声明 | 依据循环 | 变化 |
|---|---|---|---|
| alloc | O(SL bin 链长 + live_objects)，上界 O(zone/PM_MIN_BLOCK) | `bins_find`（链内 first-fit）+ `order_insert_sorted`（地址序插入，有界） | **第四轮更正**：旧声明漏掉地址序插入的 O(live) 项 |
| free | O(1 + 邻块空闲 bin 链长)，上界 O(zone/PM_MIN_BLOCK) | `free_block_binned` 有界遍历 × 2 邻块 | 第三轮已更正 |
| compact | O(objects + moved bytes)（含审计 O(objects)） | precheck + walk_order + 计划 + 搬移 + finalize | — |
| merge | O(objects + free_blocks + moved bytes) | 两池 precheck + audit_pool_bins + 计划 + 搬移 + finalize | — |
| split | O(objects + moved bytes)（含审计） | 同上 | — |
| validate | O((live + free)²) | gap_before 嵌套 | 如实保留 |
| get_stats | O(free_blocks)，步数上限 | bins 全遍历 | + valid 字段 |
| 固定 scratch | O(PM_MAX_OBJECTS)：s_plan/s_upper/s_barriers/s_slots(uint16) | — | 设备侧 512 B×4 级别 |
| 栈使用 | 维护路径无递归；最大局部为 snapshot lambda 与 verify_neighbours（均 O(1) 栈） | — | — |

## 6. 已接受的边界（明确决策，非遗漏）

1. **free 与 borrow 的 TOCTOU**：free 在锁外读 `active_borrows`。属于"普通
   access/free 单所有者"契约；在不持锁运行回调的前提下无法原子化。
2. **设备上的重复 end / 错误 token**：设备固件为 PM_DEBUG=1，重复 end 会断言
   终止（调用方 bug）；计数不下溢的语义由 host Release R25 钉住（同一代码），
   SMP 维度由设备并发测试（不同 token 的并发 begin/end）覆盖。
3. **get_stats 不审计 state/borrow_count**：控制字段不是布局不变式；损坏时
   全部入口安全拒绝（R29(4) 钉住），validate 不为之背书。
4. **逐操作性能峰值**（validate/alloc/free 计时）：v1 只测维护耗时
   （compact_time_us，任务书 v2 §12.3 要求项）；不引入额外计时设施。
5. **RawRef 可伪造**：第三轮指南兼容方案（R27 固定语义）；local 绑定在解析期
   强制，伪造无法越界。
6. **Host PM_LOCK 为空操作**（第五轮指南 P3）：Host 运行不能证明锁语义；
   SMP 证据只来自双核设备测试；已写入 pondmerge.hpp 并发契约与 README。
7. **维护期并发模型**：解锁搬移 + 单 owner/外部静默契约是 v1 既定架构
   （第五轮指南 §4：真正 SMP 安全需重新覆盖全部读写，属重设计，不在 v1）。
   debug-only owner token 评估后不采用：host 单线程下无执行价值，设备上
   维护操作本就要求单 owner 调用——本轮交付为契约文档化（头文件 + README 表）。

## 7. 账本维护记录

- 2026-09-12（第四轮）：建立本账本；登记第四轮发现并修复的两项缺陷
  （order_insert 防环、alloc 损坏可区分）与两项口径更正（alloc 复杂度、
  get_stats valid 字段）；红测探针证据见 HANDOVER_v6 §3。
- 2026-09-12（第五轮）：新增"失败输出"不变量行（alloc 首语句清空，R30）；
  并发表补充 resolve/get_stats/validate 行与 PM_LOCK 范围说明；接受边界
  追加 Host 空锁与维护期并发模型两项；设备侧模型对拍缺口关闭（模型复用
  suite 的 g_zone，4000 ops，随固件运行）。
- 2026-09-12（第六轮）：新增整理建议不变量行；R31 钉住只读性与判定矩阵；
  examples/ 双端 Demo 与快照协议 v1（docs/DEMO_REQUIREMENTS.md）。
- 2026-09-12（第七轮）：建议结论新增 INVALID_REQUEST（输入错误与元数据损坏
  区分，cache 不被输入错误污染，R32）；poll 变化键补全至全部输入
  （state/pinned/字节账目/stats_valid/请求 echo/阈值，R33）；Advice 并发
  边界（owner 上下文、禁 ISR、非并发一致快照）写入公共 API 文档；
  external_quiescence_required 更名为 caller_must_establish_quiescence；
  demo 换内置 JSON 解析器 + protocol_smoke.py 协议回归（58 项检查）。
- 2026-09-12（第九轮）：Advice 估算改为打包模拟的精确计算（池首空闲块
  场景不再误报 0——旧行为被 R35 复现）；新增计数器审计（条件减法，损坏
  即 INVALID_METADATA，R35 故障注入覆盖 used/free/fragment/live/largest）；
  host_demo 物理行分帧（超长行单次拒绝、后缀不执行）；demo 全操作改用
  cross-ref（merge/split 后按稳定 id 释放/读取，pool hint 不再是身份证明）；
  demo_server 命令串行化（COMMAND_LOCK）+ 接收状态机（协议版本/来源/seq/
  会话）+ ESP32 display-only 显式拒绝 + degraded 状态；UI 增加 before/after
  diff（含 epoch/generation/digest 不变量断言）与静默期模拟标注；
  examples/http_smoke.py（15 项 HTTP 层检查）+ protocol_smoke 扩至 95 项。
