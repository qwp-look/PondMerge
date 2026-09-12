# PondMerge v1

面向无 MMU 的 32 位 MCU（C++17 子集：无异常 / 无 RTTI / 无动态分配 / 不依赖
OS 线程）的**用户态托管内存系统**。在一段固定 Auto Zone 上实现：TLSF 风格
分离适配分配器、稳定逻辑引用（`pm_ptr`）、RAII 物理指针借用、调用方显式触发
的池内压缩 / 池合并 / 池拆分，以及只读的整理建议分析。

实现依据《PondMerge v1 代码指导书》与《PondMerge v1 架构说明》。

## 快速入口

| 我想要… | 去哪里 |
|---|---|
| 5 分钟跑起第一个示例 | [QUICKSTART.md](QUICKSTART.md) |
| 完整 API / 生命周期 / 并发契约 | [docs/USAGE_GUIDE.md](docs/USAGE_GUIDE.md) |
| 何时整理、如何解读建议 | [docs/COMPACTION_POLICY.md](docs/COMPACTION_POLICY.md) |
| 跑可视化 Demo（浏览器 + 设备） | [examples/](examples/) 与 [docs/DEMO_REQUIREMENTS.md](docs/DEMO_REQUIREMENTS.md) |
| 审计与不变量证据 | [docs/AUDIT_LEDGER.md](docs/AUDIT_LEDGER.md) |
| 历史轮次报告 | [docs/HANDOVER_v11.md](docs/HANDOVER_v11.md)（含 v2–v10 索引） |

## 目录结构

```
include/pondmerge/
    pondmerge.hpp   公共 API（Status / RawRef / pm_ptr / pm_access / advice ...）
    pm_config.h     编译期参数 + 编译期契约 static_assert
    pm_port.h       平台层：临界区 + 微秒计时 + 上下文 id（host / PM_ESP32）
src/
    internal.h      内部结构（Pool / ObjectDesc / TlsfBins / FreeBlock）
    core.cpp        核心实现（单一翻译单元）
tests/
    suite.cpp       验收测试套件（基础 1–13 + R1–R35，host 与 ESP32 共用）
    model.cpp       参考模型对拍（固定 seed，独立预言机；host）
    concurrency_esp32.cpp  双核借用/暂停/整理锁边界测试（仅 ESP32 构建）
    main.cpp        主机 runner（套件 + 模型）
    config_smoke.cpp    每个 TLSF 配置编译一次的冒烟测试
    config_limits.cpp   FL 容量契约（zone 上限拒绝 / 上限以下接受）
    serial_cap.py       ESP32 串口抓取（正常启动复位 + 完整日志/测试输出）
    run_host.sh     构建 + 运行（--release / --san / --cppcheck / --configs）
    config_matrix.sh    配置矩阵（SL 2/4/8/16、FL 31、非法配置编译期拒绝）
examples/           Demo 与协议回归
    host_demo.cpp       Host demo 进程（stdin 命令 -> stdout JSON 快照）
    demo_server.py      HTTP 服务 + 浏览器 UI（--host 子进程 / --serial 设备）
    esp32_demo/         ESP32 demo 固件（独立 IDF 工程，固定脚本场景）
    protocol_smoke.py   Demo 协议回归（95 项检查，无浏览器依赖）
    http_smoke.py       HTTP 层回归（15 项检查）
    owner_probe.cpp     Advice owner 门控违约诊断示例
esp32/              ESP32-S3 (n16r8) IDF 验收工程（v6.0.2，组件强制 gnu++17）
docs/               架构说明、代码指导书、各轮任务书与交接文档（见文末索引）
```

## 内存布局

- **Chaos Zone** / **Free Zone**：PondMerge 不触碰，由调用方自行规划。
- **Auto Zone**：由 `pm::init(Config)` 注册的一段固定内存（建议由链接脚本
  提供），内部按 `segment_size`（默认 4 KiB）划分给各个 Pool。
- **Metadata**：所有池/对象元数据位于静态存储（`internal::g()` + 维护计划
  scratch），永不放入 Auto Zone，也永不参与搬迁。

