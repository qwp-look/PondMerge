# 给贡献者与 AI 助手：怎么参与、怎么验证、有哪些铁律

> 本文件面向**任何**要在这份代码上工作的人（人类或 AI 助手），写法是自足的：
> 不需要读过任何聊天记录或交接文档就能照着做。深入背景请看文末的文档地图。

## 1. 项目是什么

PondMerge 是面向**无 MMU 的 32 位 MCU** 的用户态托管内存系统：固定 Auto Zone 上的
TLSF 风格分配器 + 稳定逻辑引用 `pm_ptr` + RAII 借用双计数 + 调用方显式触发的
compact/merge/split。核心约束：**C++17 子集，无异常、无 RTTI、无动态分配**。

两个设备目标都已在真实硬件上验收：

| | ESP32-S3 (n16r8) | 经典 ESP32 (D0WDQ6) |
|---|---|---|
| 核 | LX7，双核 | LX6，双核 |
| 验收 | suite + concurrency + model | concurrency + model（**suite 不编译**，原因见下） |

## 2. 铁律（违反任何一条都会破坏这个项目最值钱的东西）

1. **无异常、无 RTTI、无动态分配**；C++17 子集。
2. **Host 与设备共用同一份** `tests/suite.cpp` 与 `tests/model.cpp`——改动必须
   两端都过门禁。
3. **复杂度声明必须诚实**。本项目自己把 `alloc` 从"近似 O(1)"更正为
   O(bin 链长 + live_objects)，后来又实测出设备侧的真实构成（`bench/RESULTS.md`
   §5.7）。不要写超出测量的承诺。
4. **未验证的事不作声称**。板子不在线就只说"构建通过，实机未验证"；
   没跑就不说"通过"；跳过的组要**大声打印** `SKIPPED (not run, not passed)`。
5. **性能数字必须带仪器与限制**。任何时间数字都要说明：哪个平台、什么构建档、
   计时仪器的单次成本、区间里有什么偏置。负结果（实测不值得优化）与正结果一样
   要保留。
6. **文档分层不要混**：设计契约（架构说明/代码指导书）· 使用契约（USAGE_GUIDE）·
   证据（`docs/AUDIT_LEDGER.md`，每条不变式都要有"代码位置 + 证明 + 测试"）·
   过程（`docs/HANDOVER_vN.md`）。修 bug 要同步更新台账与一份新的 HANDOVER。
7. **对外可见的动作（push、发 release、上传 Registry）先问维护者。**
   仓库是公开的，push 即对外可见。
   **更新（2026-09-19，维护者授权）**：push 到 main 已获常授权——host 门禁
   全绿即可直接推，无需再问；**发 release 与上传 Registry 仍先问**。
8. **假设要用测量检验**。本项目历史上至少两个"合理"的解释后来被计数器/差分
   直接证伪（见 `bench/RESULTS.md` §5.3 保留的错误原文与 §5.7 的测量）。

## 3. 怎么跑验证（host，改任何东西之后）

一把全跑（十一项，等价于下面逐条命令；CI 的 `benchmarks` job 是其中第 11 项
的来源）：

```sh
scripts/gates.sh                # 全部十一项；输出折叠，失败时保留末 30 行
scripts/gates.sh -v             # 不折叠输出
```

逐条命令（`scripts/gates.sh` 就是按这个清单调的，改了门禁要两边同步）：

```sh
tests/run_host.sh            # Debug，10000 ops（默认档）
tests/run_host.sh --release  # Release
tests/run_host.sh --san      # ASan+UBSan
tests/run_host.sh --cppcheck # 静态检查（用 2.19.0 校准过；换版本先看
                             # tests/run_host.sh --cppcheck 分支的版本注释）
tests/run_host.sh --configs  # TLSF 配置矩阵
tests/run_host.sh --coverage # 覆盖率（Debug + Release 两档，行覆盖下限 85%）
tests/run_host.sh --fuzz     # libFuzzer 有界运行
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude -Isrc \
    examples/host_demo.cpp src/core.cpp -o build/host_demo
python3 examples/protocol_smoke.py build/host_demo   # 104 checks
python3 examples/http_smoke.py build/host_demo       # 27 checks
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
    examples/sensor_pipeline.cpp src/core.cpp -o build/sensor_pipeline
./build/sensor_pipeline                              # 末行必须 ALL AUDITS PASSED，
                                                     # 且 compactions > 0（流程必须真的触发）
g++ -std=c++17 -O2 -DNDEBUG -DPM_DEBUG=0 -Wall -Wextra \
    -Wno-unused-parameter -Werror -Iinclude -Isrc \
    bench/$b.cpp src/core.cpp -o build/bench_$b     # b ∈ {alloc_latency,
    validate_scaling, fragmentation, compaction_window}（CI 的 benchmarks job）
```

