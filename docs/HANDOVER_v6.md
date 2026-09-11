# PondMerge v1 第四轮长程审查收口报告（HANDOVER v6）

> 依据《PondMerge v1 长程审查、自修复与验收任务书》执行。
> 审查基线：`67eae49`（第三轮收口）；最终提交：`801b8c3`。
> 审计账本：`docs/AUDIT_LEDGER.md`（本轮建立，长期维护）。

## 1. 接手与基线（任务书 §2/§3）

- 接手状态：HEAD `67eae49`，工作区干净，五档门禁与设备验证全绿，ESP32 空闲。
- 基线（`67eae49`）：Debug/Release/SAN 10000 ops = 5,393,756 / 5,393,763 /
  5,393,756 checks，全部 0 failures；cppcheck exit 0；configs PASSED。
- 未使用 reset/checkout 丢弃任何前任修改。

## 2. 风险 → 函数 → 测试 → 证明

| # | 风险（任务书依据） | 修复函数 | 测试 | 证明 |
|---|---|---|---|---|
| 1 | alloc 的地址序插入无防环上限，环链挂死（§7："validate 的防环不能替代 alloc 自己的防环"） | `order_insert_sorted()` 改为有界走链并返回 bool；`alloc()` 重排为**先链入 order 链、后改块字节与 bins**，失败只回滚槽位 | R29(5) | 红测探针：旧 core 上 alloc 在自环链上永久挂死（timeout 杀进程，健康尾块可用）；新实现步数上限在触碰任何链接前触发 → 有界 CorruptMetadata，槽位链守恒 |
| 2 | alloc 把损坏 free-list 静默报成 NoSpace（§11） | `alloc()` bins_find 失败路径调用 `audit_pool_bins()` 区分 | R18(c) 更新（注明加强理由） | 红测探针：旧 core 返回 NO_SPACE；新实现 CorruptMetadata；真耗尽仍 NoSpace（R29 健康分配 + 模型可分配性预言机不变） |
| 3 | get_stats 的"0"歧义（HANDOVER_v4 T3；§11 可区分错误） | `PoolStats.valid` 字段；拒绝路径置 0 | R18(a)(b) 扩展 | healthy → valid=1 且 largest>0；环链/越界 → valid=0 且 largest=0 |
| 4 | alloc 复杂度声明漏掉地址序插入的 O(live)（§12） | 文档（README + pondmerge.hpp） | 账本 §5 | 声明改为 O(SL bin 链长 + live_objects)，与 `bins_find` + `order_insert_sorted` 两个真实循环对应 |
| 5 | finalize_layout 执行段无显式上限（§7 全量搜索） | Debug 步数断言 | — | 前置已审计/刚重建，断言为不可能性守卫；计划 A 无失败分支 |

新增 R29 扩展故障矩阵（§11 覆盖补全，全部零副作用断言）：
(1) 互逆链接损坏 → `free_block_binned` 拒绝且不跑回调；
(2) 跨 bin 重复成员 → validate 的 size-class 检查拒绝；
(3) segment 字段损坏 → validate 与 compact（搬移前）拒绝；
(4) 运行时状态字段损坏（state/borrow_count/active_borrows）→ 全入口安全拒绝、修复后恢复；
(5) 环链上有界 alloc。

## 3. 红测证据（对 67eae49 的旧 core）

- 探针 1：损坏 alloc 搜索路径上的 bin head（0xFFFFFFF0）→ 旧 alloc 返回
  **NO_SPACE**（损坏被伪装成耗尽）。
- 探针 2：order 链自环（lo.next=0，健康 9200 B 尾块可用）→ 旧 alloc **永久挂死**
  （timeout rc=124，无返回）。
- 新实现：两场景均为有界 CorruptMetadata（R29(5) + R18(c) 转绿）。

## 4. 只读复审结论（任务书 §16 第二遍）

只看最终 diff（6bcd28f..801b8c3）、调用图、状态图、复杂度与日志，专项寻找
"测试通过但契约矛盾"：

1. **调用图**：merge 不再调用 compact_impl（无残留）；所有维护路径统一走
   precheck_pool / walk_order / audit_pool_bins / finalize_layout；
   `order_insert_sorted` 仅剩 alloc 一个调用方（merge/split 由计划数组直接重建）。
2. **状态图**：Empty→Running⇄Paused；维护态 Compacting/Merging/Splitting 只能
   从 Running/Paused 进入、只能在最终提交锁内发布 Running/Empty；失败仅恢复
   入口状态（R7/R16/R22/R28 钉住）；destroy_pool 拒绝维护态；新池提交前为
   Splitting（不可观测为可用）。
3. **契约 vs 文档逐条**：并发契约（单所有者 + 静默期 + 维护串行）、复杂度表
   （alloc 项第四轮更正）、resolve/borrow_end 语义、local 绑定解析期强制、
   free 的验证顺序——README、pondmerge.hpp、测试三方一致；无矛盾点。
