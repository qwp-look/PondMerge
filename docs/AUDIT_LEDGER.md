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
| live-slot list | 是一个**无序的活对象袋**：双向链接、有限、无重复、全为对应池的 Live 描述符 | `collect_live_sorted()`、`precheck_pool()`、`validate()` 阶段 1 | 步数上限 PM_MAX_OBJECTS+1 + index 界 + state/pool/generation 核对 + addr_prev 链核对 | R8、R21、R22(7)(8) | ✅（本轮搬迁，见 §6.8） |
| live-slot list | **地址序只在维护入口成立**：收集后按 (address, index) heapsort，重复地址即 `CorruptMetadata` | `collect_live_sorted()` + `sort_slots_by_address()` | 排序只读元数据值，不解引用不受信地址；重复地址按排序后的相邻对拒绝；heapsort 无递归无分配，最坏 O(n log n) | R21(5)、R22(7)、模型对拍、validate | ✅（本轮新增） |
| live-slot list | alloc 的追加是 **O(1) 且不可能失败**，故 alloc 在任何物理字节变更之后**已无失败路径** | `order_append()` + `alloc()` | 三条指针写入，无遍历；环链/断链改由每个维护入口与 `validate()` 有界拒绝（R29(5) 已按新语义重写） | R29(5) | ✅（本轮） |
| state | 池状态机 Entry(Running/Paused) → Merging/Splitting/Compacting → 提交发布；失败恢复入口状态 | `compact()`/`merge()`/`split()` 的 arming 与最终提交锁段 | 所有发布在锁内一次性完成；计划失败仅恢复状态、其余字节不动 | R7、R16、R22、R28 | ✅ |
| state | 新池在最终提交前不可观测为可用 | `split()` arming 段（认领为 Splitting） | state ∉ {Running} 时 alloc/borrow 全部 Busy | R16(C)、R28(c) | ✅ |
| borrow | begin/end 一一对应；token（index+generation+pool）在锁内校验；任何失配不动计数 | `borrow_begin()`/`borrow_end()` | 全部校验与递减在同一 PM_LOCK 内；active_borrows/borrow_count 双计数下溢不可能 | R10、R16、R25、设备并发测试（S3 LX7 + 经典 ESP32 LX6 两颗芯片） | ✅ |
| object lifetime | 构造（placement-new）、析构（destroy thunk）恰好一次；movable 不带析构 | `pm_make`/`pm_make_pinned`/`detail::destroy_thunk`/`set_destroy_fn()` | movable 拒绝 destroy_fn（NotRelocatable）；pinned 析构在 free 验证后恰好跑一次 | R6、R19、R24 | ✅ |
| object lifetime | 可搬移类型显式 opt-in 且经审计（不含 Auto Zone 自指针/DMA/同步原语） | `pm_is_relocatable` 特化（tests/suite.cpp 顶部） | 测试中注册的 5 个类型均为 POD；含裸指针的 RawHolder 特化被 static_assert 拒绝 | R5 | ✅ |
| 统计 | used/free/fragment/live 与物理布局精确互锁 | `validate()` 第三段覆盖审计 | live+free+slack==capacity 且 free_total==free_bytes-fragment_bytes | R21(1-4)、validate | ✅ |
| 失败输出 | 任何公共入口失败时输出引用/指针必为无效（generation 0 / nullptr），旧值绝不残留 | `alloc()` 首语句清空（第五轮）、`resolve()` 入口、`borrow_begin()` 失败分支 | 失败在 `out` 赋值前返回或显式清空；槽位 generation 不动（R1 语义保持） | R26、R29(5)、R30 | ✅（第五轮补齐 alloc） |
| 整理建议 | analyze/poll 严格只读：Pool/ObjectDesc/Auto Zone 逐字节不变；verdict 五值；估算不伪造 | `analyze_compaction()`/`poll_compaction_advice()`（建议缓存为独立建议态，非分配器元数据） | 快照比对 + walk_order/get_stats 有界审计 | R31 | ✅（第六轮新增） |
| pool geometry | 池的段窗口 [segment_first, segment_first+segment_count) 必须整体落在 zone 内（段和与字节量双界，uint64 计算，不回绕不溢出） | `pool_geometry_ok()`（O(1)；check_ref / alloc / precheck_pool / create_pool 共用） | 段和与字节体积都在 uint64 中计算，损坏的 segment_count 既不能回绕也不能溢出成看似合法的窗口；字节界独立于段数对 `G.zone_size` 成立 | R44、R23 | ✅（第十四轮） |
| create_pool | 现存池的段窗口**先证明后使用**：used[] 标记不越界，新池不与存活池物理重叠 | `create_pool()` 对每个非 Empty 池的 `pool_geometry_ok()` 前置 | 未证明的窗口意味着 `bool[PM_MAX_SEGMENTS]` 越界写，或新池建在存活池的块上；现先过同一 O(1) 几何检查再标记 | R43 | ✅（第十四轮） |
| live-slot list | Live 描述符的 addr_prev/addr_next 指向描述符表内（NO_ORDER 除外）——`order_unlink()` O(1) 解引用的前提 | `check_ref()` 链接界筛查段 | free/borrow/resolve/set_destroy_fn 共用同一次 O(1) 检查，先于任何调用方到达变更路径；维护侧由 `collect_live_sorted()` 的更强链核对覆盖，不重复 | R39、R38 | ✅（第十四轮） |
| 失败输出 | alloc 在任何物理字节变更**之前**的 O(1) 筛查（order_head 界、池几何）失败同样清空输出引用 | `alloc()` 首语句清空 + 筛查段 | 两项筛查位于所有写入之前，`out` 已被首语句清空——失败输出不变式对新筛查路径天然成立，回滚路径不变 | R38、R44 | ✅（第十四轮） |

