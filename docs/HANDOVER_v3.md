# PondMerge v2 修复轮交接文档（续轮收口）

日期：2026-09-11
基线提交：`e7bccf0`（v2 第一轮收口时的状态）
收口提交：见 git log（本文档同提交入库）
依据文档：`docs/PondMerge_v2_repair_task.md`（第二轮任务书，本轮已入库）、
`docs/HANDOVER_v2.md`（第一轮收口文档）、`docs/PondMerge_v1_repair_task.md`

本轮工作 = 把 `HANDOVER_v2.md` §2 列出的未完成项做完，并在过程中修出 4 个任务书
没有列出的真实缺陷（§2）。

## 1. 本轮完成并验证的修复

| # | 任务书条目 | 修复位置 | 回归测试 |
|---|---|---|---|
| 1 | §9.1 `validate()` 整数化 + 块内边界 | `src/core.cpp` `validate()`：所有范围运算改用 zone 偏移（uint64），bins 游标在解引用前判界；补齐 `block_size ≥ MIN`、对齐、`size ≤ block_size - HEADER`、payload 落在自身块内 | R17、R21（10 个域逐项损坏） |
| 2 | §9.3 块覆盖完整性 | `validate()` 第三段审计：以「live ∪ binned free」并集为基础，要求每个未覆盖间隙 **必小于 `PM_MIN_BLOCK`**，并校验 `live + binned_free + slack == capacity` | R15(b)(c)、R21 |
| 3 | §11.1-8 descriptor/物理块关系 | 新增 `desc_block_consistent()`，在 `check_ref()` 中调用 → `resolve`/`borrow_begin`/`free`/`validate` 一致拒绝 | R17 |
| 4 | §7.3 复杂度声明诚实化 | `README.md` 复杂度表 + `pondmerge.hpp` 维护段与 `alloc` 注释 | 文档对照（无 O(1) 误述） |
| 5 | §12.1 静态检查 | 逐条修改或带理由抑制；`SIZE_MAX`…见下 | `tests/run_host.sh --cppcheck` 退出码 0、0 告警 |
| 6 | §11.1-2/3/4/9/10/11/12 定向测试 | `tests/suite.cpp` | R15–R21 |
| 7 | §11.2 模型对拍 | 新增 `tests/model.cpp`，接入 `tests/main.cpp`，所有 host 模式都跑 | `model_differential`（固定 seed） |
| 8 | §11.1-7 FL 容量契约 | `init()` 新增 2^PM_FL_MAX 容量拒绝；新增 `tests/config_limits.cpp` | `tests/config_matrix.sh`（FL=16/24 各跑「拒绝」与「接受」） |
| 9 | §7.2 `fl_index()` 不再静默 clamp | 由 #8 保证：不可表示的 zone 在 init 期即被拒绝 | 配置矩阵 |
| 10 | §5.1 维护入口语义统一 | `merge`/`split` 改为接受 `Running` 或 `Paused`；失败恢复**入口状态**；`compact` 在规划失败时也恢复入口状态 | R16（三段矩阵） |

## 2. 本轮修出的真实缺陷（任务书未列出）

1. **`free()` 可能把池尾陈旧字节当成空闲块** —— `finalize_layout` 只对**块间**小
   间隙写 poison（`store32(prev_end, 0)`），**池尾** slack 没有写。当一个 used 块是
   池中最后一个块、其后只剩 < `PM_MIN_BLOCK` 的尾部 slack 时，`free()` 的前向合并
   会读取 `block + block_size` 处的陈旧字节并做 `blk_is_free()` 判断，可能把垃圾当
   成空闲块、进而 `bins_remove()` 一个不存在的块，破坏 bins 与统计。
   修复：池尾 slack 同样写 poison。触发测试 R15(c)。
2. **`get_stats()` / `bins_find()` 的越界游标会被解引用** —— 此前只有步数上限（能
   防环但防不住越界读）。现在两者都在解引用前用 `zone_off_readable()` / 池范围判界；
   `get_stats` 遇到越界游标返回 `largest_free_block = 0`。触发测试 R18（含 ASan 下
   的野偏移）。
3. **`precheck_pool()` 未校验 descriptor 地址对齐** —— §8 的前置验证表里有「对齐」
   一项，但只查了 `block_size` 的对齐，没查 `address`。已补，并补了
   `boff + block_size ≤ end_off`。触发测试 R20。
