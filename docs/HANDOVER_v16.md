# PondMerge v1 第十四轮：多代理深度审查与元数据筛查修复（HANDOVER v16）

> 起因：多代理深度自审查轮。多路独立审查（六路视角）+ 变异测试 ×2 + 对抗
> 反驳，对库代码、测试与文档做一次全量体检；反驳阶段**没有推翻任何一项发现**，
> 两项降级。全部经确认的发现形成两个修复批次的任务清单，本文件是收口报告。
>
> 基线：`fdbe5d4`（第十一轮收口，十一项 host 门禁全绿）。
>
> **本轮性质**：修复全部是**元数据损坏筛查**（对合法路径零行为变化）与
> 文档/账本诚实性收口；门禁检查数的增量**全部**来自新增测试组（见 §4）。

## 1. 审查结果统计

- 六路深度审查 + 变异测试 ×2 + 对抗反驳；
- 发现计数：**10 High / 16 Medium / 27 Low**；
- 反驳后：**0 项被推翻、2 项降级**，其余全部进入修复或登记；
- 修复批次 1（库代码 + 测试）：7 项（§3）；修复批次 2（文档/脚本/示例/
  头文件注释/账本，本报告随附的收尾批）：其余各项以登记与同步方式落地。

## 2. 基线数字（fdbe5d4，十一项 host 门禁全绿；门标签当时错标 1/10…，见 §6）

| 档位 | checks |
|---|---|
| Host Debug（10000 op） | 5,409,619 |
| Host Release（10000 op） | 5,409,626 |
| ASan+UBSan（3000 op） | 1,508,630 |
| 参考模型对拍 | 466,859 |
| 协议 / HTTP | 95 / 15 |
| 覆盖率（Debug 档，当时为单档门） | 96.47% 行 / 77.09% 分支选取 |

工具链备注：上述历史数字产生于 g++ 14（文档参考）；本轮 host 数字产生于
**g++ 15.2.0**，历史数字在 15.2 下逐位复现（修复批次 1 的验证运行确认）。

## 3. 已修复清单（7 项，各附红测证据要点）

1. **A1-1（High）：alloc 的 order_head 界筛查。** `alloc()` 以 O(1) 方式解引用
   池的 `order_head` 做链表追加，损坏的头若落在 `[0, PM_MAX_OBJECTS)` 之外会
   写过描述符表。现于任何变更前做 O(1) 界筛查，失败 `CorruptMetadata`、
   零写入、输出引用已清空。红测 **R38**：构造超界 head，断言拒绝且无写入。
2. **A1-2（High）：check_ref 的链接界。** `free()` 的 `order_unlink()` 直接解引用
   Live 描述符的 `addr_prev`/`addr_next`（O(1) 契约不遍历）。`check_ref()` 现在
   拒绝表外链接，free/borrow/resolve/set_destroy_fn 同享这一次检查。红测
   **R39**：表外 `addr_next` 注入，断言 free 拒绝且池逐字节不变。
3. **A1-3（High）：create_pool 的窗口证明。** `create_pool()` 对每个现存池标记
   `used[segment_first + s]` 前未证明窗口；损坏窗口意味着 `bool[PM_MAX_SEGMENTS]`
   越界写，或新池建在存活池的块上。现对每个非 Empty 池先过 O(1) 几何证明再标记。
   红测 **R43**：损坏窗口注入，断言拒绝且 used[] 扫描不越界。
4. **A1-4（High）：池几何 zone 上限。** 新增 `pool_geometry_ok()`：段和与字节
   体积双界、uint64 计算、对 `G.zone_size` 独立成立；损坏的 `segment_count`
   既不能回绕也不能溢出成看似合法的窗口。红测 **R44**：跨 zone 尾的窗口注入。
5. **C-1：free 第二次邻块验证按 `destroy_fn` 门控。** 无回调时两次验证之间
   不可能有代码运行（单 owner 契约），第二次证明是第一次的确定性重放；带回调
   对象的"回调后重验"契约（R24）逐字保留。**证据**：callgrind 指令数
   756.7 → **728.5 指令/pair（−3.7%）**——host x86-64、callgrind 确定性计数、
   两运行长度差分（方法同 `bench/README.md` §2 的 `host_insn`）。引自审查代理
   实测，本机未复测（见 §8 SKIPPED）。