块格式：每块 8 字节头（`[0..4)` 自身大小 | 空闲位，`[4..8)` 前块大小），
最小块 16 字节，对齐 8 字节，负载 8 字节对齐。

### 元数据（静态 RAM）预算

这是把 PondMerge 放到小目标上时**第一件要算的事**：元数据全部在静态存储里，
不占 Auto Zone，但也**不会被释放**。`PM_MAX_OBJECTS` 是绝对主导项。

`global_stats().metadata_bytes` 精确等于（实测 6 组配置全部吻合）：

```
metadata_bytes = 72 + 476 × PM_MAX_POOLS + 98 × PM_MAX_OBJECTS     （Release 构建）
                 ↑ 其中 98 = 64（ObjectDesc）+ 34（维护计划 scratch）
                 Debug 构建再加 9 字节（advice owner 门控：上下文 id + 标志）
```

实测值（`metadata_bytes` 与链接器看到的真实 `.bss` 对照）：

| `PM_MAX_OBJECTS` | `PM_MAX_POOLS` | `metadata_bytes` | 真实 `.bss` | 适用 |
|---|---|---|---|---|
| 64 | 2 | 7,296 | 7,296 | 极小目标 |
| 128 | 4 | 14,520 | 14,528 | 小型 MCU |
| 256 | 4 | 27,064 | 27,072 | 小型 MCU（推荐起点） |
| **256** | **16** | **32,776** | **32,768** | **ESP32-S3 验收固件所用配置** |
| 512 | 8 | 54,056 | 54,048 | 中型 |
| 1024 | 16 | 108,040 | 108,032 | 默认（Host / 大内存目标） |

读法：

- **默认 1024/16 约吃掉 105 KiB 静态 RAM。** 多数 MCU 承受不起，务必下调。
  在 ESP-IDF 里把 `PM_MAX_OBJECTS` 设为 256 可降至约 31 KiB。
- `metadata_bytes` 已与真实 `.bss` 吻合到 ±8 字节；两者差异只来自对齐填充。
  **预算时仍留一点余量。**
- 该字段在 v1.0.0 之前**漏计了整理建议缓存**（随 `PM_MAX_POOLS` 增长，实测少报
  128 B @2 pools 到 960 B @16 pools），现已计入，并以上表代替原来的口头描述。

## 关键语义

1. 只有放入 Auto Zone 的对象被管理；整理 / 合并 / 拆分全部由调用方显式触发。
2. 普通物理指针只能在借用期间使用；借用期间相关池不可搬迁（`pm_access`
   RAII，池级 `borrow_count` + 对象级 `active_borrows` 双计数）。`resolve` /
   `peek` 是**不计数的高级校验接口**：返回的裸指针只在池静默期内立即使用，
   禁止跨越任何维护入口或保存；常规访问请走 `try_borrow()` / `pm_access` 或
   `pm_ptr::operator->`（表达式级 RAII borrow）。
3. `pm_ptr<T>` 是逻辑引用（对象槽位 + generation + pool_hint + offset），搬迁后
   下次使用时经描述符惰性重解析。**没有地址缓存**：每次借用做完整 O(1) 校验
   （范围 / generation / 状态 / 池）加一次描述符读取。
4. `pm_local_ptr`（默认）绑定创建时的池，对象换池后解析返回 `PoolChanged`；
   `pm_cross_ptr` 必须通过 `pm_as_cross()` / `pm_cross_ref()` 显式生成。从
   `CROSS_HINT` 原始引用构造 local 指针会被构造函数无效化（R4）。
   **绑定边界（R27）**：local 绑定在**解析时**强制，而非构造时——调用者可以
   用拷贝来的 `RawRef` 构造 local 指针（有意的低层能力），但伪造的 pool hint
   在每次 borrow/resolve/free 都被 `PoolChanged` 拒绝；`RawRef` 因此是可伪造
   的低层句柄，`pm_cross_ref(raw)` 是文档化的 unsafe 跨池构造入口。
