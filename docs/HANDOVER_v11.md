# PondMerge v1 第九轮审计修复报告（HANDOVER v11）

> 依据《PondMerge v1 `e848977` 代码审计与后续修复指示》执行。
> 审计基线：`e848977`（干净 worktree，= origin/main）；最终提交见 git log。

## 1. 本轮实际修复（指南 §1 的 9 项）

| 优先级 | 问题 | 修复 | 验证 |
|---|---|---|---|
| P1 | Host Demo 把超长物理行拆成多次解析，后缀可执行 | `read_frame()` 物理行分帧：payload ≤ 1024 B（LF 不计，CRLF 容忍），超长行**继续消费到 LF 但只产生一次固定拒绝**（op/why 为固定白名单串，不回显用户输入），永不调用 json_parse_flat 或任何 PondMerge API；EOF 无 LF 的最后一行**接受**（已写入协议文档） | protocol_smoke 新增：1024 B 边界、超长前缀+reset 只得到一次拒绝、超长行后合法命令正常执行 |
| P1 | HTTP 无串行化/request correlation；锁包网络写 | `COMMAND_LOCK` 串行化"记序号 → 发送 → 等待对应 result"；STATE_LOCK 只做短拷贝、**永不持有期间写网络**；慢客户端不再阻塞 pump；超时返回 `TIMEOUT` 而非旧 result；进程退出 → `connected=false / degraded=true`；命令 > 1024 B 拒绝 | examples/http_smoke.py（15 项检查：并发关联、seq 单调、慢轮询不阻塞、退出 degraded） |
| P1 | Demo 保存创建时 local ref，merge/split 后按 id 操作失败 | 全部按 id 的操作改用 **cross ref**（`cross_ref(entry)`：index+generation 不变，pool_hint=CROSS_HINT）；当前池从**已审计描述符**读取（`current_pool`）；fragment 按 current_pool 分组、cross ref 释放；摘要读取同策略 | protocol_smoke：merge/split 后 free/digest/fragment 全 OK；伪造/已释放 id 仍 INVALID_REF（生命周期校验未放宽） |
| P1 | ESP32 display-only 仍接受 UI 命令；服务端无协议/重启/旧 seq 校验 | demo_server 实现指南 §6.2 的**接收状态机**：protocol≠1 / source 不匹配 / seq 不前进 / 未知记录 → protocol_error 计数并忽略；ready = 新会话（seq 重置）；进程退出 → connected=false + degraded=true；serial 模式 POST /api/op → `UNSUPPORTED_DISPLAY_ONLY`（不发送） | http_smoke（含 pty 伪串口的 display-only 拒绝测试） |
| P2 | true/false/null 游标被回写、整数词法不明 | 解析器引入 JKind（INT/STR/BOOL/NUL）+ 统一 value_end；整数词法收紧为**无符号十进制、无前导零、≤ UINT32_MAX**（+/-/01/hex/浮点解析即拒绝） | protocol_smoke（bool/null 字段、负数、0x10、浮点用例） |
| P2 | alloc 丢弃 align、flags 截断 | do_alloc 接收并传递 align；缺省 8、**显式非法值（0/非 2 幂/>8）→ INVALID_REQUEST**；flags > UINT16_MAX 拒绝（不再静默截断），未知位由核心 API 校验 | protocol_smoke（align/flags 矩阵） |
| P2 | UI 缺 before/after 对比与静默期模拟 | 服务端 push 状态机对 compact/merge/split 计算**对象级 diff**（MOVED/CREATED/DELETED，含 epoch 递增、generation/digest 不变的**服务端断言**，违规计入 protocol_errors）；UI 渲染 diff 表 + 池生命周期；静默期复选框为纯标注（醒目声明不会停止真实 DMA/ISR）；serial 模式禁用全部变更按钮 | protocol_smoke + http_smoke |
| P2 | Advice 估算错误 + 计数器无审计 | 估算改为**打包模拟精确计算**（与 compact_impl 相同的地址序/屏障/游标规则）：池首空闲块场景从误报 0 修正为 15 对象/15360 B；新增**计数器审计**（used/free/capacity 关系、fragment ≤ free、largest ≤ free−fragment、walk_order 计数 == live_objects，全部条件减法），任一失败 → INVALID_METADATA（不刷新为正常建议、无减法下溢） | R35（精确平铺 + 逐项计数器故障注入 + 修复恢复 + 全程零副作用） |
| P3 | 生命周期/阈值/Demo 启动契约 | thresholds：无参=查询、ratio+min 同现=更新、只出现一个=INVALID_REQUEST；demo seed 任一步失败输出 error 并停止依赖操作；get/set 阈值的未初始化行为随 advice 契约（owner 上下文） | protocol_smoke + 代码审查 |

