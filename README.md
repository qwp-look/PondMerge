# PondMerge v1

**English overview: [README.en.md](README.en.md)**

面向无 MMU 的 32 位 MCU（C++17 子集：无异常 / 无 RTTI / 无动态分配 / 不依赖
OS 线程）的**用户态托管内存系统**。在一段固定 Auto Zone 上实现：TLSF 风格
分离适配分配器、稳定逻辑引用（`pm_ptr`）、RAII 物理指针借用、调用方显式触发
的池内压缩 / 池合并 / 池拆分，以及只读的整理建议分析。

实现依据《PondMerge v1 代码指导书》与《PondMerge v1 架构说明》。

> **英文 `README.en.md` 刻意只做概览层，深挖一律指向本文件与 `docs/`** ——
> 避免两份文档长期漂移。

## 快速入口

| 我想要… | 去哪里 |
|---|---|
| 英文概览（一页读完） | [README.en.md](README.en.md) |
| 5 分钟跑起第一个示例 | [QUICKSTART.md](QUICKSTART.md) |
| 完整 API / 生命周期 / 并发契约 | [docs/USAGE_GUIDE.md](docs/USAGE_GUIDE.md) |
| 何时整理、如何解读建议 | [docs/COMPACTION_POLICY.md](docs/COMPACTION_POLICY.md) |
| 跑可视化 Demo（浏览器 + 设备） | [examples/](examples/) 与 [docs/DEMO_REQUIREMENTS.md](docs/DEMO_REQUIREMENTS.md) |
| 审计与不变量证据 | [docs/AUDIT_LEDGER.md](docs/AUDIT_LEDGER.md) |
| **实测性能数字与仪器限制** | [bench/RESULTS.md](bench/RESULTS.md) |
| 历史轮次报告 | [docs/HANDOVER_v20.md](docs/HANDOVER_v20.md)（含 v2–v19 索引） |

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
    suite.cpp       验收测试套件（基础 1–13 + R1–R55，host 与 ESP32 共用）
    model.cpp       参考模型对拍（固定 seed，独立预言机；host）
    concurrency_esp32.cpp  双核借用/暂停/整理锁边界测试（仅 ESP32 构建）
    main.cpp        主机 runner（套件 + 模型）
    config_smoke.cpp    每个 TLSF 配置编译一次的冒烟测试
    config_limits.cpp   FL 容量契约（zone 上限拒绝 / 上限以下接受）
    serial_cap.py       ESP32 串口抓取（正常启动复位 + 完整日志/测试输出）
    run_host.sh     构建 + 运行（--release / --san / --cppcheck / --configs）
    config_matrix.sh    配置矩阵（SL 2/4/8/16、FL 31、非法配置编译期拒绝、
                        PM_MIN_BLOCK=32 变体）
    consumer_smoke.sh   消费路径冒烟（CMake 子目录 / find_package 两种集成
                        端到端构建并运行）
examples/           Demo、真实场景参考与协议回归
    sensor_pipeline.cpp 真实场景集成参考：传感器节点（pinned DMA 环 + 消息历史
                        + 大块导出触发建议/整理流程），host/设备同一份源码，
                        自带载荷完整性/账目/结构三重自检（CI 断言）
    sensor_pipeline_esp32/  上例的设备端 IDF 工程（复用验收固件的组件）
    host_demo.cpp       Host demo 进程（stdin 命令 -> stdout JSON 快照）
    demo_server.py      HTTP 服务 + 浏览器 UI（--host 子进程 / --serial 设备）
    esp32_demo/         ESP32 demo 固件（独立 IDF 工程，固定脚本场景）
    protocol_smoke.py   Demo 协议回归（95 项检查，无浏览器依赖）
    http_smoke.py       HTTP 层回归（15 项检查）
    owner_probe.cpp     Advice owner 门控违约诊断示例
