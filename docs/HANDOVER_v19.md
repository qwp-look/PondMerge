# PondMerge v1 第十七轮：审计修复轮（HANDOVER v19）

> 起因：一轮**全面的独立审计**（核心实现 / demo 安全 / 易用性三路并行，全部发现
> 在主机或设备上先复现后修复）。本轮修掉审计的全部高优先级项；设备与主机门禁
> 每步全绿。
>
> 基线：`d187339`（v18 收口）之前的 `c7cdb69` 起算。本轮提交（按序）：
> `bd83396`（create_pool）、`33601b6`（demo 层）、`d187339`（CMake）、
> `a698713`（validate）、本报告一笔（文档数字更正）。
>
> **本轮性质**：core 的两处改动（create_pool 入参界、validate 的拒绝规则）都让
> 「库不可能产生的状态」被更早、更有界地拒绝；合法布局的语义与复杂度不变——
> 模型对拍 466,859 checks 三平台不变，套件期望 checks 的增量全部来自新增测试
> R56/R57（各 +12 / +9 checks）。

## 1. create_pool 段数符号回绕 → 域外写（R56，host SIGSEGV 实锤）

`(int32_t)segment_count` 把 ≥2³¹ 的计数回绕成负数，run 搜索假阳性"找到连续段"，
`(uint16_t)` 截断后池窗口落在域外：

| 输入 | 修复前 | 修复后 |
|---|---|---|
| `0x8000000C` | **SIGSEGV**（gdb 定位在 `pool_start` 写块头，base=65525 段） | `NoSpace` |
| `0xFFFFFFFF` | 返回 **Ok**，池宣称 268 MB 空闲（zone 实际 256 KiB） | `NoSpace` |

修复：入口 `segment_count > G.segment_count → NoSpace`。一条域侧界即足够
（`PM_MAX_SEGMENTS <= 0xFFFF` 的 static_assert 保证合法计数不溢出描述符字段），
`split()` 本就有等价防护。O(1)。

## 2. validate()：二次回退可达 + 相邻空闲块假阴性（R57）

结构性审计（live 表 / bins）不约束**极大性**，因此一个被篡改的池——整池均匀
16 B 块、链入 size-16 bin、计数器全部对账——能穿过 validate 的全部前置检查：

- **nfree > kFreeOffCap**：落入二次回退，host 1 MiB 实测 **11.3 s**（0.69 s @
  256 KiB），外推到 240 MHz MCU 是分钟到小时级——违背 validate 自己的
  "有界时间"承诺。
- **nfree ≤ cap**：归并扫描对**物理相邻的已入箱空闲块**（gap==0）一路放行，
  最终**报 Ok**——假阴性。free() 恒合并其物理邻块，所以库产布局中相邻已入箱
  空闲块不可能存在；文档化的"两空闲块隔 slack"（gap < PM_MIN_BLOCK）依然合法。

修复：`free_blocks ≤ live+1` 从"回退不可达的论证"升格为**拒绝规则**——超限即
损坏，O(n) 内拒绝；二次回退整块删除（~55 行）；归并扫描新增 gap==0 的
free→free 规则。AUDIT_LEDGER §7 的死分支登记行已更正（该分支**可达**）。

## 3. demo 层（examples/，安全 + 健壮性）

| # | 缺陷（全部先复现） | 修复 |
|---|---|---|
| 1 | UI 点一次 **reset 整个 demo 服务器永久死锁**：`push()` 持 `STATE_LOCK` 时调 `log_protocol_error()` 重入同一把不可重入锁 | 拆出锁内助手；http_smoke 补"HTTP 层点 reset 后服务器仍应答" |
| 2 | `/api/op` 接受 `text/plain`（浏览器跨站免预检）→ CSRF 可执行命令直至 `quit` | 强制 `application/json`（415），伪造请求过不了预检 |
| 3 | 不校验 Host 头 → DNS rebinding 全读写 | 非 loopback Host → 403 |
| 4 | 畸形/负 Content-Length 裸异常；超长声明挂死读线程 | 统一 400 |
| 5 | `host_demo` 把用户输入回显进 JSON（`{"cmd":"a\"b"}` 产出**非法 JSON**，破坏分帧）；代码注释却声称"绝不回显" | 未知命令用固定 op `"unknown"`；protocol_smoke 补转义/换行输入（95→104 checks） |
| 6 | reset/quit 在 HTTP 层恒为 3 s 假 TIMEOUT | reset 发规范的 ready 开新会话（seq 重启合法化）；完成判定改为"该命令的任何可观察进展" |
| 7 | 子进程僵尸；display-only 按钮可点但静默失败 | 收割子进程 + 按钮禁用与横幅（DEMO_REQUIREMENTS §6） |

## 4. CMake：PM_MAX_OBJECTS 覆盖静默失效（CMP0126）

