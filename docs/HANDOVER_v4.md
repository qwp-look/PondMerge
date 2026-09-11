# PondMerge 交接文档 v4 —— 第三轮起点

> **读者**：接手本项目的 AI 或开发者。本文档假设你**没有任何本项目上下文**，
> 目标是让你在读完这一份之后，能独立地改代码、跑验证、判断对错、并且不重复
> 前两轮踩过的坑。
>
> 收口基线：`6bcd28f`（2026-09-11），`main` 已 push 到 origin。
> 前两轮的过程记录见 `docs/HANDOVER_v2.md`（第一轮）与 `docs/HANDOVER_v3.md`
> （第二轮）；本文档是**面向未来的交接**，与它们是互补关系。

---

## 0. 30 秒速览

- **项目**：PondMerge v1，面向无 MMU 的 32 位 MCU 的用户态托管内存系统（TLSF 风格
  分离适配分配器 + 稳定逻辑引用 + RAII 借用 + 显式 compact/merge/split）。
- **代码**：不在本机。仓库在 `qwp@192.168.3.71:~/code/PondMerge`（Ubuntu 26.04 虚拟机），
  origin 是 `https://github.com/qwp-look/PondMerge.git`。
- **规模**：约 5.2k 行（核心库 ~2.1k 行，测试 ~2.7k 行）。
- **当前状态**：功能完整、Host Debug/Release/ASan/cppcheck/配置矩阵全绿、ESP32-S3
  实机已验证（1,102,255 checks, 0 failures）。**没有已知 bug**。
- **下一轮最该做的**：真并发（双核 FreeRTOS）锁边界测试 —— 唯一没被验证过的契约面。
  详见 §9 的 T1。
- **三条铁律**：不得削弱既有测试；文档与实现必须同步；实机结论必须在启动日志里
  核对 `App version` 等于提交号，没验证就说没验证。

---

## 1. 项目是什么

### 1.1 解决的问题

长时间运行的 MCU 程序会因为内存碎片而分配失败。PondMerge 提供一套**显式触发**的
整理机制：开发者暂停相关池、确认没有外部持有者，然后调用 compact/merge/split 把
可搬迁对象压实；长期保存的 C++ 逻辑指针（`pm_ptr`）在下次使用时惰性解析到新地址。

### 1.2 三个内存区域

| 区域 | 管理者 | 规则 |
|---|---|---|
| Chaos Zone | 硬件/外设/驱动 | PondMerge **永不触碰** |
| Free Zone | 开发者 | PondMerge 不管理 |
| Auto Zone | PondMerge | 只管理显式分配到这里的对象 |

Auto Zone 是一段固定内存，由 `pm::init(Config)` 注册，内部按 `segment_size`
（默认 4 KiB）划分成若干 segment，再组合成 Pool。

### 1.3 对象模型与指针

- 每个分配得到一个**稳定的 slot 索引**（descriptor），带 `generation`（防 ABA）
  与 `address_epoch`（地址变更计数）。
- `pm_local_ptr<T>` 绑定创建时的池；`pm_cross_ptr<T>` 用于跨池，必须经
  `pm_as_cross()` / `pm_cross_ref()` 显式生成。
- `borrow()` 返回 RAII 对象，借用期间池不可整理；这就是获得普通 `T*` 的唯一途径。
- **没有地址缓存**：每次 borrow/resolve 做一次完整校验 + 一次描述符读取（O(1)）。

### 1.4 明确不做的事（非目标）

不修复裸指针、不在 ISR/后台自动整理、不判断 DMA 是否完成、不分析对象内部指针、
不扩大 Auto Zone、无硬件 MPU/MMU 保护、不保证整理硬实时。**不要把精力花在这些上。**

---

## 2. 运行环境与访问方式

```
主机      : qwp@192.168.3.71（VMware 虚拟机，Ubuntu 26.04.1 LTS，内核 7.0.0-31）
仓库      : ~/code/PondMerge
origin    : https://github.com/qwp-look/PondMerge.git
登录 shell: zsh（注意！见 §11.1）
工具链    : g++ / clang++ / cmake / cppcheck / rsync 均可用
ESP-IDF   : ~/esp/activate-idf.sh → IDF v6.0.2（~/.espressif python env 内含 pyserial）
开发板    : ESP32-S3（model=9, rev=0.2, 双核, 16MB flash / 8MB PSRAM 的 n16r8 模组）
串口      : /dev/ttyACM0（芯片自带的 Espressif USB JTAG/serial debug unit, 303a:1001）
            控制台走 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG，**不是** UART 桥
```