bench/              可复现基准 + 实测结果（见 README.md 与 RESULTS.md）
    bench_timer.h       计时仪器：按批计时 + 自报 + 拒绝不可支持的逐次计时
    alloc_latency.cpp   alloc/free 与 live 数的标度（含对历史 core 的 A/B）
    validate_scaling.cpp  validate / get_stats 标度
    churn_overhead.cpp  一对 free/alloc 里有多少是分配器、多少是测试夹具
                        （两行差分互证；设备端三行分解的前置）
    fragmentation.cpp   碎片治理 A/B（同一分配器，compact 开/关）
    compaction_window.cpp  整理窗口分布：512 次 compact() 的逐次计时与百分位（设备端）
    host_insn.cpp       host 侧 free+alloc 的指令数（callgrind 差分计数）
    esp32/              同一份基准源码的设备端 IDF 工程（独立于验收固件）
esp32/              IDF 验收工程，多目标（默认 ESP32-S3；经典 ESP32 见 sdkconfig.defaults.esp32）
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
metadata_bytes = 72 + 476 × PM_MAX_POOLS + 106 × PM_MAX_OBJECTS + 4（Release 构建）
                 ↑ 其中 106 = 64（ObjectDesc）+ 34（维护计划 scratch）+ 8（地址序打包键与空闲块表）
                 Debug 构建再加 9 字节（advice owner 门控：上下文 id + 标志）