4. **`init()` 没有 FL 容量检查** —— 当声明的 zone 大于等于 `2^PM_FL_MAX` 时，
   `fl_index()` 会把大块静默 clamp 到最高 bin，破坏 bin 记账。现在 init 直接返回
   `NoSpace`。触发测试 `tests/config_limits.cpp`。

## 3. 调用方可见的语义变更

- **维护入口统一**（`pondmerge.hpp` 契约段有完整表述）：
  - 入口状态：`Running` 或 `Paused` 都接受；已在 `Compacting/Merging/Splitting`
    中的池返回 `Busy`。
  - 借用计数非零 → `Busy`，且在搬移前返回。
  - **规划失败 → 恢复入口状态**（此前 merge 失败会把两池都置为 `Paused`，split 失败
    一律置 `Running`）。
  - 唯一例外：`compact` 因借用被拒时保持 `Paused`（文档 §8 规定 compact 先进入
    Paused 再查借用），由调用方 `resume()`。
  - 成功 → 结果池 Running（merge 的源池变 Empty）。
- **池尾 slack 现在被写 poison**：纯内部行为，`fragment_bytes` 统计不变。
- **`init()` 增加 FL 容量拒绝**：与 `PM_MAX_SEGMENTS`、`PM_SL_COUNT` 一样属于
  配置期拒绝；默认配置（segment 4 KiB、`PM_MAX_SEGMENTS=64`、`PM_FL_MAX=24`）不受影响。
- **doc 口径**：`alloc` 不再是 O(1)（README 与头文件同步更正）。

## 4. 未完成 / 未验证项

1. **ESP32-S3 实机：已验证（2026-09-11 补做）。** 固件按 `a9232c7` 重建并烧录，
   启动日志 `App version: a9232c7`（ESP-IDF 从 git 派生，与提交号一致）、
   `ELF file SHA256: 42cdce502...`（与本地产物 `esptool image_info` 输出一致）、
   `Compile time: Sep 11 2026 21:15:07`。实机输出
   `1102255 checks, 0 failures`、`=== suite PASSED (rc=0) ===`，
   13 组基础 + R1–R21 全部 PASS；`chip: model=9 rev=0.2 cores=2`、
   启动空闲内部堆 93,680 B、元数据 27,192 B。
   复现脚本已入库：`tests/serial_cap.py`（见 README 的 ESP32 章节）。
   > 版本对应关系：设备侧记录的是构建时提交 `a9232c7`。其后只有 `d746738`
   > （仅改 `README.md`、本文档与新增 `tests/serial_cap.py`，`src/` 与
   > `tests/suite.cpp` 未变），因此实机结论对当前 HEAD 同样成立；
   > 再次烧录会显示新提交号，但被测代码是同一份。
2. **模型对拍未在 ESP32 运行**：`esp32/main/CMakeLists.txt` 只编译 `tests/suite.cpp`。
   若要把模型测试纳入设备侧，需要同时加入 `tests/model.cpp` 并调用
   `pondmerge_run_model()`（注意设备侧静态内存预算与 `PM_MAX_OBJECTS=256`）。
   这是本轮唯一「host 有、设备没有」的覆盖。
3. **cppcheck 的 `knownConditionTrueFalse` 抑制**：`bump_epoch()` 与
   `next_generation()` 的两处回绕分支是真实可达的（uint32/uint16 回绕），属工具
   误报，已带理由抑制。若将来更换为能正确做值域分析的工具，应删除抑制。
4. **`validate()` 成本上升**：新增覆盖审计把最坏复杂度从 `O(live × free)` 提到
   `O((live + free)²)`，README 已如实声明。设备侧可观察到的代价：本轮 2000 op 压测
   的 `max_compact_us` 为 **2487 µs**（第一轮固件同档为 1176 µs，约 2.1 倍），来源是
   precheck 全量审计 + `finalize_layout` 后的 `validate` 调用。仍在文档 §14
   「不保证整理硬实时」的边界内；若产品需要更紧的维护窗口，需要把覆盖审计改成
   一次排序遍历（受「无动态分配」约束，需固定 scratch 数组）。
5. **§9.3 的更严格目标未做**：任务书建议「从 pool start 按物理块头走到 pool end，
   用 `covered == capacity` 校验」。本轮实现的是等价但更弱的表述：并集间隙
   `gap < PM_MIN_BLOCK` 且三部分求和等于 capacity（理由写在 `validate()` 的注释里
   ——朴素的「每个空闲块恰好填满一个块间空隙」在 `free()` 之后不成立）。物理头逐块
   串行校验需要额外的固定 scratch，未做。

## 5. 验收状态快照（收口时）