5. generation 只在释放并复用槽位时递增（跳过 0），防 ABA；`address_epoch` 在
   搬迁 / 换池时递增（跳过 0）。分配失败回滚不触碰 generation。
6. 压缩为“地址顺序稳定打包”：先只读规划（pinned 屏障、越界、对齐全部校验），
   再按地址升序 `memmove`，最后一次性重建头 / 空闲块 / TLSF bins。事务方案 A：
   规划失败整体放弃（池恢复入口状态、零改动），执行阶段不可失败。
7. 合并只处理物理相邻池，源池 segment 所有权转移给目标池后统一压缩重建；
   拆分把 segment 对齐边界作为虚拟 pinned 屏障，两侧独立打包、事务式提交，
   规划失败时恢复入口状态且零改动。
8. `PM_DMA` / `PM_EXTERNAL` 自动升级为 `PM_PINNED`，永不搬迁。
9. 所有失败路径返回明确 `Status`，绝不强行搬迁。只有根引用（offset == 0）
   可以释放对象；子对象视图的 free/destroy 一律拒绝；`pm_destroy` 失败时
   不清空指针（`PoolChanged` 表示对象仍存活、只是 local 绑定失效）。

### 复杂度（最坏情形）

| 操作 | 最坏复杂度 | 说明 |
|---|---|---|
| `alloc` | O(SL bin 链长 + live_objects)，上界 O(zone_size / PM_MIN_BLOCK) | TLSF 位图定位到 bin 后链内 first-fit（R2）；新描述符按地址序插入 order 链（有界防环，R29）。**不是严格 O(1)**。 |
| `free` | O(1 + 邻块空闲 bin 链长)，上界 O(zone_size / PM_MIN_BLOCK) | 合并前只读证明：自身块头、prev_size 链、后继块、邻块 bin 成员资格与互逆链接（R24）。损坏时 `CorruptMetadata` 且零副作用。 |
| `pause` / `resume` | O(1) | 单次状态翻转。 |
| `compact` / `split` | O(objects + moved_bytes) | 地址序只读规划（`walk_order` 有限遍历）+ 按序搬移与重建。 |
| `merge` | O(objects + free_blocks + moved_bytes) | 两池只读审计（order 链 / 描述符 / 统计 / bins）+ 合并区间规划 + 不可失败执行；规划失败两池逐字节不变（R22）。 |
| `validate` | O(live_objects × free_blocks) | live 块与 binned 空闲块两两重叠检查与 gap 归账；所有遍历有步数上限。 |
| `get_stats` | O(free_blocks) | 步数上限；损坏链表有限返回，`valid = 0` 与“真的没有空闲块”可区分（R18/R29）。 |
| `borrow_begin` / `resolve` / `borrow_end` | O(1) | 描述符校验（含池范围证明，R23）+ 一次描述符读取。`borrow_end` 的 token 校验与递减在同一临界区（R25）；失败输出指针必为空（R26）。 |
| `analyze_compaction` | O(objects + free_blocks) | 打包模拟精确估算搬迁对象数/字节数（R35）+ 计数器审计（损坏即 `INVALID_METADATA`）。 |

### 并发契约：单所有者 + 静默维护期

| 操作 | 并发承诺 |
|---|---|
| `alloc` / `free` / `resolve` / `get_stats` / `validate` | **单 owner 上下文**，不承诺多线程并发安全 |
| `borrow_begin` / `borrow_end` | 内部锁 + token 保护 |
| `pause` / `resume` | 内部锁保护 |
| `compact` / `merge` / `split` | 单 owner 串行（不同池之间也不并发）；入口与最终提交持锁 |
| Advice（analyze / poll / 阈值访问器） | owner 上下文 API（Debug 构建 owner 门控，跨上下文调用即断言诊断，R34） |
| DMA / ISR / 其他任务 / 外部库 | 由**调用方**在维护前停止并排干——库不发现外部持有者 |

