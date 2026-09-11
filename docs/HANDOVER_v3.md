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

1. **ESP32-S3 实机未验证**：本轮只做了**固件编译验证**（`idf.py -B build build`
   成功，产物 233,280 B，sha256 前 16 位 `e0fc7f7a7bfd8d63`），
   原因是手上没有可用开发板（无 `/dev/ttyACM*` 或 `/dev/ttyUSB*`）。
   因此**不得**声称 v2 已实机验证。重烧后必须核对启动日志 `App version` 与提交号
   一致再采信串口 PASS。
2. **模型对拍未在 ESP32 运行**：`esp32/main/CMakeLists.txt` 只编译 `tests/suite.cpp`。
   若要把模型测试纳入设备侧，需要同时加入 `tests/model.cpp` 并调用
   `pondmerge_run_model()`（注意设备侧静态内存预算与 `PM_MAX_OBJECTS=256`）。
3. **cpcheck 的 `knownConditionTrueFalse` 抑制**：`bump_epoch()` 与
   `next_generation()` 的两处回绕分支是真实可达的（uint32/uint16 回绕），属工具
   误报，已带理由抑制。若将来更换为能正确做值域分析的工具，应删除抑制。
4. **`validate()` 成本上升**：新增覆盖审计把最坏复杂度从 `O(live × free)` 提到
   `O((live + free)²)`，README 已如实声明。当前随机压测（`PM_MAX_OBJECTS=1024`，
   实际 live ≤ 160）下未见性能问题；若要在大对象数下频繁调用，需要改成一次排序遍历
   （受限于「无动态分配」约束，需要固定 scratch 数组）。
5. **§9.3 的更严格目标未做**：任务书建议「从 pool start 按物理块头走到 pool end，
   用 `covered == capacity` 校验」。本轮实现的是等价但更弱的表述：并集间隙
   `gap < PM_MIN_BLOCK` 且三部分求和等于 capacity（见 §2 注释里的理由——朴素的
   「每个空闲块恰好填满一个块间空隙」在 `free()` 之后不成立）。物理头逐块串行校验
   需要额外的固定 scratch，未做。

## 5. 验收状态快照（收口时）

| 命令 | 结果 |
|---|---|
| `tests/run_host.sh 10000`（Debug） | 5,391,417 checks, 0 failures + 模型 466,859 checks, 0 failures |
| `tests/run_host.sh --release 10000` | 5,391,417 checks, 0 failures + 模型 466,859 checks, 0 failures |
| `tests/run_host.sh --san 3000` | 1,490,428 checks, 0 failures + 模型 466,859 checks, 0 failures |
| `tests/run_host.sh --cppcheck` | 退出码 0，0 告警 |
| `tests/config_matrix.sh` | SL 2/4/8/16 + FL 31 通过；SL 32 / FL 32 编译期拒绝；FL 容量上限 16/24 均按预期拒绝与接受 |
| ESP32-S3 构建 | `idf.py -B build build` 成功（编译 `tests/suite.cpp`，含 R1–R21） |
| ESP32-S3 实机 | **未验证**（无开发板，见 §4.1） |

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