```

**这条闭式公式是 x86-64 的，不能照搬到 32 位目标。** `ObjectDesc` 里有一个
指针，所以在 ESP32-S3 上它是 52 B 而不是 64 B，系数量级因此不同：目标上每对象
约 **90 B**（52 描述符 + 38 计划 scratch；v17 起含地址序 scratch），而不是 98 B。实测过的设备配置：

| 平台 | `PM_MAX_OBJECTS` | `PM_MAX_POOLS` | `metadata_bytes` |
|---|---|---|---|
| x86-64 | 256 | 16 | 34,828（Release 实测；Debug 34,837） |
| **ESP32-S3（32 位）** | **256** | **16** | **30,729（实测，2026-09-25 验收固件）** |
| x86-64 | 1024 | 16 | 116,236（Release 实测） |

**要算 RAM 预算就用运行时的 `global_stats().metadata_bytes`**，它对任何 ABI 都是
精确上界；闭式公式只是图示，不是契约。下表 6 组配置全部为 2026-09-25 的 Release
实测值（x86-64，g++ 15.2，公式逐一吻合）：

| `PM_MAX_OBJECTS` | `PM_MAX_POOLS` | `metadata_bytes`（实测） | 适用 |
|---|---|---|---|
| 64 | 2 | 7,812 | 极小目标 |
| 128 | 4 | 15,548 | 小型 MCU |
| 256 | 4 | 29,116 | 小型 MCU（推荐起点） |
| **256** | **16** | **34,828** | **ESP32-S3 验收固件所用配置** |
| 512 | 8 | 58,156 | 中型 |
| 1024 | 16 | 116,236 | 默认（Host / 大内存目标） |

读法：

- **默认 1024/16 约吃掉 113.5 KiB（116,236 B）静态 RAM。** 多数 MCU 承受不起，
  务必下调。把 `PM_MAX_OBJECTS` 设为 256 可降至 **30 KiB（ESP32-S3 实测
  30,729 B；x86-64 同配置为 34 KiB）**。
- 预算以 `metadata_bytes` 为准、把它当**上界**用：链接器实际放置的 `.bss` 可能
  因 GCC 的节放置（toplevel reorder / 节锚定）与该计数相差正负几 KB——2026-09-25
  在 512/8 与 1024/16 档实测到 `.bss` 反而**更小** 2–4 KB。宁可多算 4 KiB，不要
  少算。
- 该字段在 v1.0.0 之前**漏计了整理建议缓存**（随 `PM_MAX_POOLS` 增长，实测少报
  128 B @2 pools 到 960 B @16 pools），v17 起已计入；本表此前的 4 行旧值
  （27,064 等）就是漏计期的产物，2026-09-25 全部重测更正——**引数字前先跑
  `global_stats()` 复核**。

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
| `alloc` | **O(SL bin 链长)**，上界 O(zone_size / PM_MIN_BLOCK) | TLSF 位图定位到 bin 后链内 first-fit（R2），再加 O(1) 追加到 live-slot 表与 O(1) 损坏筛查（R38/R44）。`PM_ZERO_INIT` 另加 O(size) 清零。**不是严格 O(1)**，但**已与 live 对象数无关** —— 见下方说明。 |
| `free` | **O(1)** | 合并前只读证明：自身块头、prev_size 链、后继块、邻块 bin 成员资格与互逆链接（R24）。损坏时 `CorruptMetadata` 且零副作用。 |
| `pause` / `resume` | O(1) | 单次状态翻转。 |
| `compact` / `split` | O(objects + moved_bytes)，另加 O(objects log objects) 恢复地址序 | 只读规划（有界收集 + heapsort）+ 按序搬移与重建。 |
| `merge` | O(objects + free_blocks + moved_bytes) | 两池只读审计（live-slot 表 / 描述符 / 统计 / bins）+ 合并区间规划 + 不可失败执行；规划失败两池逐字节不变（R22）。 |
| `validate` | O((live_objects + free_objects) log(live_objects + free_objects)) | live 块与 binned 空闲块按地址序各扫一次（归并）：前缀最大 end、gap 归账、同起点重复都在同一趟里完成；所有遍历有步数上限。改前是二次（**全库唯一剩余的超线性路径**，设备实测指数 1.70），改后指数 **0.98**、快 **13.1×**。**v19 起二次回退已删除**：binned 空闲块数超过 live+1 本身就是损坏（free() 会合并物理相邻空闲块），validate 直接在 O(n) 内拒绝（R57）——此前损坏可以把 validate 扣在二次回退里 11.3 秒（host 1 MiB 实测）。 |
| `get_stats` | O(free_blocks) | 步数上限；损坏链表有限返回，`valid = 0` 与"真的没有空闲块"可区分（R18/R29）。 |
| `borrow_begin` / `resolve` / `borrow_end` | O(1) | 描述符校验（含池范围证明，R23）+ 一次描述符读取。`borrow_end` 的 token 校验与递减在同一临界区（R25）；失败输出指针必为空（R26）。 |
| `analyze_compaction` | O(objects log objects + free_blocks) | 打包模拟精确估算搬迁对象数/字节数（R35）+ 计数器审计（损坏即 `INVALID_METADATA`）。 |

#### `alloc` 为什么与 live 对象数无关了

早先 `alloc` 把新描述符**按地址序插入** live 链，这一步被实测为 **≈100% 的 alloc
总成本，且随 live 数线性增长**——同一份基准源码编译到改动前/后两个 core 修订的
A/B：live=16 时 58.0 → 38.0 ns，live=1024 时 **844.2 → 38.4 ns（22×）**
（见 `bench/RESULTS.md` §1）。地址序只在维护路径上被需要，于是改为：**alloc 做
O(1) 追加**，地址序由每次维护调用在冷路径上一次性重建（heapsort）。改后
alloc 在 live 16→1024 全程 **x1.01（完全平坦）**，`PM_MAX_OBJECTS=256` 下
36.7 ns 且同样平坦。

> **数字口径提示**：以上数字产自 `bench/bench_timer.h`。早先版本的数字来自
> 一个未量化的计时仪器（探针主机 `RDTSC` 被拦截，单次时钟读取 ~9,400 ns，
> 而被测操作只有 ~40 ns）。形状结论不变，绝对值已被重测取代。

代价与取舍（完整记录在 `docs/AUDIT_LEDGER.md`）：

- **不变量搬迁**：live 链不再是"始终有序的地址序表"，而是一个**无序的活对象袋**；
  地址序在维护入口由 `collect_live_sorted()` 重建。
- **检测边界随之移动**：alloc 不再检测 live 链损坏（它不再遍历该链）。环链等损坏改由
  每个维护入口与 `validate()` 在有界时间内拒绝，且**零副作用**（R29 已按新语义重写）。
- 对调用方 API 无变化，错误码集合不变。

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

// 部分整理（v1.2；详见 COMPACTION_POLICY.md）：目标 + 预算的有界整理
pm::CompactionRequest part{};
part.requested_size = 4096;                  // 整理到 4 KiB 连续空间可分配
part.max_move_bytes = 8192;                  // 搬移预算 8 KiB
pm::compact(pool, &part);
pm::CompactionRequest part{};                // v1.2 部分整理：目标 + 预算
part.requested_size = 4096; part.max_move_bytes = 8192;
pm::compact(pool, &part);                    // 有界暂停，见 COMPACTION_POLICY.md
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
set(PM_MAX_OBJECTS 256)   # 约 113.5 KiB → 约 30 KiB 静态 RAM（ESP32-S3 实测 30,729 B）
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(your_app)
```