免密 SSH 已配置好，可直接非交互执行：

```sh
ssh -o BatchMode=yes qwp@192.168.3.71 "cd ~/code/PondMerge && git log --oneline -1"
```

**开发模式**：代码不在本机时，推荐「本地编辑 → `scp` 同步单文件 → 远程编译测试」。
本仓库有两份等价副本：远程是权威（有 `.git`），本地副本用于编辑。命令见 §7。

---

## 3. 当前基线状态

```
HEAD = origin/main = 6bcd28f   工作区干净
```

提交历史（近 5 条）：

```
6bcd28f  v2 round 2: 剩余问题清单（HANDOVER_v3 §8）
f8bf919  v2 round 2: 固件/HEAD 版本对应说明
d746738  v2 round 2: ESP32-S3 实机验证 + tests/serial_cap.py
a9232c7  v2 round 2: validate 整数化 + 覆盖审计、元数据加固、维护入口契约、模型对拍
e7bccf0  v2 round:  split 搬移顺序、锁纪律、init 生命周期、TLSF 配置契约、维护前预检
c330db8  v1: 实施第一轮修复任务书
ef7c1ed  v1: 基础实现
```

### 3.1 验收数字（全部 0 failures，提交 `a9232c7` 的代码）

| 档位 | 命令 | 结果 |
|---|---|---|
| Host Debug | `tests/run_host.sh 10000` | 5,391,417 checks |
| Host Release | `tests/run_host.sh --release 10000` | 5,391,417 checks |
| ASan/UBSan | `tests/run_host.sh --san 3000` | 1,490,428 checks |
| 参考模型对拍 | （随每次 host 运行） | 466,859 checks |
| 静态分析 | `tests/run_host.sh --cppcheck` | 退出码 0，0 告警 |
| 配置矩阵 | `tests/run_host.sh --configs` | SL 2/4/8/16 + FL 31 通过；SL 32 / FL 32 编译期拒绝；FL 容量上限 16/24 按预期拒绝/接受 |
| ESP32-S3 实机 | `tests/serial_cap.py` + 烧录 | 1,102,255 checks；13 组基础 + R1–R21 全 PASS |

设备记录（任务书 §12.3 要求的完整字段）：芯片 ESP32-S3 rev 0.2 双核、
串口 `/dev/ttyACM0` 115200、`App version a9232c7`、`ELF SHA256 42cdce502...`、
编译时间 `Sep 11 2026 21:15:07`、固件 233,280 B、Auto Zone 256 KiB @ 内部 SRAM、
`PM_MAX_OBJECTS=256`、启动时空闲内部堆 93,680 B、元数据 27,192 B、
压力统计 `max_live=55 max_borrow=1 max_moved=30960 max_compact_us=2488`。

---

## 4. 代码地图

```
include/pondmerge/
    pm_config.h      73 行   编译期参数 + 契约 static_assert 群
    pm_port.h        22 行   平台层：PM_LOCK/PM_UNLOCK、pm_port_ticks_us
    pondmerge.hpp   433 行   公共 API：Status/RawRef/pm_ptr/pm_access/pm_make
                             + 并发契约段 + 维护入口契约段 + 复杂度声明
src/
    internal.h      152 行   内部结构：FreeBlock/Pool/ObjectDesc/TlsfBins/GlobalState
    core.cpp       1536 行   全部实现（单一翻译单元）
tests/
    suite.cpp      2233 行   验收套件（host 与 ESP32 共用同一份！）
    model.cpp       411 行   参考模型对拍（固定 seed，host）
    main.cpp         24 行   host runner：套件 + 模型
    config_smoke.cpp         每个 TLSF 配置编译一次的冒烟
    config_limits.cpp        FL 容量契约（拒绝/接受）
    run_host.sh              四种档位 + --configs
    config_matrix.sh         配置矩阵
    serial_cap.py    94 行   设备串口抓取（复位 + 读日志）
esp32/
    CMakeLists.txt, sdkconfig, sdkconfig.defaults
    main/CMakeLists.txt      SRCS = main.cpp + ../../tests/suite.cpp（**不含 model.cpp**）
    main/main.cpp            app_main：打印芯片信息 → pondmerge_run_tests(2000)
    components/pondmerge/    组件包一层 CMake，强制 gnu++17
docs/
    架构说明、代码指导书、v1/v2 任务书、HANDOVER_v2/v3、本文档
```

