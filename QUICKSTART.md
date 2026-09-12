# PondMerge v1 快速入门

## 1. PondMerge 解决什么问题，不解决什么问题

**解决**：在无 MMU 的 MCU 上，长期运行导致的内存碎片会让分配失败。PondMerge
把一块**固定容量**的 Auto Zone 划分为池，对象用稳定的逻辑引用（`pm_ptr`）
访问；当碎片积累时，由**调用方显式触发** `compact()` 把可搬迁对象压实——
逻辑引用在下次使用时自动解析到新地址。

**明确不解决**：

- 普通裸指针（`T*`）不会自动跟随对象搬迁；
- DMA、ISR、外部库持有的地址库无法发现，必须由**调用方**先停止；
- 对象内部的裸指针不会被自动修正（可搬迁类型需要项目审计后显式注册）；
- 不提供通用多线程安全（单 owner + 静默维护期模型，见并发契约）；
- pinned/DMA/external 对象不参与搬迁，可能阻挡整理或留下碎片。

## 2. Host 环境依赖与构建

依赖：g++（支持 C++17）、make 或直接 g++ 命令行；可选 cppcheck、Python 3。

```sh
cd /home/qwp/code/PondMerge
tests/run_host.sh 10000      # 验收套件（Debug，含参考模型对拍）
tests/run_host.sh --release 10000
```

编译一个自己的程序：

```sh
g++ -std=c++17 -Wall -Wextra -Iinclude -Isrc \
    my_app.cpp src/core.cpp -o my_app
```

## 3. ESP32 环境依赖与构建

依赖：ESP-IDF v6.0.2（`~/esp/activate-idf.sh`）、ESP32-S3。

```sh
source ~/esp/activate-idf.sh
cd esp32 && idf.py -B build build
idf.py -B build -p /dev/ttyACM0 flash
```

## 4. 最小初始化代码

```cpp
#include "pondmerge/pondmerge.hpp"

static uint8_t zone[64 * 1024] __attribute__((aligned(16)));

int main() {
    pm::Config cfg{zone, sizeof(zone), 4096};   // Auto Zone：固定容量
    if (pm::init(cfg) != pm::Status::Ok) return 1;
    // ... 使用后：
    pm::deinit();
}
```

## 5. 创建池和分配对象

```cpp
pm::PoolId pool{};
if (pm::create_pool(pool, 8) != pm::Status::Ok) return 1;  // 8 个 segment

pm::RawRef ref{};
pm::Status st = pm::alloc(pool, 300, 8, 0, /*user_tag=*/1, ref);
if (st != pm::Status::Ok) { /* NoSpace / InvalidRef / ... */ }
```

推荐使用类型化入口（需要先注册可搬迁 trait）：

```cpp
struct Pod { uint32_t a; uint32_t b; };
template <> struct pm::pm_is_relocatable<Pod> : std::true_type {};

auto made = pm::pm_make<Pod>(pool);          // Result<pm_local_ptr<Pod>>
if (made.ok()) { /* made.value 是 pm_local_ptr<Pod> */ }
```

## 6. 使用 pm_ptr 和 RAII borrow

```cpp
auto made = pm::pm_make<Pod>(pool);
auto pod  = made.value;

{                                  // 表达式级 RAII borrow：
    auto acc = pod.try_borrow();   //   借用期间该池不可整理
    if (acc.ok()) acc.value->a = 42;
}
pod->a = 42;                       // operator-> 等价于上面的一次性借用
```

## 7. 释放对象

```cpp
pm::Status st = pm::pm_destroy(pod);   // 成功后 pod 自动置空
// 失败（Busy：有活跃借用；InvalidRef：陈旧引用）时 pod 保留，可重试
```

## 8. 制造碎片并查询整理建议