6. **C-2：advice 阈值判定改交叉相乘。** `stranded*1000/capacity >= permille`
   改为恒等的 `stranded*1000 >= permille*capacity`（uint64 双侧无溢出，论证
   在代码注释与账本 §5），Xtensa 上的 64 位软除法离开判定路径；
   `fragment_ratio_permille` 报告字段保留一次 64÷64（报告值，非判定）。
7. **A1-6：generation 回绕的 Debug 报告。** 回绕分支原为静默吞掉的死分支，
   现在 Debug 构建经 `pm_wrap_note` 报告；语义不变（回绕仍跳过 0）。

## 4. 新增测试组与配置变体

- **R36–R53（R45 空缺，共 17 组）**：R36 mid-gap slack、R37 post-deinit 拒绝、
  R38/R39/R43/R44 上列修复红测、R40 重复 live 地址、R41 split 故障注入、
  R42 free 的环/池外 bin 链、R46 耗尽矩阵（槽位/池表/段）、R47 validate 第三段
  头部手术、R48 advice 重复 INVALID_REQUEST、R49 Paused 池失败维护恢复、
  R50 零长尾访问、R51 generation 回绕双路径、R52 恰 16 B gap 边界、
  R53 溢出带（0xFFFFFFF8）。
- **config_matrix 新增 PM_MIN_BLOCK=32 变体**（A4-08）：它是 `need <
  PM_MIN_BLOCK` 钳制分支在默认 16 B 下结构性死亡时的唯一真实执行者；
  死分支清单见 `docs/AUDIT_LEDGER.md` §7。
- 账本同步：§1 新增四行（pool geometry / create_pool / live-slot 链接界 /
  失败输出补充）、§2 order_unlink 行补输入界来源、§3 注入矩阵补 R38/R39/
  R43/R44、§4 并发表补三行、§5 复杂度账本全面对齐、§6 新增 §6.11 与第
  12–17 项、新增 §7 死分支登记。

## 5. 性能改动（C-1/C-2）的证据与仪器

- 唯一的性能相关改动是 C-1（少一次确定性重放验证）。**仪器**：callgrind
  （指令数为确定性计数，与微架构无关）；**方法**：两运行长度差分抵消进程
  启动（与 `bench/host_insn.cpp` 归因 free/alloc 的技巧相同）；**平台**：
  host x86-64，Release 档。结果 756.7 → 728.5 指令/pair（−3.7%）。
- C-2 的收益是**去掉一次 64 位软除法**（Xtensa 上为 `__udivdi3` 调用），不另
  立数字——判定路径的除法在设备上是数十至上百周期级的调用，移除本身就是
  结论；数字级的收益未在设备上测（板子不在线，见 §8）。

## 6. 门禁与提交

- 新期待数字（已同步 `CONTRIBUTING.md` §3、`scripts/gates.sh` 头注、两份
  README 的验收表）：
  **Debug 5,428,675 / Release 5,428,682 / San（3000 op）1,527,686 checks,
  0 failures**；模型对拍 **466,859** 不变；protocol 95 / HTTP 15 不变。
- **覆盖率门改为双档**（A4-12）：Debug（PM_DEBUG=1）+ Release（PM_DEBUG=0）
  各跑一次同流程，行覆盖下限 85% 对两档生效，gcov 产物分目录
  （`build/cov`、`build/cov_rel`）互不覆盖。第二档的存在理由：R25 的负路径
  只在 Release 编译，Debug-only 的覆盖门结构性测不到它们——"套件覆盖了它"
  此前对 R25 是伪声明。本轮实测：**Debug 97.59% 行 / 81.15% 分支选取**
  （1327 行口径，与修复批次 1 的数字一致），**Release 97.77% 行 / 83.30%
  分支选取**（1301 行口径——PM_ASSERT 体编译出，总行数更少）。
- `scripts/gates.sh` 标签统一为 1/11…11/11（A5-03，benchmarks 为第 11 项）；
  门数保持 11、门名不变。
- 提交链：按逻辑轮提交（修复批次 1 的代码+测试一笔、文档/账本/脚本收尾
  一笔、设备记录按 App version 对账另计），见 `git log`。

## 7. 已知边界更新（账本 §6 新条目摘要）