### 4.1 `src/core.cpp` 关键函数索引（行号为 `6bcd28f`）

| 行 | 函数 | 注意点 |
|---|---|---|
| 23 | `load32/store32` | 块头访问必须走这两个函数（8 字节对齐的 memcpy） |
| 57/63 | `fl_index/sl_index` | `fl_index` 在 `PM_FL_MAX-1` 处**饱和**；调用方必须自己拒绝更大的块 |
| 96 | `bins_find` | 同 bin 内 first-fit 遍历；**游标解引用前有 `zone_off_readable` 判界** |
| 157 | `bump_epoch` | 跳过 0 的回绕保护（cppcheck 误报已抑制并有理由） |
| 175 | `order_insert_sorted` | 地址序链表插入（validate 的唯一可信排序来源） |
| 208 | `next_generation` | 跳过 0 的回绕保护（同上） |
| 225 | `desc_block_consistent` | **描述符↔物理块关系校验**，被 `check_ref` 调用，O(1) 无 zone 解引用 |
| 234 | `check_ref` | 所有 resolve/borrow/free 的公共校验入口 |
| 265 | `pool_maintainable` | 维护入口状态契约（Running 或 Paused） |
| 280 | `precheck_pool` | **方案 A 的关键**：搬移前的全量 descriptor 审计，整数化寻址 |
| 335 | `finalize_layout` | 重建块头/bins/统计；**写入块间与池尾 slack 的 poison** |
| 412 | `compact_impl` | 只读规划 + 按地址序 memmove；失败路径 `fail:` 恢复 Running |
| 525 | `init` | **先全部校验，最后才 `memset(&G)`**；含 FL 容量拒绝 |
| 647 | `pause` | 已在 Paused 时返回 `AlreadyPaused`（**不是** `Ok`）；处于维护态返回 `Busy` |
| 662 | `resume` | 已在 Running 时返回 `Ok`（幂等）；处于维护态返回 `Busy` |
| 674 | `get_stats` | 有限步 + 游标判界；损坏时 `largest_free_block = 0`（语义有歧义，见 §9 T3） |
| 724 | `alloc` | TLSF 定位 bin + 链内 first fit；失败回滚**不动** generation |
| 827 | `free` | **前向合并依赖 poison 不变式**（见 §6.1）；释放时 slot 走 `Destroying → Free` **并递增 `generation`**（所有旧引用立即失效），再合并邻块 |
| 892 | `set_destroy_fn` | 只接受 pinned；movable 返回 `NotRelocatable` |
| 907/927 | `borrow_begin/borrow_end` | 锁内做状态检查 + 计数增减 |
| 963 | `compact` | 文档 §8 语义：先进入 Paused 再查借用；被拒时**保持 Paused** |
| 1004 | `merge` | 入口锁内装备；失败恢复**入口状态**；成功后源池 Empty、目标 Running |
| 1101 | `split` | 两趟搬移（见 §6.2）；`fail_restore` 恢复入口状态 |
| 1338 | `validate` | 三段审计（live 链 / bins / 并集覆盖）；见 §6.3 |

---

## 5. 不可违反的约束（铁律）

> 以下每一条都是前两轮任务书明文规定的，违反等于把项目做废。

### 5.1 测试完整性

- **不得删除、放宽或跳过任何既有验收测试**；不得通过减少压力次数、删 `CHECK`、
  改期望值来"修好"测试。
- 新增测试只能加强覆盖。如果修复确实让旧断言失效，必须**在文档里说明为什么**
  并保留等价强度的新断言。
- 测试组编号 R1–R21 与基础 1–13 是引用锚点，不要重排。

### 5.2 禁止"伪修复"

- 不得把关闭 `PM_DEBUG` 当作修复内存损坏的手段。
- 不得在 `validate()` 失败时静默清空池或重建。
- 不得把所有对象标成 pinned 来绕过搬移算法。
- 不得为 movable 数据引入动态分配。
- 不得只改 README 不改实现，也不得只改实现不更新契约。
- 不得扩大 `PM_MAX_*` 或放宽边界来掩盖配置错误。