## 2. 遍历有限性清单（第四轮任务书 §7 全量搜索结论）

| 遍历 | 步数上限 / 防护 | 位置 | 备注 |
|---|---|---|---|
| bins_find | max_hops = zone_size/PM_MIN_BLOCK+1 + 游标判界 | core.cpp | ✅ |
| **collect_live_sorted** | PM_MAX_OBJECTS+1 + index 界 + state/pool/gen + addr_prev 链核对 | core.cpp | ✅ **本轮取代 `walk_order`，成为唯一受认可的活对象枚举入口**（compact/merge/split/validate/advice 共用） |
| sort_slots_by_address | 原地 heapsort；输入规模已由 collect 界定，无自身遍历上限需求 | core.cpp | ✅ 本轮新增（无递归、无分配、最坏 O(n log n)） |
| audit_pool_bins | capacity/PM_MIN_BLOCK+1 + head_ok + 互逆链接 | core.cpp | ✅ |
| validate 阶段 1 / 阶段 2 / 覆盖段 | collect 的上限 / capacity 上限 / 阶段 1 已证无环后复用 | core.cpp | ✅ |
| get_stats | capacity 上限 + 越界判界，拒绝时 valid=0 | core.cpp | ✅ |
| free_block_binned | capacity 上限 + 越界判界 | core.cpp | ✅ |
| ~~order_insert_sorted（alloc 路径）~~ | **本轮删除** —— alloc 不再遍历任何链表，改为 O(1) `order_append` | core.cpp | 该遍历及其防环上限需求随之消失；同类损坏的保护由 `collect_live_sorted` 承接（见 §6.8） |
| order_unlink | O(1)，无遍历 | core.cpp | 输入界由 `check_ref` 保证（第十四轮：Live 描述符的 addr_prev/next 表内筛查，R39；alloc 侧另有 order_head 界筛查，R38） | ✅ |
| finalize_layout | **不再遍历链表**：迭代已审计的地址序槽位数组，超出 [start,end) 的条目按范围跳过 | core.cpp | ✅ 本轮简化（split 因此不再依赖"边界以下即前缀"的未验证假设） |
| 模型/测试侧遍历 | 不触及库内部（模型仅用公共 API） | tests/model.cpp | ✅ |

## 3. 故障注入矩阵覆盖（第四轮任务书 §11）

| 字段组 | 零值 | 最大值 | 未对齐 | 池外 | 环 | 重复 | 溢出 |
|---|---|---|---|---|---|---|---|
| descriptor index/generation | R18✝/R26 | R25 | — | — | — | — | R9 |
| descriptor pool/address/size/block_size | R17(size=0) | R23(e) | R17/R20 | R23(a-d) | — | — | R9/R17 |
| addr_prev/next/order_head | R21(5) | R22(8) | — | R38、R39 | R8、R22(7)、R29(5) | R21(5) | R22(8) |
| bin head/prev/next/位图 | R21(6)(7) | R18(b) | — | R18(b)、R21(9) | R8(2)、R18(a) | R29(2) | — |
| 互逆链接 | — | — | — | R29(1) | — | — | — |
| block header/prev_size | R8(3) | R21(8) | R20 | R7 | — | — | R24(3)(7) |
| used/free/fragment/live | — | R21(1-4) | — | — | — | — | — |
| segment_first/count | — | R29(3) | — | R29(3)、R43 | — | — | R44 |
| state/borrow_count/active_borrows | R29(4)（安全拒绝语义） | — | — | — | — | — | — |

✝ R18 的零值 generation 注入在 R12（generation=0 保留值）。

## 4. 并发承诺表（第四轮任务书 §9）

| 操作 | 并发承诺 | 证据 |
|---|---|---|
| alloc/free | 单 owner | pondmerge.hpp 并发契约；README |
| resolve/get_stats/validate | 单 owner（后两者只读，仍不承诺并发） | pondmerge.hpp 并发契约（第五轮明确列出）；README 并发表 |
| resolve/get_stats | 单 owner 或 quiescent | pondmerge.hpp（resolve 注释）；get_stats 为只读统计 |
| borrow_begin/end | 内部锁 + token | 同一 PM_LOCK 内校验并递减；设备双核测试（两颗芯片：LX7 + LX6） |
| pause/resume | 内部锁 | 状态翻转在 PM_LOCK 内 |
| compact/merge/split | 单 owner 串行（scratch 共享，跨池并发亦禁止）；入口与最终提交持锁；搬移在锁外，安全性由单 owner + 外部静默契约承担 | pondmerge.hpp 并发契约（含 PM_LOCK 范围声明）；README 并发表；设备并发测试 |
| init/deinit/create_pool/destroy_pool | **lifecycle 串行**：不得与任何其他 API 并发（它们直接改写池表/全局状态，不持 borrow 锁） | pondmerge.hpp 并发契约（第十四轮补明）；USAGE_GUIDE §7 |
| set_destroy_fn | 单 owner（与 alloc/free 同级） | pondmerge.hpp 并发契约 |
| global_stats | 只读高水位**宽松读**：字段为高水位计数，无逐字段一致性承诺 | pondmerge.hpp 并发契约（第十四轮补明） |
| 重复 end / 错误 token | 任何失配不动计数 | host Release R25；设备固件为 Debug 构建，重复 end 属调用方 bug 会断言——**已接受边界**（见 §6） |

