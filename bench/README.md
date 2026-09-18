# bench/ — 性能与碎片基准

四个可复现基准，用来把"性能/复杂度能不能优化"从猜想题变成工程题，外加一个
**设备专属**的独立分配器基线（见 §4 末）。**所有数字都必须在目标平台上实测后
才能对外引用**；Host 数据只能说明形状（线性/二次/常数），不能外推绝对值。
完整实测结果见 `RESULTS.md`（§5 是设备端）。

---

## 0. 先读这个：计时仪器（`bench_timer.h`）

**仪器是分平台的，并且每次运行自报**。主机上 `std::chrono::steady_clock::now()`
每次调用要 ~9,400 ns；设备上 `esp_cpu_get_cycle_count()`（`mcycle` 寄存器）只要
**59 ns**。同一份基准源码因此在不同平台上处于完全不同的可信区间：主机只能说
"批量值"，设备上逐次值本身可用。任何数字都必须和它的仪器一起引用。

**探针主机上的 `std::chrono::steady_clock::now()` 每次调用要 ~9,400 ns**，
而被测操作只要 36–90 ns。原因是该 VMware 客机的 `RDTSC` 被拦截：时间戳
**数值**是对的（200 ms 延时测出 200.024 ms），但**读取**要付出一次 VM exit。
数值准确恰恰是这个缺陷能长期隐藏的原因。

实测（每个变体在**一个长区间内**调用 200,000 次，避免测量工具污染对工具的测量）：

| | 成本 |
|---|---|
| 空循环体 | 0.01 ns/次 |
| `sink += 1` | 0.44 ns/次 |
| 64 位 LCG 依赖链 | 1.01 ns/次（≈3 周期，说明 CPU 满速） |
| `__rdtsc()` | **10,244 ns/次** |
| `lfence; __rdtsc()` / `__rdtscp()` | ~9,950 ns/次 |
| `clock_gettime(CLOCK_MONOTONIC)` | 9,341 ns/次（裸 syscall 19,530 ⇒ vDSO 生效，但 vDSO 本身慢） |
| `clock()`（`CLOCK_PROCESS_CPUTIME_ID`） | **20,900 ns/次** |

空循环与 LCG 证明机器本身健康：成本在计数器指令**内部**，不在环境。结论是
**该主机上任何周期计数器指令都不可用**——因为被拦截的就是 `RDTSC` 本身，
换 `rdtscp`、加 `lfence` 都一样。

### 因此的硬规则

1. **按批计时，绝不逐次计时。** 一个区间装 N 次操作、两次时钟读取之间完成，
   仪器贡献从 `2 × call_cost` 降到 `2 × call_cost / N`。N = 16,384 时约 1 ns/op
   而非 18,800 ns/op。**这与机器无关**，所以同一份源码在 Host、普通 PC、设备上
   都给有效数字。
2. **每次运行自报仪器。** 后端、实测时钟成本、以及"本机能否支持逐次计时"的裁定
   都会打印。不能支持的运行会直说。
3. **不产出逐次计时**，除非该操作远长于时钟；那种情况下也必须标注偏置。

一个区间只含**一次**偏置，且该偏置在真实负载下比紧凑循环测得的更大
（用 184,296 B 的 memmove 标定：批量真值 2,780 ns，逐次 14,749 ns ⇒ 场景偏置
≈ 11,969 ns；而 `bench_timer.h` 的紧凑循环探针给 7,521 ns，是**偏低**估计，
所以 `RESULTS.md` 的修正值是保守的）。

---

## 1. `alloc_latency.cpp` — 分配/释放成本与标度

**问题**：`alloc` 曾声明 O(bin 链长 + live_objects)。哪一项主导？

**方法**：**定 live 数 churn** —— 在固定 live 数下释放一个对象、按同尺寸在
**随机位置**重新分配。区间装 16,384 对，约 1.3 ms，仪器占比 0.7%。
8 次试验取最小值，并同时打印中位数以便读者直接看到噪声水平。

**为什么必须随机位置**：插入走链的成本取决于新元素落在哪里；固定位置会低估它。
这也是"改动前/后"可比的前提。

**A/B 的做法**：同一份基准源码编译到**两个 core 修订**上（基准只用公共 API），
因此两次运行唯一的差异就是 core：

```sh
# 当前 core
g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc \
    bench/alloc_latency.cpp src/core.cpp -o build/bench_alloc
./build/bench_alloc
# 历史 core（例如方案 A 之前）
git show <rev>:src/core.cpp > /tmp/core_before.cpp
g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc \
    bench/alloc_latency.cpp /tmp/core_before.cpp -o /tmp/bench_before
/tmp/bench_before
```

