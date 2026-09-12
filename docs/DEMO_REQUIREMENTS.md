# PondMerge v1 Demo 与快照协议规范

需求来源：《PondMerge v1 使用入口与可视化 Demo 需求文档》（§7–§11）与
第九轮审计指南。本文是**落地后的实现规范**：协议 v1.1、受限 JSON 子集、
接收状态机、双端组成与验收。实现位于 `examples/`。

## 1. 组成与运行

| 组件 | 位置 | 说明 |
|---|---|---|
| Demo 进程（Host） | `examples/host_demo.cpp` | 读 stdin 命令 → 驱动公共 API → stdout 输出 JSON Lines |
| Demo 服务/UI | `examples/demo_server.py` | 纯 stdlib HTTP（仅绑定 127.0.0.1）；`--host` 子进程模式 / `--serial` 设备模式 |
| Demo 固件（ESP32） | `examples/esp32_demo/` | 独立 IDF 工程；固定脚本场景，同一协议经 USB-Serial-JTAG 输出（只读展示） |
| 协议回归 | `examples/protocol_smoke.py` | 95 项检查，无浏览器依赖 |
| HTTP 层回归 | `examples/http_smoke.py` | 15 项检查（并发关联、seq 单调、degraded、display-only 拒绝） |

```sh
# Host Demo（浏览器 UI + 协议回归）
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude -Isrc \
    examples/host_demo.cpp src/core.cpp -o build/host_demo
python3 examples/protocol_smoke.py build/host_demo
python3 examples/demo_server.py --host build/host_demo --port 8080
# 浏览器打开 http://127.0.0.1:8080

# ESP32 Demo（独立固件；固定脚本场景，只读展示）
cd examples/esp32_demo && idf.py set-target esp32s3 && idf.py -B build build
python3 examples/demo_server.py --serial /dev/ttyACM0 --port 8080   # 仅展示
```

技术选型（需求 §12 留白项的落定）：Python 3 stdlib（无第三方依赖）；浏览器
UI 为服务端内联单页（无构建链）；协议为 JSON Lines（可读、易解析）。

## 2. 数据模型（协议 v1.1）

```text
Snapshot {
  t: "snapshot"        protocol: 1
  seq                  单调序号（每次快照 +1）
  source               "HOST" | "ESP32"
  zone_size, segment_size
  pools[]              PoolSnapshot
  objects[]            ObjectSnapshot（全系统存活对象）
  advice[]             每池一条 {pool_id, verdict, borrow_count,
                                has_pinned_objects,
                                caller_must_establish_quiescence}
}

PoolSnapshot {
  pool_id, state, segment_first, segment_count, capacity,
  used_bytes, free_bytes, largest_free_block, fragment_bytes,
  live_objects, borrow_count, structure_epoch,
  blocks[]             BlockSnapshot，按地址偏移升序
}

BlockSnapshot {
  offset, size,
  kind: "MOVABLE" | "PINNED" | "FREE" | "SLACK",
  object_id / generation / address_epoch   （仅 MOVABLE/PINNED）
}

ObjectSnapshot {
  object_id, generation, pool_id, address_offset, size, block_size,
  address_epoch, flags, payload_digest    // FNV-1a，跨快照比对内容一致性
}

Result { t:"result", op, status, ...操作专属字段 }
Info   { t:"info", event, detail, commit }
```

约定：快照是**只读副本**；UI 不得直接修改分配器元数据。所有状态变化必须来自
新的真实快照，前端动画不得伪造终态。块布局经 `src/internal.h` 白盒枚举
（公共 API 不暴露空闲链——Demo 属诊断工具，此为有意豁免）。

### 2.1 协议 v1.1 相对 v1.0 的变更

- 建议记录的旧字段 `external_quiescence_required` 更名为
  `caller_must_establish_quiescence`（语义澄清：调用方义务，非库检测；
  0 不代表库已验证没有外部使用者）。
- 建议结论新增 `INVALID_REQUEST`（= 6）：调用方输入错误（size=0 / 对齐非法 /
  溢出 / 超过 FL 上限）与 `INVALID_METADATA`（池损坏）严格区分。