已在 **ESP-IDF v6.0.2 的 esp32s3 与 esp32 两个目标**上实测构建通过（含
`PM_MAX_OBJECTS` 覆盖生效）。注意组件名取自**目录名**，所以克隆目录须是小写的
`pondmerge`，否则 `REQUIRES pondmerge` 会报 unknown name。

⚠️ 若把**仓库根**当 IDF 组件用：仓库名是 `PondMerge`（大写），组件名会随之变成
`PondMerge`。详见"集成方式"一节的目录名说明。

### 2. CMake 子目录

```cmake
add_subdirectory(external/PondMerge)
target_link_libraries(your_app PRIVATE pondmerge::pondmerge)
```

### 3. 安装后用 find_package

```sh
# PM_* 预算在库的 configure 一步定型（编译进导出的 target）：
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/your/prefix -DPM_MAX_OBJECTS=256
cmake --build build && cmake --install build
```

```cmake
find_package(pondmerge 1.0 REQUIRED)
target_link_libraries(your_app PRIVATE pondmerge::pondmerge)
```

> **`PM_MAX_OBJECTS` 等 PM_* 预算在两条纯 CMake 路径上的覆盖方式不同**，且覆盖
> 失败无任何告警（静默回到 1024 对象 ≈ 116 KiB 元数据）：
> - **路径 2（add_subdirectory）**：在 `add_subdirectory` **之前**写普通
>   `set(PM_MAX_OBJECTS 256)`（与上面 IDF 写法相同；需要本库 3.21+ 的
>   cmake_minimum_required——CMP0126，旧策略下 CACHE set 会抹掉你的普通变量，
>   已实测并写进 `tests/consumer_smoke.sh` 的断言）。
> - **路径 3（find_package）**：预算在**安装库时**定型，consumer 侧的
>   `set()` / `-D` **都改不了**已安装的导出 target——必须在库的 configure
>   步骤传 `-DPM_MAX_OBJECTS=...`（如上）。

路径 2/3 由 `tests/consumer_smoke.sh` 端到端验证（配置 → 构建 → 安装 →
`find_package` → 链接 → 运行；外加 add_subdirectory 路径与覆盖生效断言），
CI 每次执行。

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
| R54–R55（第十五轮） | alloc 拒绝诊断的两半：R54 = 深层链损坏仍被拒绝但改报 `NoSpace`（且 `validate()` 仍报损坏）+ 位图/头不一致仍为 `CorruptMetadata`；R55 = 位图/头一致性检查的其余两条规则（头部游标出池、层级位与子级位不一致） |
| R36–R53（第十四轮，R45 空缺） | 元数据筛查缺陷修复的红测：alloc 的 order_head 界（R38）、check_ref 链接界（R39）、create_pool 段窗口证明（R43）、池几何 zone 上限（R44）、mid-gap slack、post-deinit 拒绝、重复 live 地址、split/free 故障注入、耗尽矩阵、validate 第三段、advice 负路径、Paused 恢复、零长尾访问、generation 回绕、16 B gap 边界、溢出带 |
| 参考模型 | 固定 seed 随机 alloc/free/compact/merge/split，独立预言机校验 live 数、payload、池归属、字节账目与可分配性 |
| 双核并发（设备） | 借用/暂停/整理的 SMP 锁边界（`tests/concurrency_esp32.cpp`） |
| 协议/HTTP 冒烟 | `examples/protocol_smoke.py`（95 项）、`examples/http_smoke.py`（15 项） |