### 5.3 事务模型 A 的前提

`compact/merge/split` 采用**方案 A**：只读规划 → 执行阶段"不可失败"。这个前提的
**唯一支撑是 `precheck_pool()` 做全量审计**。如果你放宽 precheck，方案 A 立刻失效，
必须改成可回滚方案。**改 precheck 前先读 `HANDOVER_v2.md` §3。**

### 5.4 语言与平台

核心库必须 C++17、无异常、无 RTTI、无动态分配；host 与 ESP32 编译同一份 `suite.cpp`。
`PM_MAX_OBJECTS` 在设备侧是 256（元数据在静态 BSS，设备内存紧张）。

### 5.5 文档与实现同步

复杂度声明、API 语义、锁边界必须在 `README.md`、`pondmerge.hpp` 注释、测试三处
一致。第二轮就是因为 README 声称 `alloc` 是 O(1) 而被要求更正。

### 5.6 实机结论的诚实性

设备测试报告必须包含：芯片型号/revision、串口路径、`App version`（= 提交号）、
ELF SHA256、编译时间、编译标准、检查数与失败数。**烧录前必须先提交**，否则
`App version` 会带 `-dirty`。只看到串口 PASS 但版本是旧提交时，**不得**声称当前
版本已实机验证。

---

## 6. 五条必须知道的非显然结论

> 这五条是前两轮用真实故障换来的，不知道会重复踩。

### 6.1 池内空闲区的 poison 不变式，以及 `free()` 对它的隐性依赖

`finalize_layout()` 会往「未覆盖区」的首 4 字节写 `0`（poison），包括**块间**小间隙
和**池尾** slack。原因是 `free()` 的前向合并要读 `block + block_size` 处的字判断
邻块是否空闲：

```cpp
next = block + bsize;
if (next < poolEnd && blk_is_free(next)) { /* 合并 */ }
```

如果那块内存没被 poison（第一轮就漏了池尾），它会读到陈旧字节，可能把垃圾当成
空闲块，进而 `bins_remove()` 一个不存在的块，破坏 bins 与统计。
**影响**：任何绕过 `finalize_layout()` 直接往池内写裸数据的内部路径都会破坏这个
不变式（§9 T4 可选加固）。

### 6.2 `split()` 的上半区搬移顺序是**两趟**，不是"全部升序"

上半区计划按地址序切成两段：**右移前缀**（目标地址 > 源地址）按源**降序**执行，
**左移后缀**按源**升序**执行。跨界对象恰好是前缀的首元素，因此最后执行。

> **重要**：`docs/PondMerge_v2_repair_task.md` §4.2 的伪代码写的是"upper 全部升序"，
> 这个提法**在其自身前提下不成立**（当跨界对象主体在边界下方时，其后的普通对象
> 也会向右移动）。以 `split()` 执行段注释里的证明为准，**不要照抄伪代码**。
> 覆盖测试：R13、R15(a)。

`docs/HANDOVER_v2.md` §1 条目 1 记录了当时发现它的过程（红测实测 byte 3568 损坏）。

### 6.3 `validate()` 的覆盖不变量：不能写成"每个空闲块恰好填满一个块间空隙"

朴素的表述是**错的**：`free()` 只合并**物理相邻**的空闲块，而 slack 不能吸收，
所以会出现"空闲块—slack—空闲块"这种被隔开的两个空闲块。

正确的、可检查的不变量（`validate()` 第三段实现）：

```text
1) 「live ∪ binned free」并集中，任何未覆盖间隙必小于 PM_MIN_BLOCK
   （更大的间隙按定义必须是一个 binned 空闲块）
2) live_bytes + binned_free_bytes + slack == pool_capacity
3) 空闲块两两不重叠，且没有两个空闲块共享同一起始地址
两者合起来才能证明"没有隐藏空洞、没有重复记账"。
```

### 6.4 设备串口：宿主必须持续读，否则设备会**卡住**

控制台是 USB-Serial-JTAG。**宿主不读时，发送队列会被填满，`printf` 阻塞**，
现象是"套件停在前几个测试组不动"，极容易被误判为死锁或性能问题（第二轮就误判过一次）。
务实用法：`tests/serial_cap.py`，它一直读。