```cpp
// 分配 A B C 后释放 B，再分配小块填补半个洞 → 产生搁浅空闲字节
pm::CompactionAdvice a = pm::analyze_compaction(pool);       // 通用建议
pm::CompactionRequest req{2000, 8, 0, 0};
pm::CompactionAdvice b = pm::analyze_compaction(pool, &req); // 针对 2000B 申请

if (b.verdict == pm::CompactionVerdict::COMPACT_RECOMMENDED) {
    // 建议整理；先满足静默期再执行 compact
}
```

建议是**只读分析**：不移动对象、不改任何状态、不自动执行整理。

## 9. 执行 compact() 的完整流程

```cpp
// 1) 停止外部访问：DMA/ISR/其他任务/外部库 —— 由调用方负责
// 2) 确认没有活跃借用（有则 compact 返回 Busy 并进入 Paused）
pm::Status st = pm::compact(pool);
if (st == pm::Status::Ok) {
    // 搬迁完成；pm_ptr 下次访问自动解析到新地址
} else if (st == pm::Status::Busy) {
    pm::resume(pool);   // 失败的 compact 会把池留在 Paused，调用方决定恢复
} else {
    // CorruptMetadata / PinnedConflict 等
}
pm::validate(pool);     // 可选：全量结构审计
```

## 10. Demo、测试和源码入口索引

| 入口 | 说明 |
|---|---|
| `examples/demo_server.py --host build/host_demo` | Host 可视化 Demo（浏览器） |
| `examples/host_demo.cpp` | Host demo 进程（命令 → JSON 快照） |
| `examples/esp32_demo/` | ESP32 demo 固件（同一协议，串口输出） |
| `tests/run_host.sh` | Host 验收套件（基础 1–13 + R1–R31 + 模型对拍） |
| `tests/suite.cpp` | 全部回归组源码（host 与设备共用） |
| `tests/model.cpp` | 参考模型对拍（独立预言机） |
| `src/core.cpp` / `include/pondmerge/pondmerge.hpp` | 实现 / 公共 API |
| `docs/USAGE_GUIDE.md` | 完整使用指南 |
| `docs/COMPACTION_POLICY.md` | 整理建议与整理策略 |
| `docs/DEMO_REQUIREMENTS.md` | Demo 与快照协议规范 |

## 11. 常见错误与排查

| 现象 | 原因 | 处理 |
|---|---|---|
| `alloc` 返回 `NoSpace` | 池满或最大连续块不足 | `analyze_compaction` 看建议；必要时 compact |
| `pm_destroy` 返回 `Busy` | 对象有活跃借用（RAII acc 未销毁） | 结束借用后重试 |
| `borrow_begin` 返回 `PoolChanged` | 对象被 merge 到其他池，local 引用失效 | 改用 `pm_as_cross()` 重建引用 |
| `borrow_begin` 返回 `InvalidRef` | generation 过期（对象已释放重建） | 引用已死，重新分配 |
| `compact` 返回 `Busy` 且池变 Paused | 有活跃借用或池已在维护态 | `resume()` 恢复；排除借用后重试 |
| compact 后旧裸指针失效 | 正常：裸指针不跟随搬迁 | 用 `pm_ptr`，或在 compact 后重新 `resolve` |
| 设备串口"卡死" | USB-Serial-JTAG 发送队列满（主机没读） | 用 `tests/serial_cap.py` 或 demo_server 持续读取 |

## 12. 最小可运行路径 vs 生产使用路径

本文档的代码是**教学最小路径**：单线程、无 DMA/ISR、直接调用公共 API。
**生产使用**必须完整阅读：

- 并发契约（单 owner + 静默维护期）——`docs/USAGE_GUIDE.md` 并发一节；
- DMA/ISR 停止义务与静默期——`docs/COMPACTION_POLICY.md`；
- pinned/外部/可搬迁类型的注册责任——`docs/USAGE_GUIDE.md`；
- Demo 的静默期"按钮"只是模拟，不代表库能发现真实外部访问者。