### 当前验收状态

> host 三行由 `scripts/gates.sh` 每轮收口复验，最新数字见
> [docs/HANDOVER_v20.md](docs/HANDOVER_v20.md)；下表是当前轮的记录（v20 实测，
> 2026-09-25，host 默认几何 4 KiB；设备构建为 1 KiB 几何，见 v20 §2）。

| 档位 | 结果 |
|---|---|
| Host Debug（10000 op） | 5,454,163 checks, 0 failures |
| Host Release（10000 op） | 5,454,170 checks, 0 failures |
| ASan/UBSan（3000 op） | 1,553,174 checks, 0 failures |
| 参考模型对拍 | 466,859 checks, 0 failures |
| cppcheck（warning/style/performance） | exit 0，三类计数 0/0/0 |
| 配置矩阵 | PASSED |
| 协议回归 / HTTP 回归 | 104 / 27 checks, 0 failures |
| `src/core.cpp` 覆盖率 | 96.75% 行 / 80.07% 分支执行（Debug 档；Release 档 96.91% 行 / 82.06% 分支选取；下限 85% 强制。v17 降至 92.5%、R56/R57 与 v20 拉回，逐行归账见 AUDIT_LEDGER §7.1） |
| libFuzzer（有界运行） | 无崩溃、无 sanitizer 发现 |
| **v20 夹具参数化后（当前几何：64 段 × 1 KiB = 64 KiB zone，静态 DRAM，无 PSRAM）** ||
| ESP32-S3 (n16r8) 实机，App version `v1.0.0-33-g079e4e5` | 套件 **939,054**（R1–R57 全量）+ 双核并发 28 + 模型 466,859 checks，全部 0 failures；复位重跑计数一致 |
| 经典 ESP32 (D0WDQ6 v1.1) 实机，App version `v1.0.0-32` | 套件 **939,054**（首次在经典 ESP32 上运行）+ 双核并发 28 + 模型 466,859 checks，全部 0 failures |
| 历史（v20 前基线，256 KiB zone）：S3 `v1.0.0-28` 套件 1,140,849；经典 ESP32 `v1.0.0-31` 套件未运行（编不进）——详见 HANDOVER_v18/v19 |

> 经典 ESP32 的记录已于 2026-09-25 在当前提交（`6d83194`）上重取（v19 收口）：
> 此前它停在 `v1.0.0-6`，晚于 v17 算法重构的适用性只能靠推断；现在这条推断已由
> 实测取代。三个平台（host / S3 / 经典 ESP32）的 model 对拍再次同为 466,859
> checks。
>
> S3 的套件记录在 v1.0.0-4（`a5e68b6`）之后曾长期断档：v16/v17 期间套件变大且
> 静态 DRAM 放不下 256 KiB zone，固件链接失败（v17 §7），v18 把 zone 移入 PSRAM
> 并修掉三个测试的尺寸假设后才恢复。**教训：新增的套件测试若从未上过设备，README
> 的设备行必须标注断档区间，而不是默认连续。**
>
> **v20 终结了断档的根源**：夹具几何参数化（`PM_TEST_SEG_BYTES`，64 段 × 1 KiB =
> 64 KiB zone）后，套件对两个目标都完整可编可跑——经典 ESP32 的 SKIPPED 时代
> 结束，S3 也回到内部 DRAM（PSRAM 不再是套件的依赖，v18 预告的"内存等级"代价
> 随之消除）。