## 5. 复杂度账本（第四轮任务书 §12，与真实循环一一对应）

| 操作 | 声明 | 依据循环 | 变化 |
|---|---|---|---|
| alloc | **O(SL bin 链长)**，上界 O(zone/PM_MIN_BLOCK)；**拒绝路径 O(FL×SL)**；`PM_ZERO_INIT` 另加 O(size) 清零 | `bins_find`（链内 first-fit）+ 两项 O(1) 筛查（order_head 界、`pool_geometry_ok`，先于任何变更）+ `order_append`（O(1) 追加，无遍历） | **第十轮**：地址序插入移出热路径后 O(live) 项消失（实测 799→38 ns @1024，232→38 ns @256；`bench/RESULTS.md`）；**第十四轮**：O(1) 筛查不改变声明（R38/R44）；**第十五轮**：`bins_find` 仍为链内 first-fit，但**拒绝诊断**由「审计全部空闲链 O(free_blocks)」改为「位图/头一致性 O(FL×SL)」——设备实测碎片池上一次被拒绝的分配 **160,074 → 8,041 ns**（比值 20.30× → **1.01×** 一次正常分配）。代价见 §6 第 9 项（R54/R55 钉住） |
| free | **O(1)** | `free_block_binned` 的锚定链接证明（证明邻块自身的 prev/next 与该位置互逆，而非遍历 bin 找它） | 第三轮已更正；**第十四轮**：第二次邻块验证仅在 `destroy_fn` 存在时执行——无回调时两次验证之间不可能有代码运行（单 owner 契约），第二次证明是第一次的确定性重放；R24 的"回调后重验"契约对带回调对象逐字保留。callgrind 指令数 **756.7 → 728.5 /pair（−3.7%）**（host x86-64，callgrind 确定性计数、两运行长度差分，方法见 `bench/README.md` §2 的 `host_insn`）；**第十五轮**：邻块成员性证明由 O(bin 链长) 遍历改为 O(1) 锚定链接证明，设备实测链尾邻居 K=1/16/64 → 5,613/7,375/12,975 ns（原）vs 5,608/5,617/5,617 ns（新）：**斜率 131 ns/跳 → 0，且 K=1 时两者相同**（常见情形零成本），K=64 快 2.31× |
| compact | O(objects log objects + moved bytes) | precheck（含 O(1) 池几何证明）→ collect（有界遍历 + heapsort）+ 计划 + 搬移 + finalize | **第十轮**：审计由 O(objects) 变为 O(objects log objects)，换来 alloc 与 live 数解耦；**第十四轮**：登记 precheck 的 O(1) 几何证明项 |
| merge | O(objects log objects + free_blocks + moved bytes) | 两池 precheck（各含 O(1) 几何证明）+ collect（各含一次排序）+ audit_pool_bins + 计划 + 搬移 + finalize | **第十轮**同上；**第十四轮**同 compact |
| split | O(objects log objects + moved bytes) | 同上（precheck + 排序 + finalize × 2 池） | **第十轮**同上；**第十四轮**同 compact |
| validate | **O((live+free) log(live+free))** | 覆盖审计改为「一次地址序归并」：`gap_before` 的前缀最大 end 改为随归并推进的 `run_max_end`，空闲块以 `s_free_off[]` 排序后并入同一趟 | **第十五轮**：此前是全库唯一剩余的超线性路径（设备实测指数 **1.70**）；改后指数 **0.98**，384 块 20.1 ms → 0.74 ms（**13.1×**），每块成本由 17,354–50,383 ns 变为 3,692–3,843 ns（**平坦**）。判据逐条等价（`AUDIT_LEDGER` §6 第 10 项 + `tests/suite.cpp` R54/R55 与 validate 全组）；v19 起二次扫描**已删除**：binned 空闲块数超过 live+1 是损坏（free() 合并物理相邻空闲块），在 O(n) 内拒绝（R57）；损坏布局曾借此把 validate 扣在二次回退里 11.3 s（host 1 MiB 实测）（§7） |
| analyze_compaction | O(objects log objects + free_blocks) | collect（含排序）+ 打包模拟 + get_stats | **第十轮**补登记（此前未列入本表）；**第十四轮**：fragmented 判定为交叉相乘恒等式（`stranded*1000 >= permille*capacity`，无除法；`fragment_ratio_permille` 报告字段保留一次 64÷64，已记档） |
| get_stats | O(free_blocks)，步数上限 | bins 全遍历 | + valid 字段 |
| 固定 scratch | O(PM_MAX_OBJECTS)：s_plan/s_upper/s_barriers/s_slots(uint16) **+ s_ord_key(uint32) + s_free_off(uint32, N+1)** | — | 设备侧 30 B × PM_MAX_OBJECTS（1024→30,720 B；权威逐配置数值 = `global_stats().metadata_bytes`）；第十轮**未**新增 scratch（排序原地进行），第十四轮亦然；**第十五轮新增 8·PM_MAX_OBJECTS + 4 字节**（s_ord_key 4N + s_free_off 4(N+1)；排序已用原地堆排，故**没有**第二块排序缓冲）：256 时为 **+2,052 B**，1024 时为 **+8,196 B**。实测 256/16：`metadata_bytes` 32,776 → **34,837** |
| 栈使用 | 维护路径无递归；heapsort 的 sift 为尾递归式循环（O(1) 栈）；最大局部为 snapshot lambda 与 verify_neighbours（均 O(1) 栈） | — | — |

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
   SMP 证据只来自双核设备测试，**已在两颗不同芯片上取得**：ESP32-S3
   （LX7）与经典 ESP32（D0WDQ6，**LX6**，portMUX 实现与 cache 均不同）。
   两平台的 `tests/concurrency_esp32.cpp` 均为 28 checks / 0 failures，且
   各自复位重跑两次的核验计数一致（调度计数可变，被核验的性质不变）。
   已写入 pondmerge.hpp 并发契约与 README。