4. **alloc 重排的瞬时一致性**：链接先于块变更的窗口内，槽位 state=Free、
   generation 未动（失败路径满足 R1 的 ABA 不变量），单所有者下无外部观察者；
   borrow/get_stats 不遍历 order 链，不受影响。
5. **失败输出契约**：resolve/borrow_begin 失败清空输出（R26）；alloc 新增的
   CorruptMetadata 路径不写 `out`（失败在 out 赋值之前）。
6. **发现并当场修正**：R29(4) 首版复用引用导致对象泄漏（测试 bug，已修）、
   R29(5) 槽位计数未扣除两个活对象（测试 bug，已修）——库行为无涉。

结论：未发现"测试通过但契约矛盾"的残留。

## 5. 复杂度、固定内存与栈

- alloc：O(SL bin 链长 + live_objects)，上界 O(zone_size/PM_MIN_BLOCK)。
- free：O(1 + 邻块空闲 bin 链长)。compact/split：O(objects + moved bytes)（含审计）。
  merge：O(objects + free_blocks + moved bytes)。validate：O((live+free)²)。
  get_stats：O(free_blocks)，步数上限。
- 固定 scratch：O(PM_MAX_OBJECTS) = s_plan + s_upper + s_barriers + s_slots(uint16)；
  设备（PM_MAX_OBJECTS=256）约 2.5 KiB，随 metadata_bytes 如实上报（设备实测
  meta=27,704 B 含增长）。
- 最大栈：维护路径无递归；最深局部为 free 的 verify_neighbours lambda 与测试
  快照 lambda，均 O(1) 栈。alloc 失败审计路径 O(1) 额外内存。

## 6. 验收门槛实测（任务书 §14）

```
tests/run_host.sh 10000           -> 5,393,966 checks, 0 failures（模型 466,859 checks, 0 failures）
tests/run_host.sh --release 10000 -> 5,393,973 checks, 0 failures
tests/run_host.sh --san 10000     -> 5,393,966 checks, 0 failures
tests/run_host.sh --cppcheck      -> exit 0，0 告警
tests/run_host.sh --configs       -> configuration matrix PASSED
-Wall -Wextra -Werror（host 手动编译）-> 干净；ESP-IDF 构建自带 -Werror -> 通过
```

ESP32-S3（提交 `801b8c3`，先提交后烧录，App version 无 -dirty）：

```
App version:      801b8c3（= HEAD）
Compile time:     Sep 12 2026 01:24:02
ELF file SHA256:  dd5bfaca9c30826456f46349a72206b38f1771b3aa369e6372dfce15eb9eec30
chip: model=9 (ESP32-S3) rev=0.2 cores=2 · ESP-IDF v6.0.2 · gnu++17（组件强制）
固件 263,408 B · Auto Zone 256 KiB @ 内部 SRAM · 启动空闲内部堆 93,056 B
套件（基础 13 组 + R1–R29，2000 ops）：1104804 checks, 0 failures PASSED
  max_live=55 max_borrow=1 max_moved=30960 max_compact_us=2509 meta=27704
双核并发（200 轮 pause/compact/resume + 双核 borrower）：
  borrow_a ok=3968 busy=128 | borrow_b ok=3319 busy=777
  maintainer: pause=200 compact ok=177 busy=23 resume=200
  28 checks, 0 failures PASSED · 无 task_wdt 告警
```

## 7. 提交序列（第四轮）

```
801b8c3 core/tests: alloc 防环 + 损坏可区分 + get_stats valid + R29 + 审计账本
<final>  docs: HANDOVER_v6 终报
```

## 8. 未解决风险与产品决策（全部为已接受边界，详见账本 §6）

1. free 与 borrow 的 TOCTOU：单所有者契约覆盖；持锁跑回调不可行。
2. 设备重复 end/错误 token：Debug 固件断言（调用方 bug）；语义由 host Release
   R25 钉住；SMP 维度由设备并发测试覆盖。
3. get_stats 不审计 state/borrow_count：控制字段非布局不变式，损坏时全入口
   安全拒绝（R29(4)）。
4. 逐操作性能峰值（validate/alloc/free 计时）：v1 只测维护耗时；不引入计时设施。
5. RawRef 可伪造：unsafe 低层句柄（R27 固定语义），伪造无法越界。
6. validate 的 O((live+free)²)：如实声明；优化需固定 scratch 物理扫描，单独立项。

## 9. 停止条件核对（任务书 §16）

1. 账本每项高风险有代码位置/证明/测试 ✅（docs/AUDIT_LEDGER.md）
2. P0/P1 已修复 ✅（第三轮 7 项 + 第四轮 2 项新缺陷）
3. 维护状态/锁/事务/失败恢复语义一致 ✅（只读复审 §4.2）
4. 引用/generation/epoch/offset/输出契约一致 ✅（§4.5）
5. 故障注入、随机模型、Sanitizer、设备并发完成 ✅（§3、§6；矩阵覆盖见账本 §3）
6. 复杂度/固定内存/栈与代码相符 ✅（§5 与账本 §5）
7. Host/ESP32/文档证据同一最终提交 ✅（801b8c3）
8. 剩余边界明确列出 ✅（§8）