### 2.2 受限 JSON 子集（命令行解析器）

Demo 进程内置扁平 JSON 解析器（固定容量、无动态分配）——这是**受限子集**，
不是完整 JSON：

| 项 | 规则 |
|---|---|
| 值类型 | 无符号十进制整数、字符串、"true"/"false"/"null"（折叠为 1/0/0，且数值字段只接受整数 token） |
| 明确拒绝 | 负数与前导零（"01"）、数组、嵌套对象、十六进制、浮点、未知转义 → 整条 INVALID_REQUEST |
| 容量 | 物理行 payload ≤ 1024 B（见 §3）；每对象 ≤ 16 个 pair；key ≤ 23 字符；字符串 ≤ 47 字符 |
| 重复 key | **first wins**（protocol_smoke 钉住） |
| 未知字段 | 明确**忽略**（不拒绝） |
| 转义 | 仅 `\"`、`\\`、`\/`、`\n`、`\t`；其他转义序列拒绝 |
| 必填/可选 | `alloc` 的 pool/size 必填，align 缺省 8、flags 缺省 0；显式非法值（0/非 2 幂/>8 的对齐、>UINT16_MAX 的 flags）→ INVALID_REQUEST，绝不静默改默认 |

### 2.3 应答分类（每类记录数量固定）

| 命令类别 | 应答 |
|---|---|
| state-changing（alloc/free/fragment/compact/merge/split，成功或失败） | result（必含 status）+ snapshot |
| query（advice 附 snapshot；thresholds 仅 result） | result |
| parse error / 非法输入（含超长行） | 仅 INVALID_REQUEST result，无 snapshot |
| quit | bye info，之后进程退出 |

失败结果（`Busy` / `PinnedConflict` / `NoSpace` / `CorruptMetadata` /
`PoolChanged` …）**原样展示**——Demo 不把“点击整理”设计成无条件成功。

## 3. 输入分帧与命令集

### 3.1 物理行分帧（第九轮）

- 一帧 = 一行；payload（不含 LF，容忍 CRLF）≤ **1024 字节**。
- 超长行：**继续消费到 LF 但只产生一次固定拒绝**（`op="line"`），其内容
  永不进入解析器——超长行中拼接的合法命令后缀不会被执行。
- EOF 时若无未消费字节 → 正常退出；若有未含 LF 的末行 → **接受**该行
  （交互式使用友好；本条为协议文档的明确选择）。
- 不变量：一次读取消耗且只消耗一个物理行；被标记为超长的行不会调用任何
  PondMerge API。

### 3.2 命令集

`reset` · `alloc{pool,size[,align,flags]}` · `free{id}` · `fragment` ·
`advice{pool[,size,align]}`（size=0 或缺省 = 通用建议）·
`compact{pool}` · `merge{source,target}` · `split{source,segments}` ·
`thresholds[{ratio,min}]`（无参查询；两者同现才是更新）· `quit`。

## 4. 接收状态机（服务端）

`demo_server` 对每条收到的记录执行（round-9 实现指南 §6.2）：

```text
未知记录类型 / protocol != 1      -> protocol_errors++，忽略
info.event == "ready"             -> 开启新会话（seq 重置、connected=true）
snapshot.source != 当前源          -> source_mismatch，忽略
snapshot.seq <= last_seq          -> stale_snapshot，忽略
result                            -> 更新 last_result / 日志
JSON 坏行                          -> bad_json 计数
```

- 任何协议错误进入 `protocol_errors` 计数与日志（UI 可见），**不会污染**
  当前快照。
- Demo 进程退出（stdin EOF）→ `connected=false`、`degraded=true`；
  串口断开同语义。UI/服务端由此可区分：设备重启（ready 新会话）、旧快照
  （seq 不前进）、协议不兼容（protocol_errors 增长）与正常更新。

## 5. 对象句柄与整理前后对比