7. **维护期并发模型**：解锁搬移 + 单 owner/外部静默契约是 v1 既定架构
   （第五轮指南 §4：真正 SMP 安全需重新覆盖全部读写，属重设计，不在 v1）。
   debug-only owner token 评估后不采用：host 单线程下无执行价值，设备上
   维护操作本就要求单 owner 调用——本轮交付为契约文档化（头文件 + README 表）。
8. **alloc 不再检测 live-slot 链损坏**（本轮不变量搬迁，见 §1 与 §2）：
   alloc 的追加是 O(1) 且不遍历该链，因此环链 / 断链不再由 alloc 报
   `CorruptMetadata`——该职责移至每个维护入口与 `validate()`，仍为有界时间、
   零副作用（R29(5) 已按新语义重写，并断言失败维护后池状态回到 Running）。
   alloc 的追加会写入既有结构的一个链接字段（新节点的 `addr_next`、旧头的
   `addr_prev`）：在已损坏的结构上这是"不加重、也不修复"的写入。
   **换来的**是 alloc 与 live 数解耦（实测 844.2→38.4 ns @live=1024、58.0→38.0 ns
   @live=16；同一基准源码编译到两个 core 修订，唯一的差异是 core，见
   `bench/RESULTS.md` §1）以及地址序维护成本的冷路径化（每次维护一次
   O(n log n)，在 memmove 面前可忽略）。
   若将来需要"alloc 也拒绝损坏结构"，须先恢复一次遍历，届时会重新引入 O(live)。
   **第十五轮补充（本项的第二处同族收窄）**：alloc 的**拒绝诊断**同样不再遍历空闲链
   —— 它由「审计全部空闲链」改为「位图与链头一致性」检查（O(FL×SL)）。因此
   **链头之后**的损坏（后继游标出池、环链、互逆链接、prev_size 链）不再由 alloc 在
   这一条路径上**具名**为 `CorruptMetadata`：请求仍被拒绝（`NoSpace`），
   `validate()` 仍报出损坏，每个维护入口也仍拒绝。**丢掉的是诊断的具体性，不是拒绝本身。**
   收益：碎片池上一次被拒绝的分配 **160,074 → 8,041 ns**（设备；比值 20.30× → **1.01×**），
   而这条路径正是碎片化负载反复撞上的那条。
   两条半都被测试钉住：**R54**（深层损坏 → `NoSpace` 且 `validate()` 仍报损坏；
   位图/位级不一致 → `CorruptMetadata`）、**R55**（位图/头一致性规则的其余两条）。
   **第十四轮补充**：alloc 现在在追加之前做两项 O(1) 筛查（`order_head` 界、
   `pool_geometry_ok` 池几何），恢复"不加重"前提的可证明性——追加写入只发生在
   通过筛查的结构上（头在描述符表内、窗口在 zone 内）；筛查失败零写入、输出
   引用已被首语句清空（R38/R44）；free 侧对既有链接的解引用前提由 check_ref
   的链接界筛查保证（R39）。
9. **`validate` 保持二次复杂度（O((live+free)²)），不做线性化**（本轮决策，
   非遗漏）。已实测：96 块 11.0 µs → 1536 块 1744.6 µs（拟合指数 1.83，见
   `bench/RESULTS.md` §2）。线性化的自然做法是"把 live ∪ free 按地址归并成一次
   扫描"，但它需要 **O(free) 的额外 scratch** 来排序空闲块（bins 是按尺寸分箱的，
   不提供地址序）。而 **free 在最坏情形下不被 `PM_MAX_OBJECTS` 约束**：两个空闲块
   可以由一段 sub-minimal slack 隔开（`free()` 只合并物理相邻块，slack 头部被
   poison 为 0 因而不参与合并），因此 `free ≤ live + 1` 不成立。一个按
   `PM_MAX_OBJECTS + 1` 定长的数组会在合法的极端池上**误报 CorruptMetadata**，
   而 `validate` 恰恰是"报告 OK 即真的结构完好"的那一个函数，不能引入假阳性；
   按 `capacity / PM_MIN_BLOCK + 1` 定长则要 64 KB（256 KiB 池）甚至 512 KB
   （FL 上限 8 MiB），在设备 DRAM 上不可接受。
   **触发条件**（满足即应重新评估）：出现"把 `validate` 纳入运行时健康监控"的
   真实需求；届时应同时评估上述 scratch 预算与假阳性取舍。
10. **设备端实测未完成——板子不在线，不是未做**（本项目最显眼的开放项）。
    `bench/esp32/` 是一个独立于验收固件的 IDF 工程，编译**同一份**基准源码
    （`PM_BENCH_NO_HOST_MAIN`），已实测**构建与链接通过**：ESP32-S3 目标、
    `pondmerge_bench.elf` 3,747,168 B / `.bin` 179,280 B、DIRAM 用量
    204,361 / 341,760 B（59.8%）。但**烧录与串口抓取未执行**：内核日志记录
    `Sep 18 17:24:29 usb 1-2.1: USB disconnect`，`lsusb` 中已无 Espressif
    设备（`303a:1001`），连续三次探测均无 `/dev/ttyACM*`。因此
    **没有设备端绝对数字**，`bench/RESULTS.md` 的每一项都只是 Host x86-64。
    待板子重新接上后，这一步是"插上、烧录、抓串口"即可完成的工作，产物已就绪。
    重点要取的三项（主机做不到的）：alloc/free 的**绝对**值；compact 窗口的
    **百分位**（设备 mcycle 免费，主机上一次时钟读取可达 2.7 ms）；以及用
    `heap_caps_add_region` 给 FreeRTOS `heap_4` 划独立区域的**第三方基线**。