`cmake_minimum_required(3.16)` 把 CMP0126 钉在 OLD：本库的
`set(... CACHE STRING ...)` 抹掉消费者在 `add_subdirectory` 之前写的同名普通
变量。实测：消费者 `set(PM_MAX_OBJECTS 128)` 后库仍按 1024 编（116,245 B
元数据），零告警——恰是 README 警告的 RAM 灾难，且无任何提示。
`cmake_minimum_required` 升 **3.21**（NEW 策略下普通变量优先；无功能需求）；
`consumer_smoke.sh` 补上 add_subdirectory 路径 + 预算断言（此前该路径零覆盖，
README 却声称两条路径都被验证）；README 写明两条纯 CMake 路径各自唯一的覆盖
方式（find_package 的预算在安装时冻结）。

## 5. 文档数字漂移（全部实测更正）

- 元数据预算表 4 行是 v17 之前的旧值（27,064 / 28,672 / 108,040 / "105 KiB"）。
  6 组配置 2026-09-25 全部重测：64/2=7,812、128/4=15,548、256/4=29,116、
  256/16=34,828、512/8=58,156、1024/16=116,236（x86-64 Release，公式逐一吻合）；
  S3 256/16 = **30,729**（验收固件运行时打印）。
- "约 105 KiB" → **约 113.5 KiB（116,236 B）**；README.en 同步。
- "metadata_bytes 与 .bss ±8 吻合"的说法不成立（512/8 与 1024/16 档实测 .bss
  反而小 2–4 KB——GCC 节放置）：改为"以 metadata_bytes 为上界预算"。
- ASan 门禁数 1,527,686 → 1,531,817；覆盖率行补 v17 后实际值（92.51% / 92.58%）。
- 文档索引 / 交接指针从 v16 更新至 v19。

## 6. 门禁与设备（每笔提交后全绿）

| 档位 | 数字 |
|---|---|
| Host Debug（10000 op）/ Release / San | 5,432,827 / 5,432,834 / 1,531,838（R56+R57 共 21 条新增），0 failures |
| 参考模型对拍 | 466,859（不变） |
| 协议 / HTTP 冒烟 | **104 / 27** checks, 0 failures |
| 覆盖率 / 配置矩阵 / fuzz / 示例自检 / 基准构建 | PASSED（11/11 门禁） |
| Host Debug 256 档 | 1,140,849 checks, 0 failures |
| ESP32-S3 实机（`60b079c`，`v1.0.0-28-g60b079c`） | suite **1,140,849**（R56+R57 在跑）+ 并发 28 + model 466,859，0 failures；与 host 256 档**逐位一致** |

## 7. 遗留项收口状态（本轮全部处理）

| 项 | 状态 |
|---|---|
| seg_base UB 纯化 / NotInitialized 状态码 / operator-> 诊断 / segment_size 契约 | **完成**（`dce28e5`，见 §8 CHANGELOG 摘要） |
| validate 整理窗口百分位重取（v17 §9 遗留） | **完成**：`v1.0.0-30-gdce28e5` 上 512 事件重测，p50 8,977.65 → **825.6 µs（10.9×）**，moved bytes 逐字节一致；`bench/RESULTS.md` §5.8 已更新，旧记录保留对照 |
| 经典 ESP32 重烧（v18 §6 遗留） | **硬件阻塞**：本轮该板未连接（仅 `/dev/ttyACM0` 的 S3 在线）。README 验收表已注明该行为推断非实测；板子接上后 `rm -f sdkconfig` + `set-target esp32` + 双 defaults 文件重烧即可 |

## 8. 观察与监控

- **一次未复现的设备重启**：约 7 次完整套件运行中观察到 1 次 R55 进行中无
  panic 输出的重启（ROM 横幅后 bootloader 无输出，USB CDC 随之失联）；紧随其后的
  两次完整重跑全绿（含同二进制）。已尝试 openocd USJ 复位 + 只读抓取的观察闭环，
  未能捕获现场。候选原因：PSRAM/USB 硬件抖动、或低概率的未定义行为——**列入
  监控**：再次出现时用 openocd `halt` 抓 PC 现场（复位前）。
- 设备刷写今日成功率约 50%（`Packet content transfer stopped`/串口噪声），
  重试即可恢复；先起只读抓取再刷写的流程可把刷后状态一并捕获。

- 低优先级项（下轮候选）：`seg_base()` 在几何证明前形成指针（UB 纯化）、
  未初始化调用报 `CorruptMetadata`（考虑 `NotInitialized` 状态码）、
  `operator->` 失败路径的报错可读性、`segment_size` 文档（≥4 KiB）与实现
  （接受 1024）的契约统一。
- 经典 ESP32 重烧（v18 §6 遗留）。
- validate 整理窗口百分位重取（v17 §9 遗留）。