## 2. 已有测试通过（复验）

基础 1–13、R1–R34、模型对拍、五档门禁、配置矩阵、protocol_smoke——全部
保留并通过。指南 §10/§13 的保留事实逐项维持。

## 3. 复杂度与资源证明（指南 §10/§13）

| 部分 | 证明 |
|---|---|
| 物理分帧 | O(实际行字节)；固定 1025 B 缓冲；超长行不执行后缀 |
| JSON parser | O(L + P×K)，L≤1024、P≤16、游标全程有界；无动态分配 |
| Demo entry | find_entry O(64)；cross_ref O(1)，generation/range 校验不变 |
| Advice | O(objects + free blocks)（单次 walk_order + get_stats + O(1) 判定）；额外空间 O(1)；无减法下溢（条件减法） |
| HTTP | 命令按源串行（COMMAND_LOCK）；STATE_LOCK 不包网络写；响应不引用被覆盖对象 |
| UI diff | O(objects)，仅保留相邻两条快照（prev/current），历史有界 |
| demo 协议缓冲 | 快照行 ≤ ~2 KiB；解析器/分帧全固定容量；demo 进程零动态分配 |
| owner 门控 | O(1) 时间/空间（两个标量），Debug only |

## 4. 验收实测

```
Host（最终提交）：
tests/run_host.sh 10000           -> 5,409,618 checks, 0 failures（模型 466,859 checks, 0 failures）
tests/run_host.sh --release 10000 -> 5,409,625 checks, 0 failures
tests/run_host.sh --san 10000     -> 5,409,618 checks, 0 failures
tests/run_host.sh --cppcheck      -> exit 0；warning/style/performance = 0/0/0
tests/run_host.sh --configs       -> PASSED
g++ -std=c++17 -Wall -Wextra -Werror（core/suite/model/host_demo/owner_probe）-> 干净
python3 examples/protocol_smoke.py -> 95 checks, 0 failures
python3 examples/http_smoke.py     -> 15 checks, 0 failures
owner_probe：Debug = ASSERT 诊断；Release = 正常完成

ESP32-S3（验收固件，App version `baf20e8` = HEAD，先提交后烧录）：
chip model=9 rev=0.2 cores=2 · 串口 /dev/ttyACM0（USB-Serial-JTAG）·
Compile time Sep 12 2026 20:58:59 · ELF SHA256 5c11b505b...
suite（基础 13 组 + R1–R35，2000 ops）：1,120,456 checks, 0 failures PASSED
双核并发：28 checks, 0 failures PASSED
参考模型：466,859 checks, 0 failures PASSED
```

## 5. 调用方责任与未覆盖项（指南 §14 要求的明确清单）

- **双向 ESP32 命令通道未实现**：设备端 demo 仍为固定脚本场景、只读展示；
  双向通道的设计要点（owner task 执行、输入长度/频率限制、背压）已写入
  DEMO_REQUIREMENTS §3。
- **DMA/ISR/外部持有者检测**：库无法实现，调用方责任（架构边界）。
- **普通裸指针自动跟随**：不实现；用 pm_ptr 或 compact 后重新 resolve。
- **Advice 并发一致快照**：v1 采用 owner-gate 方案 A；并发监控的快照机制
  属 v2 重设计。
- **Release 构建的 advice owner 违约**：无运行时诊断（契约责任）；Debug
  构建由 owner gate + owner_probe 覆盖。v1 未把 owner 门控扩展到
  alloc/free/resolve/get_stats/validate（避免扩大改动面；契约与测试已覆盖
  其单所有者语义）。

## 6. 提交序列

```
<final> fix: demo protocol framing/handles/state machine + advice estimate/
        counter audit (R35) + entry docs
```