**`free` / `alloc` 的拆分**由三段长区间的差分得到（`measure_split()`）：
`I1=(A F)^R`、`I2=(A F)^(R-1) A`、`I3=F (A F)^(R-1)`，则 `I1-I2` 恰为 L 次释放、
`I1-I3` 恰为 L 次分配，每段只含一次偏置并在相减中抵消。差分恒为 L 次操作，
所以只能靠**增大 L**提高信噪比——在 256 KiB / 1024 对象下差分落在噪声底之下，
基准会打印 `n/a` 而不是一个数。同时它**自我校验**：`pair` 由 churn 直接测、
`free+alloc` 来自差分，两者不一致的行会被标 `REJECTED`。

```sh
# TLSF 配置对比（已测：调大 PM_SL_COUNT 无收益）
-DPM_SL_COUNT=$sl
# 更小的对象表（ESP32 典型）
-DPM_MAX_OBJECTS=256 -DPM_BENCH_TARGET_LIVE=256
# 大配置（仅为取得 free/alloc 拆分；注意 zone 必须 < 2^PM_FL_MAX）
-DPM_MAX_OBJECTS=16384 -DPM_BENCH_TARGET_LIVE=16384 \
-DPM_BENCH_ZONE_BYTES=8388608 -DPM_BENCH_SEGMENT=131072
```

## 2. `churn_overhead.cpp` — 一对操作里有多少是分配器

**问题**：`alloc_latency.cpp` 的 `pair` 指标把"挑受害者槽位"的成本也算进了区间。
在 x86-64 上那是几个 ns，可以忽略；在 32 位目标上不行——`rng()` 是 64 位 LCG、
`% g_live` 是 64 位取模，两者在 Xtensa 上都是软件例程，而且就压在计时区间**内部**。
设备端 `pair` 一度给出 9.2 µs/对（≈2,160 周期），在怪罪分配器之前必须先排除仪器。
**先测量嫌疑对象，不要先假设。**

**方法**：五行长区间，每行只与另一行差一件事——

| 行 | 区间内含 |
|---|---|
| D | 循环 + 一次数组读取（测量下限） |
| C | 只有地址生成（`rng() % LIVE`），不含任何库调用 |
| A | `free`+`alloc`，地址生成**在区间外**（索引取自预生成的环形表） |
| B | `free`+`alloc`，地址生成**在区间内**（即 `alloc_latency` 的形态） |
| E / F | 只有 `alloc`（填充形态）/ 只有 `free`（随机序） |

于是 `A − D` 与 `B − C` 是同一物理量的两个**独立**估计，它们的吻合是对整个
构造的交叉验证，而不是同一式的移项。环形表由**同一个**发生器在区间外预生成，
所以 A 行的地址序列仍是真随机序列，只是它的生成被移出了测量窗口。

**E/F 的形态局限必须说清**：分配的地址由首次适配决定，所以"随机地址分配"不存在，
E 只能测**填充形态**（恰好对地址序结构最有利）；F 则用预生成的随机排列释放。
因此 **E + F ≠ A 是预期的**，它们各自给出该目标上单个操作的数量级，而不是 A 的
加性分解。

**该行是否可用是自报的**：会打印每操作的仪器偏置，并在偏置高于操作本身时直接
警告"低于噪声底，勿引用"（主机上就会触发；设备上 50 ns/区间 ⇒ 0.2 ns/op，可用）。

```sh
g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc \
    bench/churn_overhead.cpp src/core.cpp -o build/bench_churn
./build/bench_churn
# 调参：-DPM_BENCH_CHURN_LIVE / -DPM_BENCH_CHURN_OPS / -DPM_BENCH_IDX_RING
```

## 3. `validate_scaling.cpp` — 结构审计的成本标度

**问题**：`validate` 声明的边界是 O((live + free)²)。真二次吗？`get_stats` 值不值得优化？

**方法**：`validate`/`get_stats` **只读且幂等**，调用一千次池子逐字节不变，
所以无需恢复状态——区间内直接调用 K 次即可。K 由一次探针调用推算，使各行
墙钟时间接近，仪器相对占比因此恒定。

```sh
g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc \
    bench/validate_scaling.cpp src/core.cpp -o build/bench_val
./build/bench_val
```

实测：`validate` 拟合指数 **1.83（二次）**；`get_stats` 指数 1.17（线性）、
1.75 ns/块、最大规模 2.7 µs ⇒ **负结果：不值得优化**。

## 4. `fragmentation.cpp` — 碎片治理到底有没有用

本目录的**核心实验**，回答项目最根本的价值主张。

**变体**（同一分配器、同一负载、仅差是否整理）：