| 命令 | 结果 |
|---|---|
| `tests/run_host.sh 10000`（Debug） | 5,391,417 checks, 0 failures + 模型 466,859 checks, 0 failures |
| `tests/run_host.sh --release 10000` | 5,391,417 checks, 0 failures + 模型 466,859 checks, 0 failures |
| `tests/run_host.sh --san 3000` | 1,490,428 checks, 0 failures + 模型 466,859 checks, 0 failures |
| `tests/run_host.sh --cppcheck` | 退出码 0，0 告警 |
| `tests/config_matrix.sh` | SL 2/4/8/16 + FL 31 通过；SL 32 / FL 32 编译期拒绝；FL 容量上限 16/24 均按预期拒绝与接受 |
| ESP32-S3 实机（2000 op） | **1,102,255 checks, 0 failures**；`App version a9232c7`、ELF SHA256 与本地产物一致；13 组基础 + R1–R21 全 PASS |

设备记录的完整字段（任务书 §12.3）：芯片 ESP32-S3（model=9, rev=0.2, 双核）、
串口 `/dev/ttyACM0`（Espressif USB JTAG/serial debug unit，控制台走
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`）、固件 233,280 B、烧录时间 2026-09-11 21:15、
ESP-IDF v6.0.2、gnu++17、`PM_MAX_OBJECTS=256`、Auto Zone 256 KiB。

测试组清单：13 组基础（指导书 §18 + typed API）+ R1–R21 + 配置矩阵 + 参考模型对拍。

## 6. 关键文件导览

- `src/core.cpp`
  - `validate()`：三段审计（live 链 / bins / 并集覆盖），整数化寻址，含「为什么不能
    写成『空闲块恰好填满块间空隙』」的说明。
  - `finalize_layout()`：块间与池尾 slack 的 poison 写入。
  - `free()`：前向合并依赖上述 poison。
  - `check_ref()` / `desc_block_consistent()`：描述符-块关系的一致性拒绝。
  - `init()`：FL 容量契约。
  - `pool_maintainable()` / `compact()` / `merge()` / `split()`：统一的入口状态契约。
  - `get_stats()` / `bins_find()`：越界游标判界。
- `include/pondmerge/pondmerge.hpp`：维护契约段（入口状态/借用/失败恢复/成功），
  复杂度说明。
- `tests/suite.cpp`：R15–R21 在文件尾部。
- `tests/model.cpp`：参考模型对拍（固定 seed `0x51ED2701`，失败打印 seed + 操作轨迹）。
- `tests/config_limits.cpp` + `tests/config_matrix.sh`：配置契约。
- `docs/`：架构说明、代码指导书、v1 任务书、v2 任务书、HANDOVER_v2、本文档。

## 7. 给接手者的提示

- 改 `split()` 的搬移顺序前，先读执行段注释里的两趟证明；v2 任务书 §4.2 伪代码的
  「上半区全部升序」在其自身前提下不成立。
- 改 `validate()` 的覆盖审计前，先读 §6 里那条注释：`free()` 只合并物理相邻块，
  slack 会把两个空闲块隔开，所以不能要求「每个空闲块恰好填满一个块间空隙」。
- `precheck_pool()` 故意不校验 `prev_size` 链（merge 中间态的链未重写）；这是设计
  边界，不是遗漏。
- **跑设备测试必须持续读串口**：控制台是 USB-Serial-JTAG，宿主不读时发送队列会填满
  并阻塞 `printf`，现象是「测试停在小分组数不再前进」，很容易误判成死锁或性能问题。
  用 `tests/serial_cap.py`（它同时负责复位，脚本名 `serial_cap.py`）。
- **复位方式要选对**：正常启动复位 = IO0 保持高（`DTR=False`）+ 脉冲 EN（`RTS`）；
  把 IO0 拉低会进入下载模式（`boot:0x23 (DOWNLOAD(USB/UART0))`），串口只会打印
  `waiting for download`，看起来像设备无响应。
- 设备侧的 `App version` 由 ESP-IDF 从 git 派生，**烧录前必须先提交**，否则版本会带
  `-dirty` 后缀、失去与提交号的对应关系。

## 8. 剩余问题清单（2026-09-11 收口，按优先级）

### A. 任务书条目未完全覆盖（需要补测试）

1. **§11.1 条目 4「并发模型测试」只做到单线程**。R16 覆盖了 `borrow_count` /
   `active_borrows` 与状态转换的**单线程**矩阵，但 `pause` 与新 `borrow` 之间
   「检查-翻转」竞态没有真并发测试。库的契约是单所有者 + 静默维护期（`alloc/free/
   resolve` 本身不承诺多线程安全），所以唯一值得真并发验证的是
   `borrow_begin/borrow_end/pause/compact` 的锁边界——这需要在双核 + FreeRTOS 上做，
   ESP32-S3 正合适。**建议**：设备侧新增任务对（任务 A 反复 borrow/borrow_end，
   任务 B 反复 pause/compact），断言「Paused 期间不会有新的 borrow 成功」，循环 N 次。
2. **§12.2「长时间重复 init/deinit」没有专项压力**。R14 只做了 2~3 次
   `deinit → init` 循环。**建议**加一个固定 seed、约 200 次 init/deinit 的循环测试
   （host 侧开销很小）。
3. **ASan 档只跑了 3000 op**（Debug/Release 各跑了 10000）。`--san 10000` 未跑，属时间
   成本取舍；若怀疑内存类问题应补跑。

### B. 本轮的已知取舍

见 §4.2（模型对拍未上设备）、§4.3（cppcheck 抑制待复核）、§4.4（`validate()` 成本
2.1 倍与 `max_compact_us` 上升）、§4.5（§9.3 的更严格物理头校验未做）。

### C. 接口与能力边界（不是缺陷，但调用方必须知道）

1. **`get_stats()` 的 `largest_free_block == 0` 有两种含义**：真的没有空闲块，或遇到
   损坏链表而拒绝报告。调用方无法区分「空池」与「元数据损坏」。**建议**给 `PoolStats`
   增加状态位，或让 `get_stats()` 返回 `Status`。
2. **`free()` 与 poison 不变式的隐性耦合**：前向合并依赖「池内未覆盖区的首字为 0」
   （由 `finalize_layout()` 写入，含块间与池尾）。任何绕过 `finalize_layout()` 直接写
   池内裸数据的内部路径都会破坏它，而当前没有防御性校验。**建议**（可选）在 `free()`
   里对 `next` 处的块大小做一次界内校验。
3. **零长对象被拒**：`desc_block_consistent()` 把 live 且 `size == 0` 视为元数据损坏
   （`alloc` 也早已拒绝 `size == 0`）。若将来要支持零长对象，需同时调整这两处。
4. **FL 容量契约与 `PM_MAX_SEGMENTS` 的相互作用**：`init()` 现在拒绝「最大块 ≥
   2^PM_FL_MAX」的 zone。默认 `PM_MAX_SEGMENTS=64` + segment 4 KiB 时 zone 上限仅
   256 KiB，远低于 2^24，因此该检查在默认配置下是**防御性**的；要用更大的 zone 必须
   同时提高 `segment_size`（例如 128 KiB × 64 = 8 MiB，仍低于 16 MiB 上限）。
5. **单所有者并发契约**：`alloc/free/resolve/get_stats` 不承诺多线程安全，锁只保证状态
   翻转与借用计数的原子性。这是设计边界（架构文档 §4），不是待办。

### D. 工程与流程

1. `docs/` 尚缺第一轮的《PondMerge_v1_修复注意事项.md》（本轮只入库了 v2 任务书、
   代码指导书、架构说明）。
2. 设备侧压力次数硬编码 2000（`esp32/main/main.cpp`），host 默认 10000；不一致是有意的
   （设备 RAM 与耗时），若要让设备跑满需改 `main.cpp` 或引入编译期宏。
3. 全仓共 9 处 cppcheck 抑制：3 处既有 `dangerousTypeCast`、6 处本轮新增
   （2× `knownConditionTrueFalse` 回绕、1× 测试框架、2× `nullPointerRedundantCheck`、
   1× `constParameterCallback`），都写了理由；更换静态分析工具时应逐条复核。
4. `tests/serial_cap.py` 依赖 pyserial（ESP-IDF 的 python env 自带）；换环境需自行安装。
5. 设备侧没有跑参考模型（§4.2），因此「host 有、设备没有」的覆盖目前只有这一项。

### 已确认满足的验收项（供对照）

- §12.2 的「固定 seed 的 10000+ 随机压力」：`tests/suite.cpp` 的随机压测用固定 seed
  `12345`，Debug/Release 各跑 10000 op；模型对拍另有固定 seed `0x51ED2701`。
- §12.2 的「ESP32 多次复位后重复运行」：本轮共 3 次复位抓取，3/3 为
  `=== suite PASSED (rc=0) ===`。
- §12.2 的 split crossing 专项、配置矩阵、故障注入均已落地（R13/R15、config_matrix.sh、
  R18/R20/R21）。