`PM_LOCK` 只保护库已知的借用计数与维护状态发布；Host 构建把 `PM_LOCK`
编译为空操作，Host 运行**不能证明任何锁语义**，SMP 证据只来自双核设备测试
（`tests/concurrency_esp32.cpp`）。

### 可搬移类型是显式 opt-in

`pm_is_relocatable<T>` 默认 `false`，与 `std::is_trivially_copyable` 无关：
它是项目定义的“可整体 `memmove` 且语义不变”契约。含 Auto Zone 裸指针、
DMA / 寄存器地址、自引用、外部所有权的类型默认禁止搬移，必须显式特化注册后
才能 `pm_make`。`set_destroy_fn()` 只提供销毁，不会让类型变得可搬移。

## 公共 API 速览

```cpp
pm::init(cfg);  pm::deinit();
pm::create_pool(id, segments);  pm::destroy_pool(id);
pm::pause(id);  pm::resume(id);  pm::compact(id);
pm::merge(source, target);      pm::split(source, new_segments, out);

pm::alloc(pool, size, align, flags, tag, ref);  pm::free(ref);
pm::borrow_begin(ref, size, align, ptr);        pm::borrow_end(ref);

auto pod = pm::pm_make<Pod>(pool);                     // 可搬迁（需 opt-in trait）
auto pin = pm::pm_make_pinned<Widget>(pool, args...);  // 非平凡类型，带析构
auto buf = pm::pm_alloc_buffer(pool, n, pm::PM_ZERO_INIT);
auto acc = pod.try_borrow();                // Result<pm_access<T>>
pod->field = x;                             // operator-> 持有表达式级借用
auto cross = pm::pm_as_cross(pod);
pm::pm_destroy(pod);

pm::get_stats(pool);  pm::validate(pool);

// 整理建议（只读；详见 docs/COMPACTION_POLICY.md）
pm::CompactionRequest req{2000, 8, 0, 0};
pm::CompactionAdvice a = pm::analyze_compaction(pool, &req);
if (a.verdict == pm::CompactionVerdict::COMPACT_RECOMMENDED) { /* 安排静默期 */ }
pm::set_compaction_thresholds({100, 512});   // 碎片阈值（可查询可配置）
```

## 集成方式

四种消费路径，**同一份源码，不复制**——根 `CMakeLists.txt` 是一个双形态入口
（按 `ESP_PLATFORM` 分流到 IDF 组件注册或普通 CMake 库）。

### 1. ESP-IDF 组件（MCU 用这条）

把仓库放进工程的 `components/` 下即可。

> **目录名必须是小写 `pondmerge`。** IDF 的组件名来自**目录名**，而仓库名是
> `PondMerge`——同名克隆会让 `REQUIRES pondmerge` 报 `unknown name`（已实测）。

```sh
cd <你的工程>/components
git clone https://github.com/qwp-look/PondMerge.git pondmerge
```

```cmake
idf_component_register(SRCS "main.cpp" REQUIRES pondmerge)
```

在工程 CMakeLists 里缩小元数据预算（数值见上一节的表）：

```cmake
set(PM_MAX_OBJECTS 256)   # 约 105 KiB → 约 31 KiB 静态 RAM
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(your_app)
```

已在 **ESP-IDF v6.0.2 / esp32s3** 实测构建通过（含 `PM_MAX_OBJECTS` 覆盖生效）。

### 2. CMake 子目录

```cmake
add_subdirectory(external/PondMerge)
target_link_libraries(your_app PRIVATE pondmerge::pondmerge)
```