11. **池几何的 zone 内平移不由 `create_pool` 检测**（第十四轮）。`pool_geometry_ok`
    只证明一个池的窗口整体落在 zone 内（O(1)、防回绕/防溢出），不证明窗口之间
    不重叠——被损坏平移进 zone 内部的窗口会通过 `create_pool` 的前置检查（该检查
    的职责是证明 used[] 标记不越界，R43）。窗口重叠/平移由 `validate()` 与每个
    维护入口 precheck 的**字节账目核对**拒绝（used+free+slack==capacity、每块落
    自己池的窗内等），R29(3) 已钉住 segment_first/count 的池外/最大值注入。这是
    "O(1) 前置筛查只证明 O(1) 可证的事"的边界，非遗漏。
12. **任务书 §8.3 的"校验失败恢复 Paused"被事务式实现取代**（第十四轮，A1-5）。
    维护计划失败恢复**入口状态**（Running 进入恢复 Running、Paused 进入恢复
    Paused），不是一律 Paused：计划失败零写入，不存在需要 Paused 来保护的中间
    态；compact 的借用拒绝仍按文档 §8 进入 Paused（唯一例外，调用方显式 resume）。
    `CorruptMetadata` 由调用方处置，库不替调用方决定继续与否。R29(5)、R49 钉住。
13. **8 B slack 后继的 prev_size 写入**（第十四轮，A2-L1）。free/alloc 的合并与
    切分把后继块的 prev_size 写在后继块地址 +4 处；当后继是 8 B 的 sub-minimal
    slack 时，该写入落在 [slack+4, slack+8)——slack 的 poison 头不受影响（写偏移
    在头部之后），且 slack 不是块，它的 prev_size 没有任何读者（只有真块合并才读
    prev_size，而 slack 永不参与合并）。探针 676 检查证实无误读。有意边界。
14. **FreeBlock `reinterpret_cast` 与 C++17 对象生命周期**（第十四轮，A2-L2）。
    块头按 [size|free | prev_size] 的原始 8 字节格式解释：header 经
    `load32`/`store32`（memcpy 语义）访问，链接字段经 FreeBlock 类型化成员访问。
    严格按 C++17 的对象生命周期模型，这是块格式边界上的**设计边界**
    （`internal.h` 既有注释"the block-format boundary"的账本面）；gcc/clang
    -O3 实测无失优、无 sanitizer 发现。不在 v1 引入生命周期显式化方案（C++23
    `std::start_lifetime_as` 一类）。
15. **`bins_find` 的游标筛查是 zone 级，其余遍历是池级**（第十四轮，A2-L3）。
    `bins_find` 用 `zone_off_readable`（偏移可读即可解引用），而
    `audit_pool_bins`/`free_block_binned` 用 `in_pool`（必须落在被遍历池内）——
    两种 doctrine 不一致。后果被三层限制：zone 界保证不解引用越区指针、fl/sl
    尺寸类核对拒绝外来块、失败一律 `CorruptMetadata` 且零副作用。列为**将来加固
    候选**（统一为池级筛查），不在本轮改行为。
16. **advice 打包模拟对描述符地址做原始指针算术，且先于 counters 审计**
    （第十四轮，A2-L4）。模拟按地址序读描述符并解引用 payload 地址做
    memmove 形状的估算；若描述符地址字段可被外部损坏，这可能解引用越界指针。
    当前**无 API 途径**使描述符地址失效（地址只由库写入，位于库私有静态数据
    之后的 zone 内），故列为**纵深防御缺口**而非缺陷；加固候选：模拟前先跑
    counters 审计或对地址做池界筛查。
17. **lifecycle 并发边界**（第十四轮，A2-L5/A3-4）。init/deinit/create_pool/
    destroy_pool 是 lifecycle 串行操作，不得与任何 API 并发（它们直接改写池表
    与全局状态，不持 borrow 锁）；`set_destroy_fn` 单 owner（与 alloc/free 同
    级）；`global_stats` 是只读高水位宽松读，无逐字段一致性承诺。已同步写入
    §4 并发表与 `pondmerge.hpp` 并发契约注释。

## 7. 死分支登记（第十四轮，A4-16）

覆盖率门（Debug 档 97.59% 行 / 81.15% 分支选取）里**从未被执行的分支**中，有一批
是守卫或被前序检查支配的分支——它们不是缺口，也不该被当作缺口。本清单登记其中
结构性最重要的条目与一句支配论证，**目的是防止未来轮次把它们误当未测缺口或误加
"覆盖"**。行号按 `fdbe5d4` 版 `core.cpp`（第十四轮改动后行号有漂移，函数名不变）：

