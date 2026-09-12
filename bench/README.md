# bench/ — 性能与碎片基准

三个可复现基准，用来把"性能/复杂度能不能优化"从猜想题变成工程题。
**所有数字都必须在目标平台上实测后才能对外引用**；Host 数据只能说明形状
（线性/二次/常数），不能外推绝对值。

---

## 1. `alloc_latency.cpp` — 分配延迟与标度

**问题**：`alloc` 声明的复杂度是 O(bin 链长 + live_objects)。哪一项主导？

**方法**：**填充法** —— 逐个分配 N 个混合尺寸对象并**逐个计时**。live 数从 0
单调增长到 N，所以按 live 数分桶后的均值**就是 alloc 的成本曲线**，无混淆项。
另设**稳态段**：满 live 下随机位置 free + 同尺寸重新 alloc，free 与 alloc 分别计时。

**为什么用随机位置**：单调递增的插入点会恒定落在地址序链表**末尾**（= O(n) 最坏
情形）；随机起始位置给出期望 n/2。两者的比值本身就是结论的一部分 ——
若"最坏 ÷ 2 ≈ 随机"，则说明该操作的耗时几乎全在走链上。

```sh
g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc \
    bench/alloc_latency.cpp src/core.cpp -o build/bench_alloc
./build/bench_alloc
# 对比不同 TLSF 配置：
for sl in 4 8 16; do
  g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -DPM_SL_COUNT=$sl -Iinclude -Isrc \
      bench/alloc_latency.cpp src/core.cpp -o build/bench_alloc_sl$sl
  ./build/bench_alloc_sl$sl
done
```

## 2. `validate_scaling.cpp` — 结构审计的成本标度

**问题**：`validate` 声明的边界是 O((live + free)²)。常数有多大？值不值得优化？

**方法**：填充 N 个对象后**释放每隔一个**（制造约 N/2 个空闲块），分别计时
`validate()` 与 `get_stats()`。

```sh
g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc \
    bench/validate_scaling.cpp src/core.cpp -o build/bench_val
./build/bench_val
```

## 3. `fragmentation.cpp` — 碎片治理到底有没有用

这是本目录的**核心实验**，回答项目最根本的价值主张。

**变体**（同一分配器、同一负载、仅差是否整理）：

| 变体 | 说明 |
|---|---|
| A | PondMerge，**关闭**整理 |
| B | PondMerge，**仅当 probe 分配失败时**触发 `compact` |

**负载**：固定 seed；先填充稳定种群，然后长时间 churn（随机释放一个、按尺寸
分布重新分配），每 N 步发起一次**大块 probe 申请**。碎片的杀伤力正体现在
这个 probe 上 —— 它也是本库存在的理由。

**头条指标**：`probe FAILURES`（A 与 B 的对比）。

### 公平性规则（本基准强制，缺一即不可引用数字）

1. **整理绝不从 churn 路径触发，只由 probe 失败触发。** 否则会整理的变体可能
   保住另一个变体已失去的种群，两个变体就不再跑同一份负载。
2. 两个变体按同一顺序取同一固定 seed PRNG。
3. `churn_refusals` 会被报告。负载已调参使其为 0；**非零即代表该次运行被混淆，
   不得引用**（输出里会直接标 `*** CONFOUNDED RUN ***`）。

### 为什么这里没有"简单分配器"对照组

**刻意不带。** 拿自己写的分配器当对手，赢了也没有意义。决定性实验是
**同一分配器 compact 开/关**的 A/B —— 它直接度量本库的核心特性，且无稻草人风险。
与**独立**分配器的对比放到设备端做（见下），那里用的是 FreeRTOS heap_4。

```sh
g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Iinclude -Isrc \
    bench/fragmentation.cpp src/core.cpp -o build/bench_frag
./build/bench_frag
# 调参（默认值不保证一定能压出失败；A 若零失败说明负载太松）：
#   -DPM_BENCH_POPULATION / -DPM_BENCH_PROBE_BYTES / -DPM_BENCH_STEPS / -DPM_BENCH_PROBE_EVERY
```

### 设备端（ESP32）与独立基线

在 ESP32 上运行时，同一文件还可对比 **FreeRTOS heap_4**（ESP-IDF 默认分配器）
在**等大专用区域**上的表现 —— 这是真正的第三方基线，不是自制品。
设备端测量需要把本文件接入 IDF 工程并用 `tests/serial_cap.py` 抓串口。

---

## 引用任何数字时必须同时给出

- 平台（Host x86-64 / ESP32-S3）与构建档（Debug / Release / 优化级别）
- 编译期配置（`PM_MAX_OBJECTS`、`PM_SL_COUNT`、`PM_FL_MAX`）
- 基准的参数（population / probe / steps）
- 若是 `fragmentation.cpp`：`churn_refusals` 是否为 0

**负结果同样要保留。** 实测排除掉的候选（例如"调大 `PM_SL_COUNT` 能缩短 bin 内
走链"）比"没测"有价值得多 —— 它能防止后续为无收益的改动去破坏已被验证的代码。