### 3. 安装后用 find_package

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/your/prefix
cmake --build build && cmake --install build
```

```cmake
find_package(pondmerge 1.0 REQUIRED)
target_link_libraries(your_app PRIVATE pondmerge::pondmerge)
```

路径 2/3 由 `tests/consumer_smoke.sh` 端到端验证（配置 → 构建 → 安装 →
`find_package` → 链接 → 运行），CI 每次执行。

### 4. 手工编译（最小路径）

```sh
g++ -std=c++17 -Iinclude your_app.cpp src/core.cpp -o your_app
```

> **尚未发布到 Espressif Component Registry。** `idf_component.yml` 已就位，但发布
> 需要仓库所有者本人的 Espressif 账号与 token（命令为 `compote component upload`），
> 不属于自动化范围。

## 构建与测试

```sh
tests/run_host.sh              # Host Debug（-O1 -g，PM_DEBUG=1）+ 模型对拍
tests/run_host.sh 10000        # 指定压力次数
tests/run_host.sh --release    # Host Release（-O3 -DNDEBUG -DPM_DEBUG=0）
tests/run_host.sh --san        # ASan + UBSan
tests/run_host.sh --cppcheck   # cppcheck（warning/style/performance）
tests/run_host.sh --configs    # 配置矩阵（等价于 tests/config_matrix.sh）

g++ -std=c++17 -Wall -Wextra -Werror -Iinclude -Isrc \
    examples/host_demo.cpp src/core.cpp -o build/host_demo   # Host Demo 进程
python3 examples/protocol_smoke.py build/host_demo            # 协议回归（95 项）
python3 examples/http_smoke.py build/host_demo                # HTTP 层回归（15 项）
```

### 测试组

| 组 | 覆盖 |
|---|---|
| 基础 1–13 | 连续分配释放复用、随机压力+整理、引用稳定、Busy、generation/double-free/越界、pinned 屏障、DMA/外部不搬迁、合并/拆分后引用可解析、耗尽、大小对齐边界、validate+损坏注入、类型化 C++ API（指导书 §18） |
| R1–R12（第一轮） | 槽位回滚、同 bin first-fit、destroy 保留指针、CROSS_HINT 拒绝、可搬移 opt-in、子对象不可释放、维护失败零改动、validate 有限时返回、整数上限、静默期契约、拆分布局、generation/epoch 矩阵 |
| R13–R21（第二轮） | 拆分跨界搬移顺序、init 先校验、拆分歧界几何、维护状态矩阵、描述符-块一致性、get_stats 有界、销毁回调语义、compact 故障注入、逐域损坏检测 |
| R22–R28（第三轮） | merge 事务故障（逐字节零改动）、池外描述符拒绝、free 物理头验证先于回调、borrow_end 单临界区、resolve 输出清空、local 绑定语义、维护仅在最终提交发布 |
| R29–R30（第四轮） | 扩展故障矩阵（互逆链接/跨 bin 重复/segment/运行时状态/alloc 防环）、alloc 失败清空输出 |
| R31–R34（第六–八轮） | 整理建议（只读、判定矩阵、阈值、抑制）、INVALID_REQUEST、poll 变化键、owner 门控 |
| R35（第九轮） | 建议估算精确性（池首空闲块场景）+ 计数器审计故障注入 |
| 参考模型 | 固定 seed 随机 alloc/free/compact/merge/split，独立预言机校验 live 数、payload、池归属、字节账目与可分配性 |
| 双核并发（设备） | 借用/暂停/整理的 SMP 锁边界（`tests/concurrency_esp32.cpp`） |
| 协议/HTTP 冒烟 | `examples/protocol_smoke.py`（95 项）、`examples/http_smoke.py`（15 项） |

### 当前验收状态（提交 `e6bd516`）

| 档位 | 结果 |
|---|---|
| Host Debug（10000 op） | 5,409,618 checks, 0 failures |
| Host Release（10000 op） | 5,409,625 checks, 0 failures |
| ASan/UBSan（10000 op） | 5,409,618 checks, 0 failures |
| 参考模型对拍 | 466,859 checks, 0 failures |
| cppcheck（warning/style/performance） | exit 0，三类计数 0/0/0 |
| 配置矩阵 | PASSED |
| 协议回归 / HTTP 回归 | 95 / 15 checks, 0 failures |
| ESP32-S3 实机（2000 op，App version `baf20e8`） | 套件 1,120,456 + 双核并发 28 + 模型 466,859 checks, 全部 0 failures |

## 可视化 Demo

浏览器实时展示池泳道（MOVABLE/PINNED/FREE/SLACK 着色 + 对象明细）、整理
建议面板、整理前后对象级对比（搬迁/新建/删除，含 epoch 与摘要断言）：

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude -Isrc \
    examples/host_demo.cpp src/core.cpp -o build/host_demo
python3 examples/demo_server.py --host build/host_demo --port 8080
# 浏览器打开 http://127.0.0.1:8080

python3 examples/protocol_smoke.py build/host_demo   # 协议回归（无需浏览器）
```