| 位置（fdbe5d4） | 内容 | 支配论证（一句话） |
|---|---|---|
| L61 | `fl_index` 的 `size < (1u << MIN_FL)` 钳制 | 每个入 bin 的块 ≥ PM_MIN_BLOCK = 2^MIN_FL（alloc 把 need 钳到 PM_MIN_BLOCK，free 只合并 ≥ PM_MIN_BLOCK 的块），分支不可达；保留为函数自身的完备性守卫 |
| L35 | `log2_floor_u` 的 `v <= 1` 分支 | 唯一调用方是 `fl_index`，size ≥ PM_MIN_BLOCK > 1，恒假；totality 守卫 |
| L256 | `collect_live_sorted` 的步数上限 | 任何环都在重复访问节点时先被同一次遍历中的 addr_prev 链核对拒绝（重复访问的 prev 必与记录不符），步数上限是防御纵深 |
| L345 | `check_ref` 的 `aoff + d.size > pend` | 由 `desc_block_consistent` 的 `size ≤ block_size − HEADER` 与同一证明段的块界项共同支配（payload ≤ block ≤ 池尾），结构上不可假 |
| L836 | `init` 的 `seg_count == 0` | 前一句已拒绝 `zone_size < segment_size`，走到此处 seg_count ≥ 1 |
| L838 | `init` 的 `seg_count > UINT32_MAX / segment_size` | seg_count = zone_size/segment_size（uint32 除法）≤ UINT32_MAX/segment_size，数学上不可达；任务书要求的溢出守卫，防御前序检查被改动 |
| L943 | `destroy_pool` 的 `live_objects != 0 ‖ borrow_count != 0` | 可达的调用方错误路径（非空/被借用的池不可销毁），验收负载总先清空池；API 契约保留该拒绝 |
| L1059 / L1445 | alloc 与 advice 请求路径的 `need < PM_MIN_BLOCK` 钳制 | 默认 PM_MIN_BLOCK=16 下 need ≥ 16 恒成立（header + 对齐后的最小负载已达 16），分支死；**PM_MIN_BLOCK=32 变体下活**——config_matrix 已有 32 变体真实执行它 |
| L1079-1081 | alloc 对 bins_find 返回值的 `blk_size_of(blk) < need` 拒绝 | `bins_find` 只返回 `blk_size_of(cand) >= need` 的候选（其内层 first-fit 检查），该重查被支配；保留为与 bins_find 内部契约解耦的独立守卫 |
| L1466 | `fragment_ratio_permille` 报告字段的 `a.capacity ? … : 0` 三元 | capacity==0 仅在 Empty/未创建池上可能，验收负载未对这类池查询建议；除零防御，非不可达代码 |
| L1519 | `counters_ok` 合取的 `frag <= free_b`、`largest <= free_b - frag` 假分支 | 合法布局中 fragment ⊆ free、largest ≤ free−fragment 由 finalize/审计维持；计数器注入（R21(1-4)、R35）总被更早的合取项短路 |
| L1544 | fragmented 判定的 `a.capacity != 0` 项 | capacity==0 ⇒ stranded=0 < fragment_min_bytes，`&&` 链在该项之前短路；病态阈值（min_bytes=0）不在验收配置内 |
| L1573 | `moved_bytes <= 0xFFFFFFFE` 的 UNKNOWN 桥接 | moved_bytes ≤ 池容量 < 2^PM_FL_MAX（init 的 FL 上限拒绝），恒在界内；保留为估算诚实性契约的形状 |
| L1643 | compact arming 的 `state != Paused → Busy` | 锁内先经 `pool_maintainable` 拒绝维护态，Running 已被上一句翻转为 Paused，到达此处 state 必为 Paused |
| L2152 | validate 阶段 1 `audit_block` 的 `!in_pool(aoff, d.size)` 假分支 | 同 L345：payload 尾越出池尾被 `size ≤ block_size` 与同条件的块界项支配 |
| 第十五轮：`collect_live_sorted` 的 `sort_slots_by_address` 回退 | 打包键排序的宽度前置条件（`G.zone_size <= 2^24` 且 `PM_MAX_OBJECTS <= 256`）在全部验收配置与设备构建下恒真（zone 上限由 init 拒绝；对象表上限由编译期常量给出）；保留为宽度契约的完备性守卫 |
| 第十五轮：`linear_sweep` 的 `nfree > kFreeOffCap` 拒绝 | ~~恒不可达；保留为回退而非假设~~ **v19 更正并升级为拒绝**：结构性审计不约束极大性，损坏布局可以入箱超过 live+1 个空闲块（均匀 16 B 块实测穿过全部前置检查），因此该分支**可达**；`free_blocks ≤ live+1` 是库产布局的契约，超限即损坏 → O(n) 拒绝（R57），二次回退删除 |
| 第十五轮：`move_block` 的对齐回退 `memmove` | 块起址与块尺寸都是 PM_ALIGNMENT 的倍数（块格式与分配器不变量），且搬迁目标由规划器按同一对齐产生；保留为「宁可慢也不猜」的正确性守卫 |
| 第十五轮：`bins_bitmap_consistent` 的「头部尺寸不选它所在 bin」规则 | 公开 API 下构造不出会走到它的形状：抬高头部尺寸会让它成为本次请求的**适配目标**（bins_find 成功，诊断路径根本不运行），压低到请求以下则由链上其余节点满足遍历；保留为「头与位图必须自洽」这一族规则的完备性守卫（R55 覆盖同族的另两条） |
| L2234 | validate 阶段 3 的 `same_start != 1` 重复起点拒绝 | 阶段 2 的 fl/sl 尺寸类匹配与互逆链接已拒绝任何重复登记（同一偏移不能同时匹配两个尺寸类）；保留为零长度 gap 的最后防线 |

### 7.1 覆盖缺口逐行归账（v20，gcov 修好后首次可行）