| 变体 | 说明 |
|---|---|
| A | PondMerge，**关闭**整理 |
| B | PondMerge，**仅当 probe 分配失败时**触发 `compact` |

**负载**：三段 —— ①用陡尺寸谱填充区段，每 PINNED_EVERY 个对象中一个设为
**pinned**（搬迁屏障）；②释放每 SCATTER_MOD 个**可搬迁**对象，留下被存活/ pinned
邻居围住的孤立空洞；③churn + probe。

**头条指标**：`probe FAILURES`（A 与 B 的对比）。**这两个计数不涉及时钟，
是刻意为之**——本基准要确立的结论不能依赖仪器。

### 公平性规则（本基准强制，缺一即不可引用数字）

1. **整理绝不从 churn 路径触发，只由 probe 失败触发。** 否则会整理的变体可能
   保住另一个变体已失去的种群，两个变体就不再跑同一份负载。
2. 两个变体按同一顺序取同一固定 seed PRNG。
3. churn 重新分配的正是它刚释放的尺寸 ⇒ **构造上不可能被拒绝**。
   `churn_refusals` 仍会报告；**非零即代表该次运行被混淆，不得引用**
   （输出里直接标 `*** CONFOUNDED RUN ***`）。

### 为什么这里没有"简单分配器"对照组

**刻意不带。** 拿自己写的分配器当对手，赢了也没有意义。决定性实验是
**同一分配器 compact 开/关**的 A/B —— 它直接度量本库的核心特性，且无稻草人风险。
与**独立**分配器的对比放到设备端做（见下），那里用的是 FreeRTOS heap_4。

### 为什么负载必须长成这样（两次失败的记录）

窄尺寸谱 → **零失败**（`free()` 会合并相邻块，根本没有碎片）；仅陡尺寸谱 →
池**自我修复**（首次适配漂移把对象压低、空闲抬高，A 在几百步内自行恢复）。
能持续维持碎片的是 **pinned** 对象——这正是 pinnded 在现实中的含义（DMA 缓冲
不能移动）。两次失败都保留在本文件的注释里。

### 计时的处理

`compact` 每次约 53 µs，而单区间偏置 ~7.5 µs（场景值 ~12 µs），所以打印的是
**原始值 + 偏置 + 修正值**三者，并**不报告逐次最大值**——该主机上一次时钟读取
实测可达 2.7 ms，此处报"最坏 compact"只会是时钟的产物而非分配器的。
**百分位属于设备端**（mcycle 免费）。

```sh
g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc \
    bench/fragmentation.cpp src/core.cpp -o build/bench_frag
./build/bench_frag
# 调参（默认值不保证一定能压出失败；A 若零失败说明负载太松）：
#   -DPM_BENCH_POPULATION / -DPM_BENCH_PROBE_BYTES / -DPM_BENCH_STEPS / -DPM_BENCH_PROBE_EVERY
```

### `esp32/` — 设备端工程、独立基线，以及三个必须知道的坑

`bench/esp32/` 是一个**独立于验收固件**的 IDF 工程，编译**同一份**基准源码
（`PM_BENCH_NO_HOST_MAIN=1` 去掉各文件的 host `main()`，由 `app_main` 调用），
所以设备数字与主机数字来自同一份代码，差异只有 sizing 宏。

**四个基准共用一块 192 KiB region。** 512 KiB SRAM 装不下四块独立区域，所以
`PM_BENCH_SHARED_ZONE=1` 让各基准把 region 声明为 `extern`，由 `bench_main.cpp`
定义；region 大小与各基准的 `PM_BENCH_*_BYTES` 来自**同一个 CMake 变量**，因此
不可能各自漂移。region 只在同一时刻被一个基准使用，且第五步（基线）在
PondMerge 用完之后接管它。

**设备端的价值不在"重复主机结论"**，而在三件主机做不到的事：

1. **绝对值**。主机是 x86-64，绝对值不可外推（见 `RESULTS.md` §5.3：每周期成本
   在设备上高约 20 倍，而一个平凡循环的周期数两边相同）。
2. **逐次计时**。`esp_cpu_get_cycle_count()`（mcycle 寄存器）每次读取 59 ns，
   `bench_timer.h` 在该平台自动切换，所以主机上必须靠差分的 `free`/`alloc` 拆分
   在这里可以**直接测**（`churn_overhead.cpp` 的 E/F 行）。
3. **独立基线**（见下）。

#### 坑 1：`pm::init` 是"每实例一次"，而基准假设自己是独立进程