> **两个目标平台现在跑同一组测试、同一几何**（v20 起）：套件的段**数量**仍是 64
> ——测试 [10] 断言"16 个池 × 4 段正好填满 zone"，测试 [2] 要求一个 32 段的池——
> 但段**尺寸**变成了编译期旋钮 `PM_TEST_SEG_BYTES`（设备构建 = 1 KiB，zone
> 64 KiB；host 默认 4 KiB，套件在两种几何下都验证通过）。历史背景：v17/v18 期间
> 套件曾把 zone 钉死在 64×4 KiB = 256 KiB，经典 ESP32 因此编不进套件（链接溢出
> 126,744 B，SKIPPED 时代），S3 则把 zone 挪进了 OCTAL PSRAM（PSRAM 模式必须是
> **OCTAL**——n16r8 是八线件，配 QUAD 会 abort 并最终把 USB 拖下线，实测）。
> v20 的参数化把这两个权宜都退役了：两个目标都在**内部 DRAM** 上跑全套件，验收
> 夹具重新代表 bench 所测的内存等级。
>
> 历史教训保留：夹具的每个字节级断言都必须从几何常量推导——v20 为此扫掉了
> 约 30 处字面量（16,376 B 窗口、3584 B 边界跨越、6016/8192 偏移……），全部改为
> `PM_TEST_SEG_BYTES` 的表达式，并在 1 KiB / 4 KiB 两种几何下分别全绿。
>
> 这条分叉的**直接动因**是项目的锁语义声明：Host 的 `PM_LOCK` 是空操作，SMP 证据
> 只能来自双核设备测试，而在此之前它只在**一颗**芯片上成立过。经典 ESP32 是**双核
> LX6**（S3 是 LX7），`portMUX` 实现与 cache 都不同；同一份
> `tests/concurrency_esp32.cpp` 在第二套架构上通过，才是这条声明更强的证据。
>
> `bench/esp32/`（把基准源码编到设备）**同样已支持两个目标**：同一份基准源码、
> 同一组参数（只有 region 因 DRAM 从 192 KiB 降到 112 KiB），所以两片芯片的数字
> 可比。结论见 `bench/RESULTS.md` §6——`alloc`/`free` 的平坦性（两片都是 x0.99）、
> 碎片 A/B（两片都是 50/50 → 1/50）都在第二片复现，而且**独立 TLSF 基线在经典
> ESP32 上持续 churn 掉了 21/200（10%）而 PondMerge 开整理掉了 0**（S3 那次该计数
> 因一次同尺寸拒绝被标 CONFOUNDED，不可引用；这次可引用）。
>
> 设备侧那个 ~20×/周期的差距也已**实测定性**（`bench/RESULTS.md` §5.7）：S3 的
> cache 未命中计数器读数**恰好为 0**——"内存与代码访问"的旧解释被直接证伪。真实
> 原因是**指令数与流水线依赖**：目标上一次 free+alloc 要执行 **1,528 条指令**
> （x86-64 上同一负载 757 条，callgrind 实测），CPI 1.41、其中 24% 是依赖气泡。

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

## ESP32 上机（两个目标）

基础配置在 `esp32/sdkconfig.defaults`（面向 ESP32-S3，也是 `idf.py build` 的默认
目标；v20 起套件夹具是 64 KiB zone（1 KiB 段），不再依赖 PSRAM。历史教训保留：
这块板如果启用 PSRAM，模式必须是 **OCTAL**——n16r8 是八线件，配 QUAD 会 init
失败进入重启循环并把 USB-Serial-JTAG 拖下线，v18 实测）。第二个目标通过
`SDKCONFIG_DEFAULTS` 追加
自己的覆盖文件——ESP-IDF **不会**自动读 `sdkconfig.defaults.<target>`，而且
defaults 列表里靠后的文件**改不动** `CONFIG_IDF_TARGET`（它对 target 采用首个
匹配），所以切换必须显式 `set-target`：

```sh
source ~/esp/activate-idf.sh        # ESP-IDF v6.0.2

# ESP32-S3（主目标，套件 + 并发 + model）
cd esp32
rm -f sdkconfig && idf.py set-target esp32s3
idf.py -B build build
idf.py -B build -p /dev/ttyACM0 flash monitor      # USB-Serial-JTAG

# 经典 ESP32（并发 + model；套件不编译，原因见验收表下方说明）
rm -f sdkconfig
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" \
    idf.py set-target esp32
idf.py -B build build
idf.py -B build -p /dev/ttyUSB0 flash monitor       # UART0 + CH340 桥接
```

实机记录（S3 基线 `21f94da`；经典 ESP32 于 v20 在当前提交上重取，复位重跑一致）：