- **§6.11**：池几何的 zone 内平移不由 `create_pool` 检测——`pool_geometry_ok`
  只证明窗口在 zone 内，重叠/平移由 validate/precheck 的字节账目拒绝
  （R29(3) 既有覆盖）；"O(1) 前置筛查只证明 O(1) 可证的事"。
- **第 12 项（A1-5）**：任务书 §8.3 的"校验失败恢复 Paused"被事务式实现取代
  ——计划失败恢复**入口状态**（零写入）；compact 的借用拒绝仍按文档 §8 留在
  Paused（R29(5)、R49 钉住）。
- **第 13 项（A2-L1）**：8 B slack 后继的 prev_size 写入落 [slack+4, slack+8)，
  无读者、poison 头不受影响（探针 676 检查证实）。
- **第 14 项（A2-L2）**：FreeBlock `reinterpret_cast` 是 C++17 对象生命周期模型
  上的设计边界（header 走 load32/store32，链接走类型化成员）；gcc/clang -O3
  实测干净；不在 v1 引入 C++23 方案。
- **第 15 项（A2-L3）**：`bins_find` 游标筛查为 zone 级、其余遍历为池级——
  doctrine 不一致，后果被 zone 界 + 尺寸类核对 + 失败审计限制；将来加固候选。
- **第 16 项（A2-L4）**：advice 打包模拟对描述符地址做原始指针算术且先于
  counters 审计——描述符地址目前无 API 途径失效，列为纵深防御缺口。
- **第 17 项（A2-L5/A3-4）**：lifecycle（init/deinit/create_pool/destroy_pool）
  串行、`set_destroy_fn` 单 owner、`global_stats` 宽松读——已同步 §4 并发表
  与 `pondmerge.hpp` 并发契约注释（仅注释改动）。
- **§7 死分支登记（A4-16）**：L61/L35/L256/L345/L836/L838/L943/L1059/L1445/
  L1079-1081/L1466/L1519/L1544/L1573/L1643/L2152/L2234（fdbe5d4 行号）各附
  一句支配论证；声明该清单防止未来轮次误当缺口。

## 8. SKIPPED（未验证事项，大声声明）

- **设备端全部未验证**（SKIPPED — not run, not passed）：本轮所有修复与文档
  收口只过了 host 门禁；S3 / 经典 ESP32 的重烧与抓取未执行（板子不在线），
  设备记录仍指向此前的提交（App version 见 README 验收表的历史行）。
- **ESP-IDF 工程构建未验证**（SKIPPED — not run, not passed）：
  `examples/sensor_pipeline_esp32` 与 `examples/esp32_demo` 的 main
  CMakeLists 新增 `PM_MAX_OBJECTS=256` 编译定义后，本机无 IDF 环境无法
  重新构建验证；改动与 `esp32/main/CMakeLists.txt` 既有做法一致，风险为低。
- **C-1 的 callgrind 数字（756.7 → 728.5）引自审查代理的实测，本机未复测**
  （仪器与方法已在 §5 说明；callgrind 计数为确定性，复测应逐位一致）。
- `.gitignore` 追加 `examples/*/sdkconfig` 不影响已被跟踪的
  `examples/esp32_demo/sdkconfig`；是否 `git rm --cached` 去留由维护者决定
  （本报告即登记处）。

## 9. 文档地图（本轮更新落点）

- `README.md` / `README.en.md`：验收表新数字（San 行补 3000-op 标注）、验收
  表头改"由 scripts/gates.sh 每轮收口复验"（去掉陈旧提交号）、元数据预算补
  x86-64 限定句与 ESP32-S3 实测行、validate 复杂度改 O((live+free)²)、alloc
  行补 PM_ZERO_INIT O(size)、目录树补 churn_overhead/host_insn/
  consumer_smoke、文档索引指向 v16。
- `QUICKSTART.md`：R1–R31 → R1–R53。
- `CONTRIBUTING.md`：十一项门禁（benchmarks 为第 11 项）、新期待数字、
  cppcheck 版本注释指引、文档地图 v2–v16。
- `bench/README.md`：heap_4 叙述就地括注更正（实际为 IDF 的 TLSF）。
- `docs/USAGE_GUIDE.md` §6：compact/merge/split 三行补 O(objects log objects)
  恢复地址序。
- `CHANGELOG.md`：三个同级 Fixed 小节合并为一（保留条目顺序），本轮修复与
  门禁改动入账。