四个基准各自 `init()`、**都不 `deinit()`**——在主机上每个基准是一个独立进程，
这是对的；串在同一个 `app_main` 里就不是了。而 `deinit()` 在**还有存活对象时
会拒绝**（返回 `BUSY` 且保持已初始化），于是"某个基准忘了清理"会立刻表现为
下一个基准的 `init failed`，而不是一份悄悄错误的测量。
**这不是假设：本文件第一、二版运行就是这样**（2/5 之后 3/5、4/5 全部
`init failed`，fragmentation 报出 0 次需求）。

#### 坑 2：任务看门狗会把测量打碎

一次 churn 区间是几十毫秒的纯 CPU 循环、中途没有任何阻塞调用——正是任务看门狗
要抓的形态。它打印的日志走 USB-Serial-JTAG，会给正在测量的区间注入数百微秒抖动。
首版运行触发了 3 次，`free`/`alloc` 拆分全部被交叉校验判为 `REJECTED`。
`bench/esp32/sdkconfig.defaults` 因此显式关闭任务看门狗与中断看门狗：这是专用
基准固件，不是产品固件。

#### 坑 3：串口读端必须从不停读

USB-Serial-JTAG 控制台的发送队列一满，`printf` 就会阻塞，看起来像"测试卡死"。
用 `tests/serial_cap.py`（它自己复位芯片，因此日志从 ROM banner 开始，含
`App version`，可与提交对账），并给它一个停止标记：

```sh
python3 tests/serial_cap.py /dev/ttyACM0 600 115200 "=== benchmarks done"
```

不给标记时按验收固件的最后裁决行（`=== model PASSED/FAILED`）结束。

#### 独立基线：是 TLSF，不是 heap_4

`bench/esp32/main/idf_heap_baseline.cpp`（**仅设备端**——主机上没有第三方分配器
可做对照）。用 `heap_caps_add_region_with_caps()` 把同一块 g_zone 注册成一个带
**自定义能力位**的堆，然后用**同一份负载形状与同一组常数**跑同样两段需求。

三点必须写明：

- **IDF v6.0.2 里没有 heap_4**：`components/heap/tlsf/tlsf.c` 才是它的分配器，
  即 TLSF。本文件第一版运行由 `assert failed: tlsf_free tlsf.c:630` 自己招认了
  这件事（那个断言是基线夹具里的真实 bug，已修）。**TLSF 是同一 segregated-fit
  家族的成熟生产实现**，所以这个对照比"heap_4"更有分量，不是更少。
- **公平性与 `fragmentation.cpp` 同源**：两侧的常数由 CMake 从同一处下发，防止
  漂移。但**"同尺寸重分配不可能被拒绝"是某个分配器的性质，不是定律**——TLSF
  在本次负载下拒绝了 1 次，基线因此被标 `CONFOUNDED`。基线夹具在拒绝时把该槽位
  移出追踪集合并计数，这才把一个双重释放崩溃变成了一个数字。
- **它拿不到 `validate()` 的等价物**，改用 `heap_caps_get_info()` 做归账交叉核对：
  free + allocated 应与注册区间相符，且分配器自报的最大空闲块应与经 API 测得的
  一致。这个核对做了两次才对——第一版比较的是两个**不同时刻**的读数，报出了
  一个毫无意义的 MISMATCH。

#### 顺带：sizing 报告

`bench_main.cpp` 启动时打印**目标 ABI** 的 `sizeof(ObjectDesc/Pool/TlsfBins/
GlobalState)` 与元数据三项分解。这就是发现 `README.md` 那条闭式公式是 x86-64
专属的地方：`ObjectDesc` 含一个指针，32 位目标上 52 B 而非 64 B。

---

## 引用任何数字时必须同时给出

- 平台（Host x86-64 / ESP32-S3）与构建档（Debug / Release / 优化级别）
- **计时仪器及其单次成本**（本目录任何数字都依赖这一项）
- 编译期配置（`PM_MAX_OBJECTS`、`PM_SL_COUNT`、`PM_FL_MAX`、zone 大小）
- 基准的参数（population / probe / steps / churn 区间长度）
- 若是 `fragmentation.cpp`：**两个变体的** `churn_refusals` 是否为 0
- 若是独立基线：它是 TLSF 且**本次拒绝过同尺寸重分配**（标记 `CONFOUNDED`），
  以及填充对象数与 PondMerge 不同（per-allocation 开销不同 ⇒ 不可按字节对比）
- 若是设备端：`PM_DEBUG`（基准固件为 0，验收固件为 1）、看门狗已关闭、
  使用的是共享 region 中的哪一块
- 若是逐次计时：**已减去的偏置值**及其不确定性

**负结果同样要保留。** 实测排除掉的候选（`PM_SL_COUNT` 调大、`get_stats` 优化）
比"没测"有价值得多 —— 它能防止后续为无收益的改动去破坏已被验证的代码。
