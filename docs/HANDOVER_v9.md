# PondMerge v1 第七轮审计修复报告（HANDOVER v9）

> 依据《PondMerge v1 当前代码审计与修复指南》（审计基线 `ebb3170`）执行。
> 最终提交见 git log；审计账本 `docs/AUDIT_LEDGER.md` 已同步。

## 1. 本轮实际修复（指南 §1 的 8 项）

| 优先级 | 问题 | 修复 | 验证 |
|---|---|---|---|
| P1 | Advice 非法请求伪装成元数据损坏 | 新增 `CompactionVerdict::INVALID_REQUEST`（第六值）；请求校验前置到任何状态读取之前；非法请求**不触碰建议缓存**（有效缓存不被输入错误污染） | R32（5 类非法请求 + 损坏路径区分 + 逐字节零副作用 + 缓存未污染证明） |
| P1 | Advice 并发边界未写入公共 API | `analyze_compaction`/`poll_compaction_advice`/阈值访问器的头文件契约：**owner 上下文 API、禁 ISR、禁与 alloc/free/maintenance 并发、非并发一致快照**；复杂度 O(objects + free blocks)、O(1) 额外内存 | 文档三方一致（header/README/账本）；双核设备测试继续覆盖 borrow 侧 |
| P1 | Advice 读取非一致观察点 | 采用指南给出的**owner-gate 方案**（而非快照机制）：单所有者契约下 order 链/bins/计数的先后读取构成一次连贯观察；契约明示"不承诺并发一致快照" | 文档；所有游标保持范围检查 + 步数上限 |
| P2 | poll 变化键不完整 | 键补全至全部输入：verdict、pool_state、structure_epoch、borrow_count、used/free/live、largest、fragment、pinned 存在、stats_valid、请求 size/align/**flags/tag**、**阈值** | R33（首次报告、不变抑制、compact/borrow/阈值/pinned/请求 echo/损坏与恢复各场景） |
| P2 | `external_quiescence_required` 易误读 | 全链更名为 **`caller_must_establish_quiescence`**（API/JSON/UI/Quickstart/策略文档），并注明"0 不代表库已验证外部安全" | grep 零残留（历史 HANDOVER 除外） |
| P2 | Demo sscanf 解析不健壮 | host_demo 内置**扁平 JSON 解析器**（无第三方依赖、无动态分配）：空白容忍、字段顺序无关、可选字段文档化默认值、类型错误/越界/未知命令一律结构化 `INVALID_REQUEST` | protocol_smoke 的 9 类畸形输入全覆盖 |
| P3 | UI innerHTML 拼接 | 全部动态文本改 `createElement`+`textContent`（tooltip 走 title 属性）；继续仅绑定 127.0.0.1 | 代码审查 |
| P3 | 缺协议自动化回归 | 新增 `examples/protocol_smoke.py`：无浏览器依赖，58 项检查（JSON 合法性、record 类型、status、seq 单调、id 稳定、generation 不变、epoch 搬迁递增、digest 一致、畸形输入结构化拒绝、失败操作不破坏场景） | 本机通过 |

## 2. 已有测试通过（复验）

基础 1–13、R1–R31、模型对拍、五档门禁、配置矩阵——全部保留并通过
（数字见 §4）。指南 §11 的保留清单（alloc 非 O(1)、free 验证先于回调、
规划失败先于首条 memmove、pinned 不动、generation/epoch 语义、遍历防环）
逐项核对无回归。

## 3. 新增证明（指南 §11 模板）

- `analyze_compaction` 最坏循环：walk_order O(objects) + get_stats
  O(free blocks) + O(1) 判定；步数上限遍历。
- `poll` = analyze + O(1) 缓存比较（比较键 18 字段）。
- 建议缓存空间：O(PM_MAX_POOLS) × 88 B（host 16×88 ≈ 1.4 KiB，设备 256×… 按
  PM_MAX_POOLS 线性）。
- INVALID_REQUEST 路径 O(1)、零副作用（R32 快照）、缓存零污染。
- demo 协议缓冲：单行快照 ≤ ~2 KiB，demo 进程无动态分配增长
  （JSON 解析器全固定容量）。
- 锁内工作量：advice 无锁（owner 上下文）；borrow/状态发布锁内 O(1)。

## 4. 验收实测（指南 §13）

```
Host（最终提交）：
tests/run_host.sh 10000           -> 5,394,268 checks, 0 failures（模型 466,859 checks, 0 failures）
tests/run_host.sh --release 10000 -> 5,394,275 checks, 0 failures
tests/run_host.sh --san 10000     -> 5,394,268 checks, 0 failures
tests/run_host.sh --cppcheck      -> exit 0，0 告警
tests/run_host.sh --configs       -> PASSED
g++ -std=c++17 -Wall -Wextra -Werror（core/suite/model/host_demo）-> 干净
python3 examples/protocol_smoke.py -> 58 checks, 0 failures

ESP32（验收固件，App version `006682f` = HEAD，ELF SHA 对应本地产物）：
suite（基础 13 组 + R1–R33，2000 ops）**1,105,106 checks, 0 failures** PASSED；
双核并发 **28 checks, 0 failures** PASSED；参考模型 **466,859 checks,
0 failures** PASSED。demo 固件（协议 v1.1，改名后重建）构建烧录验证通过
（READY 后 15 条记录、0 坏 JSON、8 项操作全 OK、摘要跨场景一致）。
```

## 5. 调用方责任与未覆盖项

- Advice 为 owner 上下文 API：监控任务轮询必须与 owner 串行（指南 §4/§5 的
  快照机制属 v2 重设计，未实现——已文档化为边界）。
- 库不检测 DMA/ISR/外部使用者；`caller_must_establish_quiescence` 仅是义务
  提醒。
- ESP32 demo 为脚本场景（交互命令留待后续）；demo 固件与验收固件分开构建
  分开报告。
- UI 已转义，但 demo 服务仅绑定本机、无鉴权——仅用于开发演示。

## 6. 调试过程记录（对下一轮有价值）

1. demo"挂起"三连的根因链：① 旧解析器 bytes/str 不匹配（异常被吞）→
   ② `do_fragment` 经 `do_alloc` 双重发射导致协议错位 → ③ smoke 的
   malformed 段读取纪律错误。修复后加 **drain（select 非阻塞排空）** 使
   客户端对遗留记录免疫。
2. 排查手段教训：`pkill -f X` 会匹配包含 X 的当前 shell（自自杀）——清理
   必须独立成命令并用 `[x]` 括号技巧；`g++ | head` 会因 SIGPIPE 吞掉编译
   结果导致陈旧二进制。