另外两个串口相关的坑：
- **复位极性**：正常启动 = IO0 保持高（`DTR=False`）+ 脉冲 EN（`RTS`）。把 IO0 拉低
  会进入下载模式（`boot:0x23 (DOWNLOAD(USB/UART0))`），串口只打印
  `waiting for download`，看起来像设备没反应。
- **`pyserial` 打开串口本身会触发一次复位**，所以要"复位 → 排空 1.5s → 再复位"
  才能拿到单次干净的启动日志（脚本已这么做）。

### 6.5 默认配置下，`init()` 的 FL 容量检查是**防御性**的

`init()` 会拒绝「声明的 zone 最大块 ≥ 2^PM_FL_MAX」的配置（`NoSpace`），避免
`fl_index()` 把大块静默 clamp 到最高 bin。但默认 `PM_MAX_SEGMENTS=64` + segment
4 KiB 时 zone 上限只有 256 KiB，远低于 2^24，所以这条检查平时不会触发。
要用更大的 zone，必须**同时**提高 `segment_size`（如 128 KiB × 64 = 8 MiB，
仍须低于 16 MiB 上限）。测试用 `-DPM_FL_MAX=16/24` 把上限压到可触发区间。

---

## 7. 工作流：怎么改、怎么验证、怎么提交

### 7.1 推荐节奏

1. **只读勘察**：`git status --short`、`git rev-parse HEAD`、先跑一遍 §8 的验收命令
   建立基线。**基线失败要先判断是环境问题还是代码问题，不要把基线失败混进本轮修复。**
2. **本地编辑**（若远程不便编辑）：保持一份本地副本，改完 `scp` 单个文件回去。
   ```sh
   scp -o BatchMode=yes src/core.cpp qwp@192.168.3.71:~/code/PondMerge/src/
   scp -o BatchMode=yes tests/suite.cpp qwp@192.168.3.71:~/code/PondMerge/tests/
   ```
   ⚠️ 不同目录的文件**必须分开 scp**，否则全被丢进同一个目标目录。
3. **小步验证**：每改一个主题就跑最小回归（几百~2000 op），全绿后再上满规模。
4. **收口**：跑 §8 全部档位 → 设备验证 → 更新文档 → 提交 → push。

### 7.2 设备验证（完整流程）

```sh
# 1) 先提交（App version 来自 git，未提交会带 -dirty）
cd ~/code/PondMerge && git add -A && git commit -m '...'

# 2) 构建并查看会烧进固件的版本信息
bash -c 'source ~/esp/activate-idf.sh >/dev/null 2>&1; cd ~/code/PondMerge/esp32; \
  idf.py -B build build'
bash -c 'source ~/esp/activate-idf.sh >/dev/null 2>&1; cd ~/code/PondMerge/esp32; \
  python -m esptool --chip esp32s3 image_info build/pondmerge_test.bin' \
  | grep -Ei 'image size|app version|compile time|elf file'

# 3) 烧录（结束后会自动硬复位）
bash -c 'source ~/esp/activate-idf.sh >/dev/null 2>&1; cd ~/code/PondMerge/esp32; \
  idf.py -B build -p /dev/ttyACM0 flash'

# 4) 复位并抓完整启动日志 + 测试输出（脚本会一直读，避免设备阻塞）
cd ~/code/PondMerge && bash -c 'source ~/esp/activate-idf.sh >/dev/null 2>&1; \
  python tests/serial_cap.py /dev/ttyACM0 300 115200' | tee /tmp/device.log

# 5) 核对证据链
grep -E 'App version|ELF file SHA256|Compile time|chip: model|checks,|suite (PASSED|FAILED)' /tmp/device.log
```

**必须**确认 `App version` 等于当前提交号、`ELF file SHA256` 与本地产物一致，
再采信串口 PASS。

### 7.3 提交与推送

- 在**远程**执行 git 操作（仓库在那）。
- 多行提交信息：本地写文件 → `scp` 到 `/tmp` → `git commit -F /tmp/msg.txt`。
  **不要在 ssh 命令里嵌 heredoc**（引号会和本地 shell 打架）。
- 提交信息写清「改了什么 / 为什么 / 怎么验证（含实测数字）」。
- 提交前 `git status --short` + `git diff --stat` 自查，避免把误拷进仓库根的文件带上。
- 推送前**先问用户**（本轮的 push 是用户明确要求的）。

---

## 8. 验收门槛（每轮收口必须全绿）

