# PondMerge v1 整理建议与整理策略

本文回答三个问题：**何时查询整理建议**、**如何解释建议结果**、**如何执行整理**。
建议能力由 `analyze_compaction` / `poll_compaction_advice` 提供（round-6 需求
文档 §6），与 `compact()` 的事务规划完全解耦。

## 1. 建议是什么、不是什么

`analyze_compaction(pool, expected)` 是**严格只读**分析：不移动对象、不改池
状态、不改 generation/epoch、不隐式暂停池（R31 用逐字节快照钉住）。它回答：

```text
现在是否值得尝试整理？
接下来申请 size X / align A 的对象，当前连续空间是否可能不足？
如果现在不能整理，是因为借用、池状态，还是需要外部静默期？
```

它**不能**：

- 保证整理一定成功、或整理后一定放得下（估算字段可能为 UNKNOWN）；
- 发现 DMA/ISR/外部库持有的裸地址，或对象内部的裸指针；
- 预测未来的分配模式；
- 替代 `compact()` 的最终事务规划（那才是唯一权威判定）。

执行 `compact()` 前仍必须走正式维护契约：状态、借用、外部静默期、返回值。

## 2. 统一结论（五值）

| verdict | 含义 | 调用方动作 |
|---|---|---|
| `NO_ACTION` | 当前无需整理（ packed、或预期申请现在就放得下） | 直接分配 |
| `COMPACT_RECOMMENDED` | 碎片达标，或预期申请"现在放不下但整理后大概率放得下" | 安排静默期 → `compact()` → 重试分配 |
| `COMPACT_BLOCKED` | 有活跃借用（`borrow_count`）或池不在 Running/Paused（`pool_state`） | 排除阻挡来源后再查询 |
| `COMPACT_UNLIKELY_TO_HELP` | 即使整理成功也放不下预期申请（总空闲不足） | 不要整理；换池 / 换大小 / 扩容设计 |
| `INVALID_METADATA` | 池 id 无效，或有界审计发现损坏 | 按 CorruptMetadata 处理（validate / 复位） |
| `INVALID_REQUEST` | **调用方输入错误**（size=0、对齐非法、溢出、超过 FL 上限）——与元数据损坏严格区分，不污染建议缓存 | 修正请求参数后重试（第七轮指南 §3） |

调用方只需读 `verdict` 一个字段即可决策；诊断字段供日志、Demo 与人工分析。

## 3. 关键诊断字段

| 字段 | 含义 |
|---|---|
| `pool_state` / `borrow_count` | BLOCKED 的来源（维护态 vs 借用） |
| `capacity / used_bytes / free_bytes` | 字节账目（used + free = capacity） |
| `largest_free_block` | 最大连续空闲块（0 且 `stats_valid==0` = 拒绝报告） |
| `fragment_bytes` | 小于最小块、无法独立利用的 slack |
| `fragment_ratio_permille` | fragment × 1000 / capacity |
| `live_objects` / `has_pinned_objects` | 对象数；pinned 屏障存在标记 |
| `request_can_fit_now` | `largest_free_block >= need`（need 按 alloc 的圆整规则计算） |
| `request_can_fit_after_compaction_estimate` | **估算**：`(free - fragment) >= need`；pinned 屏障可能使实际值更小 |
| `caller_must_establish_quiescence` | 1 = 执行 compact 前**调用方**必须自行建立并排空外部静默期。这只是义务提醒：**0 不代表库已验证没有外部使用者**——PondMerge 永远不会检测 DMA/ISR/外部库 |
| `estimated_moved_objects/bytes` | **精确估算**（第九轮）：对已审计的地址序 live 链模拟 compact 的打包规则（同一游标/屏障语义），给出确定会搬迁的对象数与字节数——以"compact 成功"为前提，仍不是最终 compact 事务规划 |

## 4. 阈值

