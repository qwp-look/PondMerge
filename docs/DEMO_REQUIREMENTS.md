# PondMerge v1 Demo 与快照协议规范

需求来源：`PondMerge v1 使用入口与可视化 Demo 需求文档`（§7–§11）。本文是
落地后的实现规范：协议 v1、数据模型、双端验收。实现位于 `examples/`。

## 1. 组成

| 组件 | 位置 | 说明 |
|---|---|---|
| Demo 进程（Host） | `examples/host_demo.cpp` | 读 stdin 命令 → 驱动公共 API → stdout 输出 JSON Lines |
| Demo 服务/UI | `examples/demo_server.py` | 纯 stdlib HTTP（127.0.0.1）；`--host` 子进程模式 / `--serial` 设备模式 |
| Demo 固件（ESP32） | `examples/esp32_demo/` | 固定 seed 脚本场景，同一协议经 USB-Serial-JTAG 输出 |
| 快照协议 | 本文 §3 | JSON Lines，protocol_version 1 |

技术选型（需求 §12 留白项的落定）：Python 3 stdlib（无第三方依赖）；浏览器
UI 为服务端内联单页（无构建链）；串口协议用 JSON Lines（可读、易解析）。

## 2. 数据模型（协议 v1）

```text
Snapshot {
  t: "snapshot"        protocol: 1
  seq                  单调序号（每次快照 +1）
  source               "HOST" | "ESP32"
  zone_size, segment_size
  pools[]              PoolSnapshot
  objects[]            ObjectSnapshot（全系统存活对象）
  advice[]             每池一条 {pool_id, verdict, borrow_count,
                                has_pinned_objects, caller_must_establish_quiescence}
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

## 2.1 协议 v1.1 变更（第七轮）

- 建议记录的旧字段 `external_quiescence_required` 已更名为
  `caller_must_establish_quiescence`（语义澄清：调用方义务，非库检测）。
- 建议结论新增 `INVALID_REQUEST`（= 6）：调用方输入错误（size=0/对齐非法/
  溢出/超过 FL 上限）与 `INVALID_METADATA`（池损坏）严格区分。
- Demo 进程使用内置的扁平 JSON 解析器（无第三方依赖）：空白容忍、字段顺序
  无关、缺省字段取文档化默认值、类型错误/越界/未知命令一律回结构化
  `INVALID_REQUEST` 结果记录，绝不静默取默认值。
- 协议回归：`examples/protocol_smoke.py`（无浏览器依赖，58 项检查）。

### 2.1 受限 JSON 子集与应答分类（第八轮明确化）

**解析器支持的 JSON 子集**（host_demo 内置解析器，非完整 JSON）：

| 项 | 规则 |
|---|---|
| 值类型 | 十进制整数、字符串、"true"/"false"/"null"（压成 0/1/0） |
| 不支持 | 数组、嵌套对象、十六进制、浮点（解析即整条 INVALID_REQUEST） |
| 容量 | 每行 ≤ 1024 字符；每对象 ≤ 16 个 pair；key ≤ 23 字符；字符串 ≤ 47 字符 |
| 重复 key | **first wins**（明确语义，protocol_smoke 钉住） |
| 未知字段 | 明确**忽略**（不拒绝） |
| 转义 | 仅 `" ` / \n / \t`；其他转义序列拒绝 |

**应答分类**（每类记录数量固定，protocol_smoke 按类验证）：

| 命令类别 | 应答 |
|---|---|
| state-changing（alloc/free/fragment/compact/merge/split，成功或失败） | result + snapshot |
| query（advice / thresholds） | result（thresholds 无 snapshot；advice 附 snapshot） |
| parse error / 非法输入 | 仅 INVALID_REQUEST result，无 snapshot |
| quit | bye info，不再发送 snapshot |

## 3. 串口/管道协议

- Host 模式：命令行（紧凑 JSON，`{"cmd":"alloc","pool":0,"size":120,...}`）
  写入 demo 进程 stdin；record 行读自 stdout。
