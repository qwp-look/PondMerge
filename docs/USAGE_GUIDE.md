# PondMerge v1 使用指南

配套文档：`QUICKSTART.md`（快速入门）、`docs/COMPACTION_POLICY.md`（整理策略）、
`docs/DEMO_REQUIREMENTS.md`（Demo 规范）。本文覆盖完整 API、生命周期与并发契约。

## 1. 三区模型

| 区域 | 管理者 | 规则 |
|---|---|---|
| Chaos Zone | 硬件/外设/驱动 | PondMerge 永不触碰 |
| Free Zone | 应用普通堆 | PondMerge 不管理 |
| Auto Zone | PondMerge | 固定容量，`init()` 注册；按 `segment_size` 划分 segment 再组合成 Pool |

Auto Zone 容量在 `init()` 时固定，之后不可扩大。所有被管理的对象只存在于
Auto Zone 的 Pool 中。

## 2. 生命周期

```
init(cfg) → create_pool → alloc/对象操作 → 维护（compact/merge/split）
          → destroy_pool → deinit
```

- `init` 先校验全部配置再复位全局状态；重复/失败的 init 不破坏存活系统
  （返回 `Busy`）。
- `deinit` 要求 `live_object_count == 0`；池未显式销毁也可以整体 deinit。
- `create_pool`：从空闲 segment 中找连续段；`destroy_pool`：要求池空且无借用，
  维护态返回 `Busy`。

## 3. 分配标志

| 标志 | 语义 |
|---|---|
| `PM_MOVABLE` | 可搬迁（需项目注册 `pm_is_relocatable<T>` 后经 `pm_make` 使用） |
| `PM_PINNED` | 地址固定；搬移屏障 |
| `PM_DMA` | implies pinned；DMA 正在使用的缓冲 |
| `PM_EXTERNAL` | implies pinned；外部库持有 |
| `PM_ZERO_INIT` | 分配后清零 |

标志在分配时指定，**不可事后修改**（Demo 的"标记 pinned"是重新分配）。

## 4. 引用与指针

| 概念 | 含义 | 何时变化 |
|---|---|---|
| object slot index | 描述符槽位，稳定的对象身份 | 不变（直到池销毁） |
| `generation` | 槽位复用计数，防 ABA | 仅在对象释放且槽位复用时递增（跳过 0） |
| `address_epoch` | 地址变更计数，纯诊断 | 搬迁 / 换池时递增 |
| `pm_local_ptr<T>` | 绑定创建时的池 | 对象换池后解析返回 `PoolChanged` |
| `pm_cross_ptr<T>` | 跨池引用 | 跟随对象到任何池；仍校验 generation |
| `RawRef` | 可复制、可伪造的低层句柄 | 解析时全量校验兜底 |

- `pm_local_ptr` 从 `CROSS_HINT` 构造会被无效化；伪造的具体 pool hint 在每次
  解析时被 `PoolChanged` 拒绝——**绑定在解析期强制**。
- 子对象视图（`ptr.at<U>(offset)`）offset 是**替换语义**（相对对象根）；
  子对象不能 free/destroy 父对象。

## 5. 访问方式与生命周期限制

| 接口 | 计数 | 返回 | 生命周期限制 |
|---|---|---|---|
| `try_borrow()` / `borrow_begin` | 池+对象双计数 +1 | RAII `pm_access<T>` / 指针 | 借用期间池不可整理；析构自动 end |
| `operator->` | 同上（表达式级） | 代理 | 仅限完整表达式 |
| `resolve` | **不计数**（高级接口） | 裸指针 | 仅静默期内立即使用；禁止跨越维护入口；失败输出为空 |
| `peek` | 不计数 | 裸指针 | 同 resolve |

失败语义：`resolve`/`borrow_begin`/`alloc` 失败时输出引用/指针**必为无效**
（generation 0 / nullptr），调用者复用旧变量不会误用。

## 6. 维护操作

| 操作 | 入口状态 | 失败语义 | 成功结果 | 复杂度（最坏） |
|---|---|---|---|---|
| `compact` | Running/Paused | 规划失败零改动；借用拒绝时**留在 Paused** | Running，epoch+1 | O(objects + moved) |
| `merge(s,t)` | 两池 Running/Paused | 规划失败两池逐字节不变 | target Running（epoch+1），source Empty | O(objects + free + moved) |
| `split(s,n)` | Running/Paused | 零改动；新池槽位回收 | 两池 Running；新池 epoch=1 | O(objects + moved) |
| `validate` | 任意非 Empty | — | Ok / CorruptMetadata | O((live+free)²) |

- 结构（四阶段）：加锁 arming → 只读审计+规划（scratch）→ 不可失败执行 →
  单锁最终提交。pinned/DMA/external 对象地址不变。
- 新池/合并结果只在最终提交发布为 Running，此前所有入口返回 `Busy`。

## 7. 并发契约（单 owner + 静默维护期）

| 操作 | 并发承诺 |
|---|---|
| `alloc` / `free` / `resolve` / `get_stats` / `validate` | **单 owner 上下文** |
| `borrow_begin` / `borrow_end` | 内部锁 + token 保护（双核安全） |
| `pause` / `resume` | 内部锁保护 |
| `compact` / `merge` / `split` | 单 owner 串行（不同池也不并发；scratch 共享） |
| DMA / ISR / 其他任务 / 外部库 | 调用方在维护前停止并排干 |

`PM_LOCK` 只保护借用计数与状态发布；Host 构建把 `PM_LOCK` 编译为空——
Host 运行不能证明锁语义，SMP 证据来自双核设备测试（`tests/concurrency_esp32.cpp`）。
构造函数与 destroy 回调不得对"正在构造/销毁的对象"重入分配器。