v17 §9 记录的"gcov 注解空壳"已破案并修复：门禁在 `build/cov/` 里运行 gcov，
而 `.gcno` 记录的相对源路径在彼处不解析——摘要百分比照常算出，注解文件却只写
一个头（`Cannot open source file src/core.cpp`）。修复：门禁把 `src/`、
`include/`、`tests/` 符号链接进 pass 目录。注解文件自此有全文。

Debug 档 45 个未覆盖行（Release 档 42 个）逐行归账如下：

| 未覆盖行 | 归账 | 性质 |
|---|---|---|
| `pm_debug_abort` / `pm_debug_assert_fail` 全函数（1,088–1,103） | Debug 断言中止路径：套件不触发 `operator->` 失败（那是编程错误中止，探针已验证） | 预期未覆盖 |
| `sort_packed_keys` 全函数（392–409） | 仅在 `PM_MAX_OBJECTS <= 256` 且 zone ≤ 2^24 时运行——host 门禁 1024 走比较器回退；**设备构建走此路径**（§7 已登记的宽度契约守卫） | 配置守卫，设备已覆盖 |
| `move_block` 的 `memmove` 重叠回退（67–68） | 规划器证明不重叠才 memcpy；重叠即契约违反 | 防御分支 |
| `bump_epoch` 的回绕重映射（254–256） | 需要 2^32 次整理 | 预期不可达 |
| `check_ref` 的对齐拒绝（566）、`free_block_binned` 根检查（643）、`audit_pool_bins` 两条（783/799）、`compact_impl` 两条（1,001/1,020）、`merge` 的 Busy/恢复/分类（2,092–2,180）、`split` 的规划分类（2,342–2,344）、`alloc` 的首适应耗尽尾（1,456–1,457）、`destroy_pool` Busy（1,249）、`borrow_end` 的 token 分支（1,703）、`analyze_compaction` 的 INVALID_REQUEST 尾（1,822–1,823） | 损坏/故障注入拒绝路径：**可达但需要更细的注入场景**（对 R13–R15/R28/R47 未覆盖的特定子分支，如 merge 的 PinnedConflict 分类、compact 的 Pinned-vs-NoSpace 区分） | 后续轮次的红测试候选清单 |
| `borrow_end` 的 assert（1,721） | 同 pm_debug_abort：`operator->` 中止路径 | 预期未覆盖 |

## 8. 账本维护记录

- 2026-09-24（第十五轮）：同步维护路径算法重构的落地（free 的 O(1) 锚定证明、
  validate 的线性化、alloc 拒绝诊断的 O(bins) 化、搬运原语的可证不重叠选择、
  地址序排序的打包键 + 自适应方向、precheck 审计的原生指针宽度）；新增 §6 第 9 项
  （alloc 拒绝诊断的第二处同族收窄）与 §7 的四行死分支登记；复杂度表与 scratch 计量同步。
  性能证据与仪器见 `bench/RESULTS.md` §9。
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
- 2026-09-12（第十轮，性能轮）：**先测量再改**。新增 `bench/`（alloc 延迟、
  validate 标度、碎片 A/B 三套基准 + RESULTS.md），实测确认 alloc 的成本
  ≈100% 是 live-slot 链的**地址序插入**且线性于 live 数（斜率 1.42 ns/live），
  同时用**负结果**排除了两个看似可疑的候选（调大 `PM_SL_COUNT` 无效、
  `get_stats` 本来就便宜）。据此实施不变量搬迁：`order_insert_sorted` 删除，
  改为 O(1) `order_append`；新增 `collect_live_sorted()`（唯一受认可枚举入口，
  承接环链/断链检测）与原地 heapsort `sort_slots_by_address()`；
  `precheck_pool` 改为输出地址序槽位数组；`walk_order` 删除；`finalize_layout`
  改为迭代数组并按范围过滤（split 不再依赖"边界以下即前缀"的未假设）；
  R21(5)/R22(7) 修正了两处**测试自身**在新语义下失效的假设（注入点恰好已是
  尾部、修复时硬编码了 NO_ORDER）；R29(5) 按新语义重写并断言失败维护后状态
  回到 Running。实测收益：稳态 alloc 799→**37.6 ns** @1024、232→**38.5 ns** @256
  （均与 live 数解耦）；free 与 validate 标度不变；碎片基准 A/B 结果逐项不变
  （无行为回归）。接受边界新增 §6.8。
  **注（第十一轮更正）**：本条目中的绝对值产生于一个未量化的计时仪器，
  已被 `bench/RESULTS.md` 用可靠仪器重测取代——见下一段。形状结论
  （线性 → 平坦）不变且被强化。