```sh
cd ~/code/PondMerge
tests/run_host.sh 10000          # Host Debug，期望 0 failures
tests/run_host.sh --release 10000
tests/run_host.sh --san 3000     # ASan+UBSan
tests/run_host.sh --cppcheck     # 期望退出码 0、0 告警
tests/run_host.sh --configs      # 配置矩阵
# 设备侧（有板子时）
idf.py -B build build && idf.py -B build -p /dev/ttyACM0 flash
python tests/serial_cap.py /dev/ttyACM0 300 115200
```

⚠️ `$?` 会被管道吃掉：取退出码请 `cmd > /tmp/x.log 2>&1; echo "rc=$?"`。

历史参考值（10000 op）：Debug/Release 各 5,391,417 checks；3000 op 1,490,428 checks；
模型对拍 466,859 checks；设备 2000 op 1,102,255 checks。**check 数变了不一定是错**
（改了测试就会变），关键是 `0 failures`。

---

## 9. 待做任务清单（按建议优先级）

> 每项都给了「目标 / 做法提示 / 验收标准」。**做完一项就单独提交**，不要攒在一起。

### T1 ★ 真并发锁边界测试（设备侧，最有价值）

- **目标**：把「pause 与新 borrow 之间不存在检查-翻转竞态」这条契约固化成测试。
  目前 R16 只覆盖了**单线程**的借用计数/状态矩阵，而 `PM_LOCK` 在双核 ESP32 上才
  真正被使用。
- **做法提示**：在设备侧新增测试（可放 `esp32/main/` 或新建
  `tests/concurrency_esp32.cpp`，**只在 ESP32 构建里加入 `SRCS`**，不要污染 host 套件）。
  用 FreeRTOS 建两个任务：
  - 任务 A：循环 `borrow_begin` → 若成功则写 payload → `borrow_end`，统计成功/失败次数；
  - 任务 B：循环 `pause` → `compact` → `resume`，统计 `compact` 返回 `Ok` 的次数。
    **注意返回码语义**：`pause` 在池已是 Paused 时返回 `AlreadyPaused`（不是 `Ok`），
    在维护态返回 `Busy`；`resume` 在已是 Running 时返回 `Ok`。任务 B 要把
    `{Ok, AlreadyPaused}` 都当作"现在已暂停"，否则断言会误报。
  断言（用 task-safe 的计数与临界区）：
  1. 任何一次 `compact` 返回 `Ok` 时，池在 compact 开始前的 `borrow_count` 必为 0
     （这是 `compact` 内部保证的，但要断言**没有新的 borrow 混进来**）；
  2. 池处于 Paused 期间，任务 A 的 `borrow_begin` 只允许返回 `Busy`（以及
     `PoolChanged`/`InvalidRef` 这类与状态无关的失败），**不得返回 `Ok`**；
  3. 运行结束后 `borrow_count == 0`、对象的 `active_borrows == 0`、`validate()` 通过。
- **验收标准**：设备实机跑 ≥200 轮，0 failures；`borrow_count` 守恒；
  host 侧套件不回归（新增文件不进 host 构建）。
- **注意**：正常的 `alloc/free/resolve` **本来就不承诺**多线程安全（单所有者契约），
  所以这个测试**只针对 borrow/pause/compact 的锁边界**，不要试图去测 alloc 并发
  ——那会测出一个"设计上不保证"的失败。若发现 Paused 期间 borrow 竟然成功，那是
  **真 bug，最高优先级**。

### T2 ★ 长时间重复 init/deinit 循环压力（host，成本低）

- **目标**：补齐任务书 §12.2 的「长时间重复 init/deinit」，目前 R14 只做了 2~3 次。
- **做法提示**：新增 R22，固定 seed，循环 200~1000 次：
  `init → create_pool → 若干 alloc/free → destroy_pool → deinit`，
  每次轮次校验：deinit 后重新 init 时 `generation` 从 1 重新开始、`live_object_count == 0`、
  bins 为空、`global_stats()` 的高水位统计被重置（或按文档定义保持不变——先读 R14 的断言）。
- **验收标准**：Debug/Release/ASan 全绿，0 failures；轮次数写进测试输出便于对账。

### T3 `get_stats()` 语义澄清（API 变更，需同步文档）

- **目标**：消除 `largest_free_block == 0` 的歧义（既可能是"真的没有空闲块"，
  也可能是"遇到损坏链表拒绝报告"）。