## 8. Debug / Release 行为差异

| 方面 | Debug（默认） | Release（`-DPM_DEBUG=0`） |
|---|---|---|
| `PM_ASSERT` | 失败 → `pm_debug_abort`（终止） | 编译为空 |
| 调用方 bug（重复 borrow_end、伪造 token） | 断言终止 | 忽略，不动计数 |
| finalize_layout 步数守卫 | 生效 | 编译出（前置已审计，逻辑不可能触发） |
| 安全校验（precheck、范围证明、free 验证） | **全部保留** | **全部保留** |

损坏元数据在任何构建下都返回 `CorruptMetadata`，不会静默清空池或伪装成
`NoSpace`。

## 9. 错误码

| Status | 含义 | 典型场景 |
|---|---|---|
| `Ok` | 成功 | — |
| `Busy` | 活跃借用/维护态阻挡 | 维护入口、paused 池上的 borrow |
| `NoSpace` | 真实耗尽 | 池满、FL 上限 |
| `InvalidPool` | 池 id 无效 | 未创建/已销毁 |
| `InvalidRef` | index/generation/offset/参数非法 | 陈旧引用、越界 |
| `InvalidAlignment` | 对齐非法 | >8 或非 2 的幂 |
| `PinnedConflict` | pinned 阻挡布局 | split/compact 规划 |
| `NotRelocatable` | movable 对象试图挂析构 | `set_destroy_fn` |
| `PoolChanged` | local 引用的对象已换池 | merge/split 后 |
| `AlreadyPaused` | 已处于 Paused | 重复 pause |
| `CorruptMetadata` | 元数据损坏（精确报告，不伪装） | 故障注入/内存踩踏 |

## 10. validate 与 PoolStats

- `validate(pool)`：三段审计（order 链 + bins + 覆盖归账），任何损坏有界返回
  `CorruptMetadata`；`get_stats(pool)` 的 free-list 走链有步数上限，
  `PoolStats.valid` 区分"损坏拒绝"与"真的没有空闲块"。
- `PoolStats` 字段：state、segment_first/count、used/free/largest_free_block、
  fragment_bytes、object_count、borrow_count、structure_epoch、objects_moved、
  bytes_moved、compact_time_us。
- `GlobalStats`：高水位统计 + `metadata_bytes`——库自有静态对象的总量
  （GlobalState + 维护计划 scratch + 整理建议状态），按 C++ 类型计算。实测与链接器
  看到的真实 `.bss` 相差 ±8 字节。公式与分档表见 `README.md` 的"元数据（静态 RAM）
  预算"一节：**默认 1024 对象约 105 KiB，MCU 上务必下调 `PM_MAX_OBJECTS`。**

## 11. 复杂度与资源

| 操作 | 最坏复杂度 |
|---|---|
| alloc | O(SL bin 链长) —— **与 live 对象数无关**（见下） |
| free | O(1 + 邻块空闲 bin 链长) |
| compact / split | O(objects + moved bytes) + O(objects log objects) 恢复地址序 |
| merge | O(objects + free blocks + moved bytes) |
| validate | O((live + free)²) |
| get_stats | O(free blocks) |
| analyze_compaction | O(objects log objects + free blocks) |

**关于 `alloc`**：早先它把描述符**按地址序插入** live 链，那一步被实测为 ≈100% 的
alloc 成本且随 live 数线性增长。地址序只在维护路径上被需要，因此改为 alloc 做 O(1)
追加、维护入口一次性重建地址序。实测 232 ns @256 → **38 ns**，799 ns @1024 → **38 ns**
（`bench/RESULTS.md`）。**不变量搬迁与其检测边界变化见 `docs/AUDIT_LEDGER.md`**：
alloc 不再检测 live 链损坏，该职责移到每个维护入口与 `validate()`。API 与错误码不变。

固定元数据：描述符表、池表、bins、计划 scratch（s_plan/s_upper/s_barriers/
s_slots）——`global_stats().metadata_bytes` 如实上报；核心库零动态分配、
零异常、零 RTTI。

## 12. 整理建议

见 `docs/COMPACTION_POLICY.md`。要点：`analyze_compaction` 严格只读、
统一结论（含 `INVALID_REQUEST`：输入错误与元数据损坏严格区分）、
阈值可查询可配置、`estimated_moved_*` 为精确估算（以 compact 成功为前提，
仍非事务规划）、绝不自动整理。

**推荐的调用时机**（第九轮指南 §11）：

1. 分配返回 `NoSpace` 后，用同样的 size/alignment 查询一次；
2. 业务已知下一阶段要申请大块时，提前用 `CompactionRequest` 查询；
3. owner 上下文内由低频 monitor 调用 `poll_compaction_advice`，只在
   `changed=true` 时记录/提示（轮询绝不放 ISR，绝不隐式执行 compact）；
4. `COMPACT_RECOMMENDED` → 调用方安排 DMA/ISR/其他任务/外部库静默 → 显式
   `compact` → 失败按状态恢复或重试；
5. `COMPACT_BLOCKED` 先处理借用/状态；`COMPACT_UNLIKELY_TO_HELP` 不要盲目
   整理；`INVALID_METADATA` 进入故障处理；`NO_ACTION` 继续分配。

阈值需要按实际分配失败率、碎片率、整理耗时调优——没有适用于所有产品的
固定百分比。

**Advice 并发边界**（第八轮定案）：analyze/poll/阈值访问器是 owner 上下文
API——Debug 构建有 owner 门控（跨上下文调用即断言诊断，`examples/
owner_probe.cpp` 可复现）；Release 不做运行时检查但契约同样禁止。