- ESP32 模式：**固定脚本场景，当前版本只输出快照（display-only）**——
  设备端的 compact/merge/split 由固件场景触发，**不由浏览器命令触发**；
  双向串口命令通道（Host → device JSON 命令行、owner task 内执行、
  背压约束）留待后续版本（需求文档 §8 允许该选择）。
- 命令集：`reset / alloc{pool,size,align,flags} / free{id} / fragment /
  advice{pool,size,align} / compact{pool} / merge{source,target} /
  split{source,segments} / thresholds{ratio,min} / quit`。
- 每个操作命令应答一条 `result`（必含 `status`）+ 一条新 `snapshot`。
  `Busy / PinnedConflict / NoSpace / CorruptMetadata / PoolChanged` 等
  **失败结果原样展示**，Demo 不把"点击整理"设计成无条件成功。

## 4. 状态辨识

UI/服务端必须能区分：

- **设备重启**：出现新的 `ready` info（commit 字段）且 `seq` 重置；
- **旧快照**：`seq` 不前进或 `source` 切换；
- **协议不兼容**：`protocol` != 1 的记录按协议错误入日志，不渲染为当前状态；
- **正常更新**：`seq` 递增、来源一致。

## 5. 交互能力清单（Host UI）

创建/重置场景 · 分配（含 flags 选择：movable/pinned/dma）· 释放指定对象 ·
一键制造碎片 · 输入预期大小/对齐并查询建议 · compact / merge / split ·
静默期模拟（提示性；见 §6 警告）· 刷新快照 · 操作日志与失败原因。

对象着色固定：蓝=MOVABLE、红=PINNED/DMA/EXTERNAL、灰=FREE、黄=SLACK；
选中绿框、维护/错误紫框。颜色不是唯一信息来源——每个对象悬停显示
id/generation/pool/offset/size/block_size/flags/epoch/borrow。

## 6. 安全与生命周期警告（UI 常驻横幅）

普通 `T*` 不跟随搬迁 · borrow 期间整理返回 Busy · 设备端 DMA/ISR 由调用方
停止 · pinned 可能造成整理失败 · `RawRef` 是可伪造低层句柄 ·
`resolve/peek` 指针不等同 RAII borrow · 建议不是保证 · 静默期按钮只是模拟。

## 7. 整理前后对比

`compact` 的对比通过**相邻两条快照**（前、后）按 object_id 联结完成：
pool、address_offset、address_epoch（搬迁时递增）、payload_digest（必须一致）。
generation 不因搬迁改变。

## 8. 验收记录（本轮实测）

### Host

- `examples/host_demo.cpp` + `demo_server.py` 冒烟：alloc(id 6, pinned)、
  fragment（FREE 3 / MOVABLE 3）、advice(4000B)（verdict/can_fit/quiesce
  字段齐备）、compact（FREE 3→1 合并）全部经 HTTP API 往返成功。

### ESP32-S3（demo 固件）

- 固件 `pondmerge_demo.bin`（examples/esp32_demo，独立 IDF 工程，
  EXTRA_COMPONENT_DIRS 复用 esp32/components）。
- 场景脚本（ready 之后）：15 条记录（5 快照 + 8 result + 2 info），
  **0 条坏 JSON**；操作序列 fragment→advice×2→compact_before→compact→
  merge→split→advice 全部 OK。
- 稳定 id：场景删除 id 2/4，新增 id 6；存活对象 payload_digest 跨快照一致。
- 搬迁可见：id 3/5 在 compact/merge 后 offset/pool 变化、epoch 递增、
  摘要不变（id 5 经 merge 换池——演示 local 引用 `PoolChanged` 语义；
  demo 的内部 digest 读取走 cross 引用）。
- 版本证据：`PM_DEMO_COMMIT` 由 CMake 从 git 注入，随每条 info 记录输出。

### 工程质量边界

Demo 不改变核心 API 的单 owner/静默维护契约；无后台整理线程、无中断整理、
核心库无动态内存/异常/RTTI 依赖。ESP32 acceptance suite 与 Host model
differential 的数字分开报告（见 README / HANDOVER）。