- **做法提示**（二选一）：
  (a) 给 `PoolStats` 加一个 `uint8_t valid;`（或 `corrupt`）字段，损坏时置位并保持
      `largest_free_block = 0`；
  (b) 保留现有函数并新增 `Status get_stats(PoolId, PoolStats&)` 重载。
  **必须保持既有入口可用**，否则会破坏 R18/R21 与模型测试。
- **验收标准**：R18 扩展为「损坏时 `valid == 0`」；`README.md`、`pondmerge.hpp`、
  `docs/` 同步；全档位 0 failures。

### T4 解除 `free()` 对 poison 不变式的隐性依赖（可选加固）

- **目标**：见 §6.1。让 `free()` 即使读到未 poison 的陈旧字节也不会破坏 bins。
- **做法提示**：在前向合并处对 `next` 做界内校验（在池内、8 字节对齐、若判为 free
  则 `size >= PM_MIN_BLOCK`）；Debug 用 `PM_ASSERT`，Release 选择"跳过合并"而不是
  返回错误（**不要**把 `free()` 改成会返回 `CorruptMetadata` 的重路径——那会改变
  它现在的语义与 R7 的断言）。
- **验收标准**：新增一个"人为把池尾首字改成非 0 后 `free()` 仍安全"的测试；
  R15(c)/R20/R21 不回归。

### T5 参考模型对拍上设备（可选）

- **目标**：消除"host 有、设备没有"的唯一覆盖缺口。
- **做法提示**：把 `tests/model.cpp` 加入 `esp32/main/CMakeLists.txt` 的 `SRCS`，
  在 `app_main()` 里以较小 ops 调用（如 `pondmerge_run_model(64*1024, 500)`），
  注意设备侧内存（模型对象数组 64 个 + 池数组 16 个，很小；但 Zone 会另外占 64 KiB）。
- **验收标准**：设备日志出现 `[model] N checks, 0 failures`；设备 RAM 仍够（看
  `free internal heap at boot`）。

### T6 `--san 10000` 补跑（便宜）

- 目前 ASan 只跑了 3000 op。跑 `tests/run_host.sh --san 10000` 并记录数字即可。

### T7 文档补齐（便宜）

- 入库第一轮的《PondMerge_v1_修复注意事项.md》（`docs/` 目前缺这一份）。
- 复查 README 的复杂度表与 `pondmerge.hpp` 注释是否仍与实现一致。

### T8 设备侧压力次数可配置（便宜）

- `esp32/main/main.cpp` 里 `pondmerge_run_tests(2000)` 是硬编码；改成可由
  `-DPM_DEVICE_STRESS_OPS=...` 覆盖，便于临时跑满 10000。

### 不建议现在做的

- §9.3 更严格的「逐物理块走到 pool end」校验：需要固定 scratch 或复用维护路径的
  scratch 数组，收益有限（现有两条不变量已能证明无空洞），成本较高。要做请单独立项。
- 给 `alloc` 做严格 O(1)：需要改 bin 内组织结构（按块大小的固定容量结构），
  属于设计变更，不在当前任务范围。

---

## 10. 剩余问题台账（浓缩定位）

| 类别 | 问题 | 详述位置 |
|---|---|---|
| A 未覆盖 | 真并发锁边界测试（§11.1 条目 4 只做了单线程） | 本文 §9 T1 |
| A 未覆盖 | 长时间 init/deinit 循环压力（§12.2） | 本文 §9 T2 |
| A 未覆盖 | ASan 只跑 3000 op | 本文 §9 T6 |
| B 取舍 | 模型对拍未上设备 | §9 T5 |
| B 取舍 | `validate()` 复杂度 O((live+free)²)；设备 `max_compact_us` 1176 → 2488 µs | `HANDOVER_v3.md` §4.4 |
| B 取舍 | §9.3 更严格形式未做 | `HANDOVER_v3.md` §4.5 |
| C 接口 | `get_stats()` 的 0 值歧义 | §9 T3 |
| C 接口 | `free()` 与 poison 的隐性耦合 | §6.1 / §9 T4 |
| C 接口 | 零长对象被拒（`desc_block_consistent` 视为损坏） | `HANDOVER_v3.md` §8 C3 |
| C 接口 | FL 契约与 `PM_MAX_SEGMENTS` 的相互作用 | §6.5 |
| D 工程 | `docs/` 缺第一轮《修复注意事项》 | §9 T7 |
| D 工程 | 设备压力次数硬编码 | §9 T8 |
| D 工程 | 9 处 cppcheck 抑制待换工具时复核 | `HANDOVER_v3.md` §8 D3 |
| D 工程 | `serial_cap.py` 依赖 pyserial | `HANDOVER_v3.md` §8 D4 |

