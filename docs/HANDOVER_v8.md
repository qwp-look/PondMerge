# PondMerge v1 第六轮交付报告（使用入口 / 整理建议 / 可视化 Demo）

> 依据《PondMerge v1 使用入口与可视化 Demo 需求文档》执行。审查基线：
> `08aa039`。本文记录全部交付物与验收证据。

## 1. 交付物对照（需求 §3）

| 交付物 | 状态 | 位置 |
|---|---|---|
| `QUICKSTART.md` | ✅ | 仓库根；最小示例 → compact 全流程 → 常见错误表 |
| `docs/USAGE_GUIDE.md` | ✅ | 三区模型/生命周期/标志/引用语义/访问限制/维护/并发/错误码/复杂度 |
| `docs/COMPACTION_POLICY.md` | ✅ | 建议语义、五值结论、阈值（搁浅空闲字节）、标准整理流程 |
| `docs/DEMO_REQUIREMENTS.md` | ✅ | 快照协议 v1、数据模型、状态辨识、验收记录 |
| Host Demo | ✅ | `examples/host_demo.cpp` + `examples/demo_server.py`（stdlib，无第三方依赖） |
| ESP32 Demo | ✅ | `examples/esp32_demo/`（独立 IDF 工程，固定 seed 脚本场景） |
| 快照协议说明 | ✅ | DEMO_REQUIREMENTS §2–§4（协议 v1） |
| 整理建议能力 | ✅ | `analyze_compaction` / `poll_compaction_advice` / 阈值（核心 API）+ R31 |

## 2. 整理建议能力（需求 §6）

- **严格只读**：R31(1) 对碎裂池做 Pool/ObjectDesc/Auto Zone 逐字节快照比对，
  analyze/poll 前后零变化；validate 通过。建议缓存是独立的"建议态"固定存储，
  不属于分配器元数据。
- **统一五值结论**：NO_ACTION / COMPACT_RECOMMENDED / COMPACT_BLOCKED /
  COMPACT_UNLIKELY_TO_HELP / INVALID_METADATA；诊断字段齐备（pool_state、
  borrow_count、has_pinned、can_fit_now/after、external_quiescence_required…）。
- **预期申请**：传入 size/align 时按 alloc 的同一圆整规则计算 need，
  `request_can_fit_now`（最大连续块）与
  `request_can_fit_after_compaction_estimate`（free − fragment，估算值，
  pinned 屏障可能更小——文档明示）分别判定。
- **诚实估算**：仅"完全 packed"时给出精确 0，其余一律
  `COMPACTION_ESTIMATE_UNKNOWN`，不伪造精度。
- **阈值可查询/可配置**：`get/set_compaction_thresholds`（默认 100‰ / 512 B），
  作用于**搁浅空闲字节**（free − fragment − largest；需求 §12 留白的默认值
  在此落定并文档化）；R31(6) 验证阈值改变建议、恢复后行为一致。
- **自动提示**：`poll_compaction_advice` 按 verdict/epoch/borrow/largest/
  fragment/请求参数抑制重复提示（R31(6) 三个状态变化场景）；轮询即提示、
  停止轮询即关闭；**绝不自动执行 compact**、不可在中断中调用（文档约束）。

## 3. Demo（需求 §7–§9）

- **协议 v1**（JSON Lines）：Snapshot/Result/Info 三类记录，字段见
  DEMO_REQUIREMENTS §2；每个操作应答 result（必含 status）+ 新 snapshot。
- **Host**：`demo_server.py --host` 以子进程驱动 `host_demo`；浏览器单页 UI
  展示池泳道（蓝/红/灰/黄 + 悬停明细）、对象表、建议面板、操作日志；
  HTTP API：GET /api/state、POST /api/op。冒烟：alloc(pinned)、fragment、
  advice、compact 全部经 HTTP 往返验证，块布局合并正确（compact 后 FREE 3→1）。
- **ESP32**：`examples/esp32_demo` 独立 IDF 工程（EXTRA_COMPONENT_DIRS 复用
  组件）；固定 seed 脚本场景输出同一协议。实测：ready 后 15 条记录
  （5 快照 + 8 result + 2 info），0 条坏 JSON；操作全 OK；稳定 id 生存、
  payload 摘要跨 compact/merge/split 一致、搬迁对象（id 3/5）的
  offset/pool/epoch 变化可见；`PM_DEMO_COMMIT` 由 CMake 注入并随 info 输出。
- **Demo 语义教学**：id 5 经 merge 换池后，local 引用解析为 `PoolChanged`——
  demo 的内部 digest 读取改走 cross 引用（库语义的正确展示，见 §5 修正记录）。

## 4. 修正记录（本轮发现并当场修正，均为 Demo/测试自身问题）

1. host_demo：JSON 双逗号、digest 非法十六进制字面量、reset 未先释放存活对象、
   fragment 循环只释放首个对象。
2. R31：两处对象泄漏/重复释放、场景漏算池尾大空闲块（threshold 语义因此
   修正为"搁浅空闲字节"——库判定逻辑本身正确，探针证实）。
3. poll_compaction_advice：抑制比较必须取**分析前**的缓存快照（否则永远
   相等）——实现 bug，已修并补三场景测试。
4. cppcheck：R31(5) 的 bin 索引在 CHECK 失败时可能越界（真实缺陷，加守卫）。

## 5. 验收（需求 §11）

Host（最终提交）：

```
tests/run_host.sh 10000           -> 5,394,174 checks, 0 failures（模型 466,859 checks, 0 failures）
tests/run_host.sh --release 10000 -> 5,394,181 checks, 0 failures
tests/run_host.sh --san 10000     -> 5,394,174 checks, 0 failures
tests/run_host.sh --cppcheck      -> exit 0，0 告警
tests/run_host.sh --configs       -> PASSED
```

ESP32：验收固件（App version = 最终提交，含 R31）重新构建烧录，实测记录见
README 设备小节；demo 固件独立构建烧录验证后，设备恢复验收固件。

## 6. 需求 §12 留白项的落定

- Host 辅助服务：Python 3 stdlib；前端：服务端内联单页（无框架）。
- 串口协议：JSON Lines；协议版本 1。
- 阈值默认：fragment_ratio_permille=100、fragment_min_bytes=512（可配置）。
- 枚举命名：`CompactionVerdict::{NO_ACTION, COMPACT_RECOMMENDED,
  COMPACT_BLOCKED, COMPACT_UNLIKELY_TO_HELP, INVALID_METADATA}`。
- Demo 与测试 runner 不合并；设备端 model differential 已在上一轮完成
  （见 HANDOVER_v7），demo 固件不包含模型（保持 demo 轻量）。