```cpp
pm::CompactionThresholds t = pm::get_compaction_thresholds();
// 默认：fragment_ratio_permille = 100（10%）、fragment_min_bytes = 512
pm::set_compaction_thresholds({200, 1024});   // 按产品负载调整
```

阈值作用于**搁浅空闲字节**（stranded = free − fragment − largest_free_block），
即"卡在次级洞里、整理可以合并"的部分。注意 `fragment_bytes` 本身只在打包后
的布局里出现，正常碎裂（多个洞）时它接近 0——所以阈值不能只看它。

判定：stranded ≥ min_bytes **且** stranded×1000/capacity ≥ ratio → 视为
"碎片化"，通用建议给 `COMPACT_RECOMMENDED`。阈值可查询、可配置，没有写死的
通用百分比。

**计数器审计**（第九轮）：判定算术运行前先验证
`used ≤ capacity`、`free ≤ capacity`、`used + free == capacity`、
`fragment ≤ free`、`largest ≤ free − fragment`、walk_order 的 live 计数等于
`live_objects`——全部使用条件减法（uint64），任一失败即 `INVALID_METADATA`
（R35 故障注入覆盖）。这意味着 Advice 能发现统计字段损坏，且**永不发生
减法下溢**。

## 5. 自动提示（可选，不自动执行）

两种受支持的方式：

1. **同步更新**：每次 `analyze_compaction` 都刷新该池的"最近建议"缓存
   （独立固定存储，不是分配器元数据）。
2. **轮询**：监控循环里调用 `poll_compaction_advice(pool, &req, &changed)`
   （owner 上下文、低频、只读；绝不在 ISR 中调用）；
   当 verdict、structure_epoch、borrow_count、largest_free_block、
   fragment_bytes、请求参数**全部不变**时 `changed == false`，实现重复提示
   抑制。首次查询总是报告。

约束（需求文档 §6.5）：不在中断里做复杂分析；回调/轮询**绝不**自动执行
`compact()`；调用方停止轮询即关闭提示；提示失败不影响分配器状态。
抑制键（第七轮补全）：verdict、pool_state、structure_epoch、borrow_count、
used/free/live、largest_free_block、fragment_bytes、pinned 存在、stats_valid、
请求 size/align/flags/tag 以及**阈值**——任一变化即再次提示；`INVALID_REQUEST`
每次都报告（调用方错误不缓存、不抑制）。

## 6. 执行整理的标准流程

```cpp
pm::CompactionRequest req{expected_size, expected_align, flags, tag};
pm::CompactionAdvice a = pm::analyze_compaction(pool, &req);

switch (a.verdict) {
case pm::CompactionVerdict::COMPACT_RECOMMENDED:
    // 1) 停止 DMA / ISR / 其他任务 / 外部库（调用方义务）
    // 2) 确认无活跃借用（有则 compact 返回 Busy 且留在 Paused）
    if (pm::compact(pool) == pm::Status::Ok) { /* 重试分配 */ }
    else { pm::resume(pool); }        // 失败时池在 Paused，由调用方恢复
    break;
case pm::CompactionVerdict::COMPACT_BLOCKED:
    /* 排除 borrow_count / pool_state 指出的阻挡 */
    break;
case pm::CompactionVerdict::COMPACT_UNLIKELY_TO_HELP:
    /* 总空闲不足：整理无济于事 */
    break;
case pm::CompactionVerdict::INVALID_METADATA:
    /* validate() 确认；按损坏处理 */
    break;
case pm::CompactionVerdict::NO_ACTION:
    /* 直接分配 */
    break;
}
pm::validate(pool);  // 整理后可选的结构审计
```

## 7. pinned 对象与整理

pinned/DMA/external 对象是搬移屏障：整理会在其两侧分别打包。屏障过密或
屏障间剩余空间不足时，`compact` 可能 `PinnedConflict`/`NoSpace` 失败——
建议层不预测 pinned 布局的细节，`has_pinned_objects` 只是提示。设计上应
控制 pinned 对象的数量与聚拢摆放。
