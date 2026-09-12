# PondMerge v1 第八轮审计修复报告（HANDOVER v10）

> 依据《PondMerge v1 最新代码审计与修复指南》（审计基线 `ed94518`）执行。
> 审计账本 `docs/AUDIT_LEDGER.md` 已同步。

## 1. 本轮实际修复（指南 §1 的 8 项）

| 优先级 | 问题 | 决策与修复 | 验证 |
|---|---|---|---|
| P1 | Advice 无运行时 owner 保护 | 采用指南**方案 A（固定 single-owner）**：Debug 构建为整个 advice 族（analyze/poll/thresholds get+set）加 owner 门控——首次调用绑定上下文（`pm_port_context_id`：Host=POSIX 线程 id，ESP32=任务句柄），其他上下文调用在 Debug 下 `pm_debug_abort`（与重复 borrow_end 同类别的调用方 bug 诊断）；Release 无运行时检查、要求调用方遵守契约。生命周期：init()/deinit() 清除绑定；ESP32 owner 任务删除后需重新 init() 再绑定（已文档化） | R34（绑定/持续/重臂生命周期）+ `examples/owner_probe.cpp`（Debug 构建违约被诊断为 ASSERT；Release 构建正常完成并打印说明） |
| P1 | cache/threshold 全局共享无同步 | 方案 A 下 set/get 与 analyze/poll 同属 owner 上下文——阈值更新不可能在分析中途生效；poll 的一次分析使用同一份阈值；阈值在 poll 变化键内（上一轮已加）。头文件与策略文档写明 owner 规则；Debug owner 门控覆盖 set/get | R34 + R33（阈值变化 → changed=true） |
| P1 | analyze 字段来自多个读取阶段 | 单所有者契约下无任何分配器变更可插入分析过程——顺序读取（池计数器 → order 链 → bins via get_stats）构成**一次连贯观察**（代码注释 + 文档明示"这不是无锁并发一致快照，并发监控超出 v1 范围"）；若需并发监控须实现方案 B 快照机制（重设计，未做） | 代码注释 + 契约文档 |
| P2 | JSON 子集边界不明确 | DEMO_REQUIREMENTS §2.1 新增**受限 JSON 子集表**：值类型（十进制整数/字符串/true/false/null）、明确拒绝（数组/嵌套/十六进制/浮点）、容量（行 1024 B、16 pair、key 23、字符串 47）、重复 key first-wins、未知字段明确忽略；host_demo 解析器头部同步注明 | protocol_smoke 新增边界测试（嵌套/数组/十六进制/浮点/负数/超长 key/value 全部 INVALID_REQUEST） |
| P2 | 错误命令应答与"result+snapshot"约定不一致 | 采用指南**方案 2**：DEMO_REQUIREMENTS §2.1 应答分类表——state-changing → result+snapshot；query（advice 附 snapshot、thresholds 仅 result）；parse error → 仅 INVALID_REQUEST result；quit → bye info。protocol_smoke 按类严格验证 | protocol_smoke（95 checks） |
| P2 | 负面测试允许 OK 削弱证明 | 拆分：合法省略可选字段 → 必须 OK 且**验证默认值生效**（flags=0、align 8 → block 112）；非法输入 → 必须 INVALID_REQUEST **且前后场景签名逐项相同**（对象集/摘要/池布局/计数器） | protocol_smoke（15 类拒绝输入 + 场景不变证明 + 重复键 first-wins + 缺省字段） |
| P2 | ESP32 Demo 单向性易误解 | QUICKSTART/DEMO_REQUIREMENTS 明确：Host=交互式；ESP32=**固定脚本场景、只读展示**，设备端整理由固件触发；双向串口命令通道留待后续（含 owner task/背压/输入长度约束的设计要点） | 文档 |
| P3 | cppcheck 口径 | 验收报告改为记录**分类计数**（warning/style/performance = 0、exit code 0、启用类别 warning,style,performance、--error-exitcode=2、"0 告警"仅指启用类别）而非只写 exit 0 | 本轮 §4 实测 |

## 2. 已有测试通过（复验）

基础 1–13、R1–R33、模型对拍、五档门禁、配置矩阵、protocol_smoke——全部
保留并通过。指南 §10 的保留事实逐项维持（alloc 非 O(1)；compact/merge/split
至少对象数+搬移字节；Advice 非最终 compact 规划；UNKNOWN 诚实标注；库不检测
外部持有者）。

## 3. 复杂度与资源证明（指南 §10）

- Advice：walk_order O(objects) + get_stats O(free blocks) + O(1) 判定；
  所有游标范围检查 + 步数上限。
- poll：analyze 成本 + O(1) 缓存比较（18 字段键）。
- 解析器：每行 O(line_length × pair_count)，上界 1024×16。
- owner 门控：O(1) 时间、O(1) 空间（两个标量）。
- 建议 缓存：O(PM_MAX_POOLS) × 88 B。
- demo 协议缓冲：快照行 ≤ ~2 KiB；解析器全固定容量；demo 进程无动态分配。
- owner 门控不在任何锁内（advice 无锁，owner 上下文）；borrow/状态发布
  锁内 O(1) 不变。

## 4. 验收实测

```
Host：
tests/run_host.sh 10000           -> 5,394,278 checks, 0 failures（模型 466,859 checks, 0 failures）
tests/run_host.sh --release 10000 -> 5,394,285 checks, 0 failures
tests/run_host.sh --san 10000     -> 5,394,278 checks, 0 failures
tests/run_host.sh --cppcheck      -> exit 0；warning/style/performance 分类计数均为 0
                                      （--enable=warning,style,performance --error-exitcode=2）
tests/run_host.sh --configs       -> PASSED
g++ -std=c++17 -Wall -Wextra -Werror（core/suite/model/host_demo/owner_probe）-> 干净
python3 examples/protocol_smoke.py -> 95 checks, 0 failures
owner_probe：Debug 构建违约被诊断（PondMerge ASSERT）；Release 构建正常完成
owner_probe：Debug 构建违约被诊断（PondMerge ASSERT）；Release 构建正常完成

ESP32（验收固件，App version `6071008` = HEAD，先提交后烧录）：
suite（基础 13 组 + R1–R34，2000 ops）**1,105,116 checks, 0 failures** PASSED；
双核并发 **28 checks, 0 failures** PASSED；参考模型 **466,859 checks,
0 failures** PASSED。demo 固件（examples/esp32_demo，协议 v1.1）独立
工程构建通过（本轮未重新烧录 demo，场景验证见第六轮记录）。
```

## 5. 调用方责任与未覆盖项

- Advice/alloc/free/resolve/get_stats/validate/maintenance：**单 owner**；
  Debug owner 门控仅覆盖 advice 族（v1 其余 API 的违规靠契约与测试）。
- 库不检测 DMA/ISR/外部持有者；ISR 永不调用 Advice。
- ESP32 demo：固定脚本场景、只读展示；双向控制未实现（设计要点已写明）。
- Release 构建的 advice owner 违约无运行时诊断（契约责任）；v2 可扩展
  owner 门控到全部单 owner API（本轮仅 advice 族，避免扩大改动面）。

## 6. 提交序列

```
<final> feat: advice owner gate (R34) + protocol classification + tightened
        negative tests + docs
```