```
ESP32-S3 (n16r8) · /dev/ttyACM0（USB-Serial-JTAG）· v1.0.0-33-g079e4e5（v20，当前，zone 64 KiB 纯 DRAM）
  suite（基础 13 组 + R1–R57，2000 ops）：939,054 checks, 0 failures PASSED
  双核并发：28 checks, 0 failures
  参考模型对拍（4000 ops）：466,859 checks, 0 failures PASSED

经典 ESP32 (D0WDQ6 v1.1) · /dev/ttyUSB0（CH340，160 MHz）· v1.0.0-32（v20，当前）
  suite（基础 13 组 + R1–R57，2000 ops，zone 64 KiB）：939,054 checks, 0 failures PASSED
  双核并发：28 checks, 0 failures
  参考模型对拍（4000 ops）：466,859 checks, 0 failures PASSED
```

> S3 的 1 KiB 几何实机行已补齐（2026-09-25 晚）：三平台在**同一几何、同一配置**
> 下的套件计数逐位一致（939,054），model 同为 466,859——"三平台同数"叙事在
> 新几何下完整保持。

三个平台（host、S3、经典 ESP32）的参考模型对拍都是**同一个 466,859 checks**：
这是同一份确定性差分测试在三种架构上逐项走完了同样多的判定。并发的
rounds 计数则每次运行都不同（调度相关），而 **28 checks 恒为 0 failures**——这正是
该测试要的性质：被核验的性质稳定，调度才是可变的。

`App version` 由 ESP-IDF 从 git 派生，用于提交号核对；ELF SHA256 须与本地产物
一致。历史轮次的实机记录见各 HANDOVER 文档。

设备侧注意：**宿主必须持续读串口**——不读时发送队列会填满并阻塞 `printf`，
表现为"测试卡在小分组数"。`tests/serial_cap.py` 在两种控制台上都实测可用：S3 是
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`（`/dev/ttyACM0`），经典 ESP32 板载没有该
外设、控制台是 UART0 经 CH340 桥接（`/dev/ttyUSB0`，需要用户在 `dialout` 组）。
它的正常启动复位（IO0 保持高、只脉冲 EN）对两者都有效；把 IO0 拉低会进入下载模式。
`tests/serial_cap.py` 还接受一个可选的停止标记，因为基准固件的结束行与验收固件不同。

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
| [CONTRIBUTING.md](CONTRIBUTING.md) | **给贡献者与 AI 助手**：铁律、怎么跑门禁、怎么加设备目标、文档地图 |
| [README.en.md](README.en.md) | 英文概览（一页；深挖指向本文件与 docs/） |
| [QUICKSTART.md](QUICKSTART.md) | 快速入门：最小示例 → compact 全流程 → 常见错误 |
| [docs/USAGE_GUIDE.md](docs/USAGE_GUIDE.md) | 完整使用指南：三区模型、API 契约表、并发、错误码、复杂度 |
| [docs/COMPACTION_POLICY.md](docs/COMPACTION_POLICY.md) | 整理建议语义、判定矩阵、阈值、标准整理流程 |
| [docs/DEMO_REQUIREMENTS.md](docs/DEMO_REQUIREMENTS.md) | Demo 组成、快照协议 v1(.1)、JSON 子集、应答分类、验收 |
| [docs/AUDIT_LEDGER.md](docs/AUDIT_LEDGER.md) | 审计账本：不变量 → 代码位置 → 证明 → 测试 |
| [bench/README.md](bench/README.md) | 基准方法、公平性规则、**计时仪器规则**与引用数字的必备条件 |
| [bench/RESULTS.md](bench/RESULTS.md) | 全部实测数字、仪器限制、负结果、方案 A 的 A/B |
| docs/架构说明.md | 目标架构契约 |
| docs/PondMerge_v1_代码指导书.md | 接口与内存布局的原始设计 |
| docs/PondMerge_v1_repair_task.md / v2 | 第一/二轮修复任务书 |
| docs/HANDOVER_v2–v20.md | 各轮收口报告（v20 为最新：夹具几何参数化轮——经典 ESP32 首跑全套件） |