ESP32 demo（固定脚本场景、只读展示；设备端整理由固件触发，不支持浏览器
命令）：见 `examples/esp32_demo/README.md`。协议与数据模型规范：
`docs/DEMO_REQUIREMENTS.md`。

## ESP32-S3（n16r8）上机

```sh
source ~/esp/activate-idf.sh        # ESP-IDF v6.0.2
cd esp32
idf.py -B build set-target esp32s3  # 首次
idf.py -B build build
idf.py -B build -p /dev/ttyACM0 flash monitor
```

设备端跑同一套验收套件（压力 2000 次，Auto Zone 256 KiB @ 内部 SRAM，
`PM_MAX_OBJECTS=256`；`suite.cpp` 与 host 共用）。当前实机记录
（App version `baf20e8`，与本表验收状态同行）：

```
chip: model=9 rev=0.2 cores=2 · 串口 /dev/ttyACM0（USB-Serial-JTAG）
suite（基础 13 组 + R1–R35，2000 ops）：1,120,456 checks, 0 failures PASSED
双核并发（200 轮 pause/compact/resume + 双核 borrower）：28 checks, 0 failures
参考模型对拍（4000 ops）：466,859 checks, 0 failures PASSED
max_live=55 max_borrow=1 max_moved=30960 meta=27704
```

`App version` 由 ESP-IDF 从 git 派生，用于提交号核对；ELF SHA256 须与本地产物
一致。历史轮次的实机记录见各 HANDOVER 文档。

设备侧注意：控制台为 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`，**宿主必须
持续读串口**——不读时发送队列会填满并阻塞 `printf`，表现为“测试卡在小分组
数”。`tests/serial_cap.py` 使用正常启动复位（IO0 保持高、只脉冲 EN）；把
IO0 拉低会进入下载模式。

## Debug / Release

`PM_DEBUG=1`（默认）启用 `PM_ASSERT` 与 Advice owner 门控（跨上下文调用
advice 即断言诊断）；`PM_DEBUG=0` 时断言编译为空，但 generation、状态与边界
校验全部保留（文档 §15）。损坏元数据在任何构建下都返回 `CorruptMetadata`。

## v1 非目标（文档 §20）

不修复任意裸指针、不在 ISR / 后台自动整理、不自动判断 DMA 完成、不自动分析
对象内部指针、不扩大 Auto Zone、不提供硬件隔离、不保证压缩硬实时。

## 文档索引

| 文档 | 内容 |
|---|---|
| [QUICKSTART.md](QUICKSTART.md) | 快速入门：最小示例 → compact 全流程 → 常见错误 |
| [docs/USAGE_GUIDE.md](docs/USAGE_GUIDE.md) | 完整使用指南：三区模型、API 契约表、并发、错误码、复杂度 |
| [docs/COMPACTION_POLICY.md](docs/COMPACTION_POLICY.md) | 整理建议语义、判定矩阵、阈值、标准整理流程 |
| [docs/DEMO_REQUIREMENTS.md](docs/DEMO_REQUIREMENTS.md) | Demo 组成、快照协议 v1(.1)、JSON 子集、应答分类、验收 |
| [docs/AUDIT_LEDGER.md](docs/AUDIT_LEDGER.md) | 审计账本：不变量 → 代码位置 → 证明 → 测试 |
| docs/架构说明.md | 目标架构契约 |
| docs/PondMerge_v1_代码指导书.md | 接口与内存布局的原始设计 |
| docs/PondMerge_v1_repair_task.md / v2 | 第一/二轮修复任务书 |
| docs/HANDOVER_v2–v11.md | 各轮收口报告（v11 为最新） |