- Demo 的对象身份 = `index + generation`；**创建时的 pool hint 不是永久归属**
  ——merge/split 会移动对象。所有按 id 的操作（free、payload 摘要）与
  fragment 的池分组都使用 **cross ref**（`pool_hint = CROSS_HINT`）与
  **描述符中的当前 pool_id**，因此 merge/split 后按稳定 id 释放/读取仍然
  成功；伪造 id 或已释放 id 仍被完整校验拒绝（INVALID_REF）。
- 整理前后对比：服务端对 compact/merge/split 后的相邻两条**已接受快照**做
  对象级 diff——MOVED（pool/offset 变化，且服务端断言 epoch 严格递增、
  generation 与 payload_digest 不变，违规计入 protocol_errors）、CREATED、
  DELETED，以及池生命周期变化（源池消失 / 新池出现）。UI 渲染
  “旧 pool/offset → 新 pool/offset”对比表。

## 6. 交互能力清单（Host UI）

创建/重置场景 · 分配（含 flags 选择：movable/pinned/dma、align 输入）·
释放指定对象 · 一键制造碎片 · 输入预期大小/对齐并查询建议（含搬迁估算）·
compact / merge / split · 静默期模拟复选框（**纯标注**，见 §8）· 刷新快照 ·
操作日志、失败原因与协议错误计数 · 整理前后对比表。

对象着色固定：蓝=MOVABLE、红=PINNED/DMA/EXTERNAL、灰=FREE、黄=SLACK。
颜色不是唯一信息来源——每个对象悬停显示 id/generation/pool/offset/size/
block_size/flags/epoch。ESP32（display-only）模式下所有变更按钮禁用并显示
说明横幅。

## 7. 安全与生命周期警告（UI 常驻横幅）

普通 `T*` 不跟随搬迁 · borrow 期间整理返回 Busy · 设备端 DMA/ISR 由调用方
停止 · pinned 可能造成整理失败 · `RawRef` 是可伪造低层句柄 ·
`resolve/peek` 指针不等同 RAII borrow · 建议不是保证（估算以 compact 成功
为前提）· 静默期复选框只是声明，库无法看见，也不会停止真实外部访问者。

## 8. 验收记录

### Host

- `protocol_smoke.py`：**95 项检查全部通过**——协议合法性、record 类型、
  status、seq 单调、object_id 稳定、generation 搬迁不变、epoch 搬迁递增、
  payload_digest 一致、行分帧（边界/超长/CRLF/EOF）、解析器边界（嵌套/数组/
  十六进制/浮点/负数/重复键/超长 key/value）、15 类非法输入的
  INVALID_REQUEST 与前后场景签名不变。
- `http_smoke.py`：**15 项检查全部通过**——并发 POST 无响应错配、seq 单调、
  维护 diff 不变量、慢轮询不阻塞 pump、demo 进程退出 → degraded、
  display-only 拒绝（pty 伪串口）。

### ESP32-S3（demo 固件）

- 固件 `pondmerge_demo.bin`（独立 IDF 工程，`EXTRA_COMPONENT_DIRS` 复用
  esp32/components；`PM_DEMO_COMMIT` 由 CMake 从 git 注入并随 info 记录输出）。
- 场景脚本（ready 之后）：15 条记录（5 快照 + 8 result + 2 info），
  **0 条坏 JSON**；操作序列 fragment → advice×2 → compact_before → compact →
  merge → split → advice 全部 OK。
- 稳定 id：场景删除 id 2/4，新增 id 6；存活对象 payload_digest 跨快照一致。
- 搬迁可见：id 3/5 在 compact/merge 后 offset/pool 变化、epoch 递增、摘要
  不变（id 5 经 merge 换池——演示 local 引用 `PoolChanged` 语义；demo 的
  内部 digest 读取走 cross 引用）。

### 工程质量边界

Demo 不改变核心 API 的单 owner/静默维护契约；无后台整理线程、无中断整理、
核心库无动态内存/异常/RTTI 依赖。ESP32 acceptance suite 与 Host model
differential 的数字分开报告（见 README / HANDOVER_v11）。