已确认**满足**的验收项（不要重复做）：§12.2 的固定 seed 10000+ 随机压力（Debug/
Release 各 10000，seed `12345`）、ESP32 多次复位后重复运行（3/3 PASS）、
split crossing 专项、配置矩阵、故障注入。

---

## 11. 已踩过的坑（避免重复浪费）

1. **远程登录 shell 是 zsh**：`ls /dev/ttyACM* /dev/ttyUSB*` 在 `ttyUSB*` 不存在时
   zsh 的 glob 失败会**中断整条命令**，导致我误判"设备没接"。查设备用
   `ls /dev | grep -E '...'`。跑 IDF 用 `bash -c 'source ~/esp/activate-idf.sh; ...'`。
2. **`scp` 多文件陷阱**：`scp a.cpp b.cpp host:~/repo/` 会把两个都放到仓库根。
   不同目录要分开 scp。
3. **工作区根目录的临时文件会被外部清掉**（`serial_cap.py`、`commit_msg.txt` 都
   消失过）。临时文件放 `%TEMP%` 或项目子目录里。
4. **`$?` 被管道吃掉**：`cmd | tail; echo $?` 拿到的是 `tail` 的状态，永远是 0。
   要取真实退出码就重定向到文件。
5. **ESP32 构建放后台**（增量约 30s，全量更久），并 `> /tmp/idf.log 2>&1` 再看
   `rc` 与 `tail`，不要直接刷屏。
6. **第一次设备抓取只看到一半日志**：因为烧录后的硬复位已经开始跑套件，而宿主没读，
   设备阻塞。用 `tests/serial_cap.py`（自带复位 + 持续读 + 先排空）就不会。
7. **提交顺序**：设备固件的 `App version` 来自 git，**先提交再烧录**，否则是
   `<commit>-dirty`，无法与提交号对应。
8. **`validate()` 的覆盖校验第一版写错了**：我最初写成"每个空闲块必须恰好填满一个
   块间空隙"，在 R13/R15 上红测。原因见 §6.3——`free()` 只合并物理相邻块。

---

## 12. 文档索引

| 文档 | 用途 |
|---|---|
| `docs/架构说明.md`（原 `架构.md`） | 顶层设计：三区模型、池/对象/指针、整理规则、非目标 |
| `docs/PondMerge_v1_代码指导书.md` | 接口与内存布局的原始设计（含 §18 基础测试要求） |
| `docs/PondMerge_v1_repair_task.md` | 第一轮任务书（R1–R12 的来源） |
| `docs/PondMerge_v2_repair_task.md` | 第二轮任务书（R13–R21 的来源）。**注意 §4.2 伪代码有前提缺口，见本文 §6.2** |
| `docs/HANDOVER_v2.md` | 第一轮收口报告 + 设计边界（事务模型 A、并发契约、split 向右组） |
| `docs/HANDOVER_v3.md` | 第二轮收口报告：10 项修复、4 个新修缺陷、语义变更、§8 剩余问题清单 |
| `docs/HANDOVER_v4.md` | **本文档**：面向下一轮的完整交接 |
| `README.md` | 对外说明：关键语义、复杂度表、测试清单、验收数字、设备流程 |

---

## 13. 给你的第一条命令

```sh
ssh -o BatchMode=yes qwp@192.168.3.71 \
  "cd ~/code/PondMerge && git log --oneline -1 && git status --short && \
   tests/run_host.sh 2000 2>&1 | tail -3 && \
   echo 'device:' && bash -c 'source ~/esp/activate-idf.sh >/dev/null 2>&1; \
     python tests/serial_cap.py /dev/ttyACM0 300 115200' | tail -4"
```

预期：提交为 `6bcd28f`（或其后继）、工作区干净、host `0 failures`、
设备 `=== suite PASSED (rc=0) ===`。**如果这三项都对，说明基线与文档一致，可以开始
T1 了。** 如果不对，先修基线，不要在坏基线上叠加改动。