真实场景集成参考是 `examples/sensor_pipeline.cpp`（host/设备同一份源码，
设备工程在 `examples/sensor_pipeline_esp32/`）——写新集成时先读它。

全部通过应看到 **5,454,163 (Debug) / 5,454,170 (Release) / 1,553,174 (San，
3000 ops) checks, 0 failures**，模型对拍 **466,859**。CI（`.github/workflows/ci.yml`）
在每次 push 上跑这十一项 + 消费路径冒烟 + 基准构建。

## 4. 怎么跑设备端（需要真实硬件）

**一键流程**：`scripts/device_run.sh s3|esp32 [ops] [full|capture]` —— 构建、刷写
（自动重试）、只读抓取、openocd JTAG 复位（S3）、锚定判定（"app 横幅之后的 ROM
横幅且未完成 = 崩溃"；完成后 IDF 自动重启产生的横幅不算）。它固化了 v18–v20 的
全部排障经验：S3 的 CDC 控制线刷写后不可靠（Errno 84）、esptool 失败直接重试
（约 50% 概率）、芯片楔死在 ROM 时（PC 在 0x4004xxxx，可用 openocd `halt; reg pc`
确认）用 esptool 刷写强制进下载模式即可解除。抓取诊断细节见脚本头注释。

两个 IDF 工程，**都支持两个目标**：

- `esp32/` — 验收固件（suite + concurrency + model / 只后两组）
- `bench/esp32/` — 性能基准（与 host 同一份源码；S3 与经典 ESP32 两种 sizing）

```sh
source ~/esp/activate-idf.sh          # ESP-IDF v6.0.2

# ESP32-S3（esp32/ 的默认目标）
cd esp32 && rm -f sdkconfig && idf.py set-target esp32s3 && idf.py build
idf.py -p /dev/ttyACM0 flash          # USB-Serial-JTAG 控制台

# 经典 ESP32（必须显式追加它自己的覆盖文件，见下）
rm -f sdkconfig
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32" \
    idf.py set-target esp32 && idf.py build
idf.py -p /dev/ttyUSB0 flash          # UART0 经 CH340 桥接
```

**串口宿主必须持续读**：发送队列一满 `printf` 就阻塞，看起来像卡死。
用 `tests/serial_cap.py`（它自带复位与可选停止标记，在两种控制台上都验证过）。

**为什么切目标必须 `set-target`**：IDF 不会自动读
`sdkconfig.defaults.<target>`，且 defaults 列表里靠后的文件**改不动
`CONFIG_IDF_TARGET`**（target 用首个匹配猜测）。两个坑的细节写在
`esp32/sdkconfig.defaults.esp32` 与 `bench/esp32/sdkconfig.defaults.esp32` 里。

**为什么经典 ESP32 不跑 suite**：suite 把 zone 尺寸写在自己的断言里
（测试 [10] `CHECK(made == 16)`、测试 [2] 要 32 段池），而该芯片静态 DRAM
只有约 200 KiB，链接实测 `dram0_0_seg overflowed by 126744 bytes`。
固件会打印 `=== suite SKIPPED (not run, not passed) ===`——**跳过绝不写成通过**。

## 5. 改性能相关代码之前

先读 `bench/README.md` §0（计时仪器规则：**按批计时、绝不逐次**；每次运行自报
仪器；主机上禁止逐次计时）与 `bench/RESULTS.md`（全部实测数字与它们的边界）。
已有的"实测否定"结论不要回头再猜：`get_stats` **走全部空闲链是契约的一部分**
（只走最高非空 bin 的版本被 R54/R55 同族的具名用例挡下，见 HANDOVER_v17 §6）、
`PM_SL_COUNT` 调大在**时间与空间两个轴**上都没有收益、`bins_find` 的 bin 内
best-fit **两轴都更差**。设备侧那个 ~20×/周期的差距**已实测定性为指令数与
流水线依赖，与 cache 无关**（§5.7）。

## 6. 文档地图

| 要知道什么 | 去哪 |
|---|---|
| 最新一轮的完整交接（夹具几何参数化轮） | `docs/HANDOVER_v20.md` |
| 全部实测数字、仪器限制、负结果 | `bench/RESULTS.md` |
| 基准方法、公平性规则、引用数字的必备条件 | `bench/README.md` |
| 不变式与证据（代码位置 + 证明 + 测试） | `docs/AUDIT_LEDGER.md` |
| 用户侧集成（4 种方式）与元数据预算 | `README.md` |
| 各轮收口报告 | `docs/HANDOVER_v2–v20.md` |
