# PondMerge v1 第五轮审计修复报告（HANDOVER v7）

> 依据《PondMerge v1 代码审计与修复指南》（审计基线 `d8345e1`）执行。
> 最终提交：`564a15f`（代码）+ 本报告提交。
> 按指南 §9 要求，本报告分三部分：**本次修复** / **已有测试通过** /
> **未覆盖/依赖调用方契约**。

## 1. 本次修复

| 优先级 | 指南问题 | 修复 | 测试 |
|---|---|---|---|
| P1 | `alloc()` 失败不保证 `out` 无效 | `alloc()` 首语句 `out = RawRef{}`（generation 0 = 无效），覆盖全部失败路径（InvalidPool/参数/NoSpace/CorruptMetadata）；槽位 generation 不动（R1 语义保持） | 新增 **R30**：以旧引用为 out 触发 6 类失败 + 损坏路径，断言 `out` 全零且旧引用仍可独立使用与释放 |
| P1 | 维护期并发契约未明确化 | pondmerge.hpp 并发契约与 README 并发表：明确 alloc/free/resolve/get_stats/validate **单 owner**；维护前由调用方停止 DMA/ISR/其他任务/外部持有者；**PM_LOCK 只保护借用计数与状态发布**，函数体不在锁内；Host 空锁不能证明锁语义 | 文档一致性：头文件/README/账本三方同文；设备双核并发测试保留 |
| P2 | `finalize_layout()` Release 未使用变量告警（core.cpp:519） | `++steps` 与断言分离 + 尾部 `(void)steps` | Release `-Wall -Wextra -Werror` 对 core/suite/model 三文件全净（告警先复现后消除） |
| P2 | `resolve()`/`peek()` 边界不清 | 头文件标注为**高级不计数接口**：裸指针仅在静默期立即使用，禁止跨越任何维护入口或保存；常规路径 `try_borrow()`/`pm_access`/`operator->`（表达式级 RAII） | 文档；R26 行为不变 |
| P2 | ESP32 未编译参考模型，不能声称设备侧模型对拍 | `model.cpp` 在 `PM_DEVICE_BUILD` 下复用 suite 的 `g_zone`（设备静态 DRAM 无余量放第二个 64 KiB zone）；加入 esp32 构建与 `app_main`（**4000 ops**，host 同规模）；`serial_cap.py` 结束标记更新为 model 判定 | **设备实测 466,859 checks, 0 failures——与 host 同数**；HANDOVER_v4 T5 缺口关闭 |
| 关联更新 | 测试[10] 依赖旧行为（失败 alloc 保留旧 out） | NoSpace 探针改用独立 out 变量并在该处钉住新语义（按任务书规则更新并注明理由） | 测试[10] + R30 |

复杂度影响：alloc 成功路径仍 O(bin 链长 + live_objects)，失败清空 O(1)，额外空间 O(1)；不得修改 descriptor generation 的约束保持（R1 复验通过）。

## 2. 已有测试通过（未改动部分复验）

- 基础 1–13、R1–R29、模型对拍（host 4000 ops 规模）全部保留并通过。
- 维护事务结构（只读审计 → 只读规划 → 不可失败执行 → 单锁提交）、free 的
  验证先于回调、pinned/DMA/external 不搬迁、generation/address_epoch 语义、
  全部损坏遍历的范围检查与步数上限——第五轮指南 §8 的保留清单逐项核对无回归。
- 五档门禁与设备验证见 §4。

## 3. 未覆盖 / 依赖调用方契约（明确列出）

1. **维护期 SMP 安全**：解锁搬移 + 单 owner/外部静默契约是 v1 既定架构；真正
   SMP 安全需重新覆盖全部 GlobalState/Pool/ObjectDesc/bins/order-list 读写并
   给出锁内最大阻塞时间证明——属重设计，不在 v1（指南 §4 原文认可）。
   debug-only owner token 评估后不采用（host 单线程无执行价值；账本 §6.7/6.8）。
2. **外部持有者**：DMA/ISR/外部库持有的裸地址库不可见，调用方负责静默期
   （架构边界，非缺陷）。
3. **Host PM_LOCK 为空操作**（P3）：Host 并发运行不能证明锁语义；SMP 证据
   只来自双核设备测试——已写入头文件契约与 README。
4. **RawRef 可伪造 / relocatable trait 项目责任 / 对象内部裸指针不重定位**：
   架构边界维持原状。

## 4. 验收实测（任务书 §9/§14）

```
Host（提交 564a15f）：
tests/run_host.sh 10000           -> 5,394,058 checks, 0 failures（模型 466,859 checks, 0 failures）
tests/run_host.sh --release 10000 -> 5,394,065 checks, 0 failures   <- Release 零告警
tests/run_host.sh --san 10000     -> 5,394,058 checks, 0 failures
tests/run_host.sh --cppcheck      -> exit 0，0 告警
tests/run_host.sh --configs       -> configuration matrix PASSED
g++ -std=c++17 -Wall -Wextra -Werror（与 runner 等价，Debug/Release 两档）-> 通过

ESP32-S3（App version 564a15f = HEAD，先提交后烧录）：
固件 270,864 B · Compile time Sep 12 2026 08:06:22
ELF SHA256 58c7c9fa9a2a6469c6433659c16b9a8cbb76409b5793c23748556b4922eba506
chip model=9 rev=0.2 cores=2 · 启动空闲内部堆 92,976 B
套件（基础 13 组 + R1–R30，2000 ops）：1,104,896 checks, 0 failures PASSED
  max_live=55 max_borrow=1 max_moved=30960 max_compact_us=2519 meta=27704
双核并发（200 轮 + 双核 borrower）：28 checks, 0 failures PASSED
  borrow_a ok=3631 busy=64 | borrow_b ok=3208 busy=429
  maintainer: pause=200 compact ok=193 busy=7 resume=200
参考模型对拍（设备，4000 ops）：466,859 checks, 0 failures PASSED
```

并发状态发布观察（指南 §4 验收项）：borrower 的 busy 计数（64/429）与
maintainer 的 compact busy（7）即为维护状态发布/静默期闸门的直接观测——
Paused/Compacting 期间无任何 borrow 成功（R30/设备并发测试的组合断言）。

## 5. 提交序列

```
564a15f core/tests: round-5 fixes（alloc 清空 out / Release 告警 / 契约 /
                    设备模型对拍 / R30 / 账本）
<final>  docs: HANDOVER_v7
```
