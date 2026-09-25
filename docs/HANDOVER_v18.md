# PondMerge v1 第十六轮：设备验收恢复（HANDOVER v18）

> 起因：收口 v17 §7/§9 的遗留——S3 验收固件因静态 DRAM 放不下 256 KiB zone 而
> **链接失败**，套件自 v1.0.0-4（`a5e68b6`）之后**从未在设备上运行过**。本轮采纳
> v17 §7 的方向 1（zone 入 PSRAM），并在真机上暴露、修复了**三个此前不可见的
> 缺陷**。
>
> 基线：`7c4f75c`（v17 收口）。本轮提交：固件配置 + 套件可移植性一笔（`21f94da`），
> 本报告一笔。
>
> **本轮性质**：`src/core.cpp` 零改动，库语义与复杂度声明不变；改动全部在
> **测试/固件配置**层。host 期望 checks 逐位不变（5,432,806 / 5,432,813 /
> 1,531,817 / 466,859）。

## 1. 三个设备侧缺陷（全部先复现后修复）

| # | 缺陷 | 复现 | 修复 |
|---|---|---|---|
| D1 | `esp32/sdkconfig.defaults` 给 n16r8 配了 **QUAD** PSRAM，而该板是**八线件** | 刷写后 `quad_psram: PSRAM chip is not connected` → abort → `RTC_SW_CPU_RST` 重启循环（抓取窗口内 116 次），USB-Serial-JTAG 最终被拖出总线（`dmesg: cannot reset (err = -110)` → disconnect），需物理重插 | `CONFIG_SPIRAM_MODE_OCTAL=y`。实机：`Found 8MB PSRAM device, Speed: 80MHz`、`SPI SRAM memory test OK` |
| D2 | 套件三个测试**硬编码了宿主档的尺寸**：R46 用 `refs[900]`（静态数组 `refs[PM_MAX_OBJECTS]` 在设备档只有 256 项——**测试自身的数组越界**）；R54/R55 以 512 个 8 B 对象精确填满 2 段池（256 档填不满，slot 表先耗尽报 `NoSpace`） | S3 上套件跑到 R46 必出 4 条 CHECK 失败；R54 级联 `free(o[i]) → INVALID_REF`、`destroy_pool → BUSY`、`deinit → BUSY`，runner 的 `init failed: BUSY` 触发 `abort()`——**确定性重启循环**，每次开机约 64 组测试后死在 R46。host 侧 `-DPM_MAX_OBJECTS=256` 精确复现同一失败序列 | R46 改用 `hi = PM_MAX_OBJECTS - 1`（任意表尺寸下都成立，LIFO 语义不变）；R54/R55 以 `kN/kSegs` 参数化（512→2 段、256→1 段，保持"最小块精确填满整池"的几何前提；`<256` 编译期拒绝）。**教训入账**：这些测试加入时设备套件因 D3 无法构建——**从未上过设备**，host 默认 1024 把它们全部掩盖 |
| D3 | zone 属性的**源码级** `#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` 让 cppcheck 枚举一个 `EXT_RAM_BSS_ATTR` 未知的配置 → 门禁红 | cppcheck 对 `defined(X) && X` 与纯 `#if X` **都会**枚举 X=defined 的配置（实测两种写法都红） | 属性改由构建系统决定：`esp32/main/CMakeLists.txt` 在 sdkconfig 允许时传 `-DPM_ZONE_ATTR=EXT_RAM_BSS_ATTR`，源码 `#ifndef PM_ZONE_ATTR` 收尾，保持**单一配置** |

## 2. zone 入 PSRAM 的代价（v17 §7 预告过，必须写明）

正确性验收不受影响（套件断言的是库行为，不关心内存在哪），但**验收夹具不再是
"内部 SRAM"场景**：它不再代表 `bench/RESULTS.md` 所测的内存等级。基准数字全部
来自独立夹具的内部 RAM，与设备验收记录是两回事。S3 无 PSRAM 的替代方案（缩
zone / 砍套件）都被套件自身的断言排除（v17 §7）。

## 3. 实机验证（`21f94da`，`v1.0.0-22-g21f94da`，ELF SHA256 `2d93dfdaf...`）

```
ESP32-S3 (n16r8) · /dev/ttyACM0 · zone 在 PSRAM
  suite（基础 13 组 + R1–R55，2000 ops）：1,140,828 checks, 0 failures PASSED
  双核并发：28 checks, 0 failures PASSED
  参考模型对拍（4000 ops）：466,859 checks, 0 failures PASSED
  复位重跑（openocd USJ JTAG `reset run`）：三组计数逐位一致
```

套件 1,140,828 与 **host 256 对象档逐位一致**——同一份确定性差分测试在两种
平台上走完同样多的判定（此前"三平台同数"的叙事由此恢复，且新增了 256 档这一
对照点）。双核并发 28 checks 不变：SMP 锁语义声明现在有 LX7 + PSRAM 布局下的
新证据。

## 4. 排障基建（本轮建立，未入库，属于主机侧工具）

- **先起只读抓取再刷写**：刷完的硬复位正好被从 ROM 横幅捕获，全程不需要
  DTR/RTS——刷写后该板的 CDC 控制线 ioctl 常报 `OSError: [Errno 84]`（断电重插
  可恢复；`tests/serial_cap.py` 的复位路径会踩到）。
- **openocd 走 USJ 的 JTAG 接口复位**（`interface/esp_usb_jtag.cfg` +
  `target/esp32s3.cfg`，`init; reset run; shutdown`）：完全不依赖 CDC 控制线，
  本轮用它完成了复位重跑。

## 5. 门禁（11/11 全绿，`21f94da` 上复验）

| 档位 | 本轮 |
|---|---|
| Host Debug / Release / ASan+UBSan | 5,432,806 / 5,432,813 / 1,531,817，全部 0 failures |
| 参考模型对拍 | 466,859（不变） |
| cppcheck | exit 0（D3 修复后恢复） |
| 配置矩阵 / 覆盖率 / fuzz / 协议 / HTTP / 示例自检 / 基准构建 | 全部 PASSED（覆盖率 Debug 92.51% / Release 92.58%，与 v17 一致） |

⚠️ README 的覆盖率行此前仍写 v16 的 97.59%/98.80%——本轮一并更正为 v17 起的实际
值（下降原因见 v17 §5，并非本轮引入）。

## 6. 未做 / 待办

- **经典 ESP32 未接**（v17 §9 遗留）：其记录仍停在 `v1.0.0-6`，早于 v17 算法重构。
  该重构声明"行为保持"，所以旧记录对当前代码的适用性是**推断**——README 已如实
  标注。接板重烧（`rm -f sdkconfig` + `set-target esp32` + 两个 defaults 文件）
  即可消除这条推断。
- `validate()` 整理窗口百分位重取（v17 §9，未动）。
- 覆盖率逐行归属（v17 §5，未动）。
- 套件对 `PM_MAX_OBJECTS < 256` 的支持：R54/R55 现在是编译期拒绝。要不要支持
  更小的表（几何上需要非 16 B 块精确填充）留给后续轮决定。