- 2026-09-18（第十一轮，测量仪器轮）：**发现并修正了一个使前十轮的绝对值
  全部不可信的计时缺陷**。为做设备实测而复核基准时发现：探针主机
  （VMware 客机）拦截 `RDTSC`，`steady_clock::now()` 单次调用耗时 **~9,400 ns**，
  而被测操作只有 36–90 ns——**仪器是被测对象的 100–250 倍**。数值本身正确
  （200 ms 延时测出 200.024 ms），这正是缺陷长期隐形的原因：**数值准确 ≠ 读取
  便宜**。逐项实测：空循环 0.01 ns、`sink += 1` 0.44 ns、64 位 LCG 依赖链
  1.01 ns（≈3 周期，说明 CPU 满速），而 `__rdtsc()` 10,244 ns、`__rdtscp()`
  9,936 ns、`clock_gettime` 9,341 ns、裸 syscall 19,530 ns（⇒ vDSO 生效但本身慢）、
  `clock()` **20,900 ns**。结论：该主机上**任何周期计数器指令都不可用**，
  因为被拦截的就是 `RDTSC` 本身。
  修正为 `bench/bench_timer.h`：**按批计时**（一个区间 N 次操作 ⇒ 仪器贡献
  `2·call_cost/N`）、**每次运行自报仪器与裁定**、**不产出逐次计时**（除非操作
  远长于时钟，且必须标注偏置）。并用真实负载标定了"场景偏置"比紧凑循环更大
  （184,296 B memmove：批量真值 2,780 ns、逐次 14,749 ns ⇒ 偏置 ≈11,969 ns；
  紧凑探针给 7,521 ns，故修正值仍偏保守）。
  据此重写三套基准并重测，**同时得到一个新的库缺陷**：
  **`pm_port_ticks_us()` 的 host 分支用 `clock()`（即 `CLOCK_PROCESS_CPUTIME_ID`）**，
  在 glibc 上是系统调用、实测 20.9 µs/次，而 `compact()` 每次调用它**两次**
  来填 `compact_time_us`——即库的唯一性能统计量为每次整理**加进了 ~42 µs 的
  自测开销**，约等于它实际搬运内存（184 KB 拷贝 2.8 µs）的 15 倍；同时
  `clock()` 是 CPU 时间而非墙钟时间，使被抢占的维护窗口被**低报**。host 分支
  改为 `CLOCK_MONOTONIC`（ESP32 分支本就是 `esp_timer_get_time()`，不动）。
  效果：碎片基准中整理耗时从 80 µs 原始值降到 61 µs，纯粹来自去掉库自己的时钟读取。
  重测结论（`bench/RESULTS.md` 全文替换）：alloc 的 pair 成本在 live 16→1024 上
  **x1.01（完全平坦）**，同一基准源码对两个 core 修订的 A/B 为
  **844.2 → 38.4 ns @1024（22×）**、58.0 → 38.0 ns @16；`PM_MAX_OBJECTS=256`
  配置下 36.7 ns 且同样平坦；`validate` 拟合指数 **1.83（二次确认）**；
  `get_stats` 指数 1.17、最大规模 2.7 µs ⇒ 负结果保持；`PM_SL_COUNT` 4/8/16 =
  40.7/37.8/40.8 ns ⇒ **负结果在可靠仪器下确认**（并记录了上一版 SL8=48.0 ns
  是单次噪声）。碎片 A/B 逐项不变（50/50 vs 1/50、3/500 vs 0/500，零 churn
  拒绝、结构审计 OK）；整理成本 0.061 ms 原始 / 0.053 ms 修正后。
  接受边界新增 §6.10（设备端未实测，板子离线）。

- 2026-09-18（第十三轮，整理窗口百分位轮，HANDOVER_v14）：**建议估算的精确性
  第一次在真实硬件上跨碎片形态检验**。`bench/compaction_window.cpp`（新增）在
  S3 上重建碎片状态 512 次、每次先只读建议后计时整理：`estimated_moved_objects/
  estimated_moved_bytes` 与 `compact` 实际搬迁在 **512/512 事件上偏差为 0**
  （0 UNKNOWN；CI 已断言 host 侧同项）。同时证伪并修正了一处文档归因：碎片
  基准的 scatter 按 `(i+1)%4` 释放、pinned 在 `(i+1)%16`（子集）⇒ **测量阶段
  实际无 pin**（探针实测 `has_pinned=0`，整合搬迁 189 对象/184,296 B 与原
  host 记录一致）；`fragmentation.cpp` 注释与 `bench/README.md` §4 已更正
  （原文保留）。pinned 屏障密集的真实效应由开发版实测记录（512 次整理后
  probe 重试全部失败——屏障把整合空间切成 ~1 KiB 口袋），印证 COMPACTION_POLICY
  §7 的定性预告。不变式本体无增改：本条登记的是**证据升级**（R31/R35 行的
  设备端佐证）与一处叙述修正。

- 2026-09-19（第十四轮，多代理深度审查与元数据筛查修复轮，HANDOVER_v16）：
  **四项 High 元数据筛查缺陷修复**（A1-1 alloc 的 O(1) order_head 界筛查、
  A1-2 check_ref 的链接界筛查、A1-3 create_pool 的窗口证明、A1-4 池几何
  zone 上限——全部 O(1)，见 §1 新增四行），**C-1** free 的第二次邻块验证按
  `destroy_fn` 门控（§5 free 行，callgrind −3.7%），**C-2** advice 阈值判定改
  交叉相乘、去 64 位除法（§5 analyze_compaction 行），**A1-6** generation 回绕
  的 Debug 报告；§5 alloc 行补 O(1) 筛查与 PM_ZERO_INIT O(size)，compact/
  merge/split 行补 O(1) 几何证明项，固定 scratch 注脚量纲更正（30 B ×
  PM_MAX_OBJECTS）；§2 order_unlink 行补输入界来源；§3 注入矩阵补 R38/R39/
  R43/R44；§4 并发表补 lifecycle 串行 / set_destroy_fn 单 owner /
  global_stats 宽松读三行；§6 新增 §6.11（zone 内平移）与第 12–17 项
  （A1-5、A2-L1…A2-L5/A3-4）；新增 §7 死分支登记（A4-16）。新增红测
  R36–R44、R46–R53（R45 空缺）与 config_matrix 的 PM_MIN_BLOCK=32 变体。
  全部为筛查与账面修正，**库的合法路径行为不变**（门禁计数增量全部来自
  新增测试组）。
