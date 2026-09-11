# PondMerge v1 第二轮修复指南与注意事项

本文档用于交给其他 AI 或开发者实施第二轮修复。目标仓库当前基线为：

```
/home/qwp/code/PondMerge
baseline: c330db8
```

本文档允许使用伪代码进行精确设计，但不允许在没有测试和复杂度说明的情况下直接改实现。修复过程中不得删除、放宽或跳过现有验收测试。

## 1. 任务目标

本轮必须解决以下问题：

1. 拆分时跨边界对象与上半区对象混合搬移可能造成源数据覆盖。
2. `merge`/`split` 的暂停、借用计数、状态转换和锁保护不一致。
3. 重复调用 `init()` 或配置失败时可能清空已有全局状态。
4. 配置允许的 `PM_SL_COUNT=32` 与实际 16 位位图不匹配。
5. 整理计划没有验证全部 descriptor，方案 A 的“执行阶段不可失败”前提没有被代码证明。
6. `validate` 没有验证对象 payload 是否落在自己的物理块内。
7. `set_destroy_fn()` 可被用于 movable 对象，生命周期语义不明确。
8. `get_stats()` 遇到损坏 free list 可能无限循环。
9. 静态检查、复杂度和 README 中的声明必须与实际实现一致。

## 2. 不可改变的设计约束

- Auto Zone 是唯一由库管理的区域；Chaos Zone、Free Zone 不得被触碰。
- descriptor 槽位是稳定逻辑身份；`generation` 防止 ABA；`address_epoch` 表示地址变化。
- `pm_local_ptr` 只能绑定原池；跨池必须显式生成 `pm_cross_ptr`。
- 普通物理地址只在 borrow 生命周期内有效。
- 维护前必须保证相关池没有 DMA、ISR、其他线程或任务持有普通物理指针。
- pinned、DMA、external 对象不可移动。
- v1 不增加动态分配、异常、RTTI 或硬件 MPU/MMU 保护。
- 核心库必须在 C++17 下编译。
- 如果某个 API 仍然要求调用者提供静默维护期，必须在头文件、README 和测试中统一说明；不能一处要求 `pause`，另一处只接受 `Running`。

## 3. 推荐工作流

### 3.1 阶段 A：建立基线

执行者首先只读检查：

```sh
git status --short
git rev-parse HEAD
tests/run_host.sh
tests/run_host.sh --release
tests/run_host.sh --san 3000
tests/run_host.sh --cppcheck
```

记录每个命令的退出码、检查数量和告警。若基线已经失败，先判断是环境问题还是代码问题，不要把基线失败混入本轮修复。

ESP32 端先执行：

```sh
source ~/esp/activate-idf.sh
cd esp32
idf.py -B build build
idf.py -B build -p /dev/ttyACM0 flash
idf.py -B build -p /dev/ttyACM0 monitor
```

必须从启动日志确认 `App version` 与当前提交一致，再采信设备测试结果。只看到串口 PASS、但版本仍是旧提交时，不得声称当前版本已实机验证。

### 3.2 阶段 B：并行审查与分工

可以使用子代理，但每个子代理必须在独立 worktree 或独立分支中工作，避免多个代理同时改同一文件。建议分工如下：

| 代理 | 范围 | 交付物 |
|---|---|---|
| A | `split`/`compact`/`merge` 搬移算法 | 算法证明、伪代码、故障注入测试 |
| B | allocator、TLSF、配置边界 | 复杂度分析、配置矩阵、分配器测试 |
| C | init/lifecycle、pause/borrow/lock、API 语义 | 状态转移表、竞态测试、生命周期测试 |
| D | validate、静态检查、ESP32 构建与文档 | 校验器测试、命令复现、文档对照 |

每个代理必须先输出：

1. 现状问题和精确代码位置；
2. 拟修改的函数和不修改的范围；
3. 最坏时间复杂度和额外内存；
4. 对应回归测试；
5. 失败时如何保证无半提交状态。

代理完成后由一个集成代理负责合并。集成代理不得只看“测试通过”，必须重新阅读所有改动和状态机路径。

### 3.3 阶段 C：实施顺序

推荐顺序：

1. 先修复 split 搬移顺序，新增能复现数据覆盖的测试。
2. 统一 merge/split 的暂停和锁语义。
3. 让 `init()` 配置检查和旧状态检查完全先于清理。
4. 修复 TLSF 配置位宽和 FL/SL 范围。
5. 完善整理计划的全部前置验证。
6. 完善 `validate`、`get_stats` 的有限遍历和块内边界检查。
7. 限制 `set_destroy_fn` 的适用对象。
8. 更新 README、复杂度声明、cppcheck 脚本和 ESP32 CMake 依赖。

每完成一个主题，立即运行最小回归测试；不要把所有改动积累到最后才测试。

## 4. 拆分搬移算法修复

### 4.1 当前风险

`split()` 将上半区计划存入 `s_upper`，当前按逆序执行。该序列同时可能包含：

- 跨越边界、从边界下方向上方移动的对象，目标地址向右；
- 原本在上方、因压缩向左移动的对象。

向左移动的高地址对象可能覆盖低地址对象尚未复制的源区域。

R11 当前只有“低区 pinned + 高区 pinned + 两侧连续 movable”，没有覆盖“跨界 movable + 高区有空洞”的情形。

### 4.2 推荐执行顺序

在当前布局规则下，跨边界 movable 对象至多有一个，因为地址有序的第一个跨界对象结束后，后续对象已经位于边界上方。将上半区计划拆成：

- `upper_left_moves`：源地址在边界上方，目标地址不大于源地址；
- `crossing_moves`：源地址低于边界、目标地址从边界开始，向右移动。

执行伪代码：

```text
plan_upper_in_address_order()

for each entry in upper_plan:
    if source_block_start >= boundary:
        append entry to upper_left_moves
    else:
        assert source_block_start < boundary
        assert source_block_start + size > boundary
        append entry to crossing_moves

# 所有普通上半区对象目标地址 <= 源地址。
# 升序搬移不会覆盖尚未处理的低地址源块。
for entry in upper_left_moves in ascending source address:
    memmove(entry.dst, entry.src, entry.size)
    descriptor[entry.slot].address = entry.dst + HEADER
    bump_epoch(descriptor[entry.slot].address_epoch)

# 普通对象已经离开其源区域后，再搬移跨界对象。
for entry in crossing_moves in ascending source address:
    assert entry.dst >= boundary
    memmove(entry.dst, entry.src, entry.size)
    descriptor[entry.slot].address = entry.dst + HEADER
    bump_epoch(descriptor[entry.slot].address_epoch)
```

如果未来允许多个跨界对象，不能直接套用上面的顺序，必须重新证明源/目标区间的拓扑关系；最稳妥的方案是为跨界对象使用固定 scratch 缓冲，但不能引入动态内存。

### 4.3 计划阶段必须验证

每一项计划在执行前验证：

```text
source 在原池 [start, end) 内
destination 在目标侧允许范围内
size >= PM_MIN_BLOCK
size % PM_ALIGNMENT == 0
source 和 destination 不越过 uint32 地址范围
destination 与已规划目标不重叠
pinned 对象目标地址 == 原地址
```

验证失败必须在任何 `memmove` 之前返回 `PinnedConflict`、`NoSpace` 或 `CorruptMetadata`，并恢复源池为 `Running`。

### 4.4 必须新增的 split 测试

至少构造如下布局：

```text
[空洞][跨界 movable][空洞][upper movable 1][空洞][upper movable 2]
                 ^ boundary
```

每个对象写入不同模式；split 后逐对象验证内容、descriptor 地址、池 ID、generation、epoch 和 `pm_validate()`。该测试必须在 Host Debug、Release、ASan/UBSan 和 ESP32 上运行。

## 5. merge/split 状态与锁

### 5.1 先确定唯一 API 语义

推荐统一为：维护操作自己完成状态转换，但调用者必须保证外部 DMA/ISR/线程已停止。

伪代码：

```text
merge(source, target):
    lock
    if source/target 无效: unlock; return InvalidPool
    if 任一状态不是 Running: unlock; return Busy
    if 任一 borrow_count != 0: unlock; return Busy
    source.state = Merging
    target.state = Merging
    unlock

    只读规划
    if 规划失败:
        lock
        source.state = Running
        target.state = Running
        unlock
        return error

    执行搬移与不可失败提交

    lock
    完成 source 清空、target Running、epoch 更新
    unlock
    return Ok
```

`split()` 使用同样规则。若选择“调用者必须先 pause”，则操作必须接受 `Paused` 并在锁内从 `Paused` 进入 `Splitting/Merging`；不能继续要求 `Running`。

### 5.2 锁边界

锁至少保护以下原子状态转换：

- `Running -> Paused`
- `Paused -> Compacting/Merging/Splitting`
- `Compacting/Merging/Splitting -> Running`
- borrow 开始时的状态检查与计数增加
- borrow 结束时的 token 校验与计数减少

不要在持锁期间执行大规模 `memmove`；正确做法是锁内完成“封锁新借用 + 确认计数为零”，解锁后执行搬移，最后锁内提交最终状态。

如果采用单所有者契约，必须明确：普通 `alloc/free/resolve/get_stats` 不承诺多线程安全。若要声称 SMP 安全，则所有公共状态访问都必须重新设计，不能只给 `borrow_begin` 加锁。

### 5.3 借用计数要求

计数器只能表示库已知的 borrow，不代表 DMA、ISR 或裸指针持有者。维护入口应满足：

```text
pause/封锁新借用
等待 pool.borrow_count == 0
确认外部使用者已停止
执行维护
恢复 Running
```

建议 `borrow_begin` 返回不可伪造的 token，`borrow_end` 只接受该 token；至少要验证 index、generation、pool hint、对象状态和 active count。

## 6. `init()` 生命周期修复

### 6.1 禁止先清理后校验

推荐流程：

```text
init(cfg):
    if G.initialized != 0:
        return Busy                         # 或要求显式 deinit

    if cfg.zone == null:
        return InvalidAlignment
    if cfg.segment_size 不为合法幂次:
        return InvalidAlignment
    if cfg.segment_size < 最小值:
        return InvalidAlignment
    if cfg.zone_size < cfg.segment_size:
        return InvalidAlignment
    segment_count = cfg.zone_size / cfg.segment_size
    if segment_count == 0 or segment_count > PM_MAX_SEGMENTS:
        return NoSpace
    if zone_size/segment_size 计算存在溢出:
        return NoSpace
    if zone 地址未按 PM_ALIGNMENT 对齐:
        return InvalidAlignment

    # 只有到这里才允许初始化全局状态
    memset(G, 0)
    初始化 descriptor free-slot 链
    G.initialized = 1
    return Ok
```

如果产品确实需要“无 live object 时重新 init”，也必须先验证 `G.live_object_count == 0`、所有池无维护状态，并在文档中写清楚；不得在存在 live object 时静默清空。

### 6.2 必须新增测试

- 已初始化且有 live object 时再次 `init`，返回 `Busy`，原对象仍可访问。
- 已初始化但传入超大 segment 数时返回 `NoSpace`，原池和对象保持不变。
- 未初始化时所有非法配置均不会留下半初始化状态。
- `deinit`、重新 `init` 后 generation/统计状态符合文档定义。

## 7. TLSF 配置与复杂度

### 7.1 位图宽度

当前 `sl_bitmap` 是 `uint16_t`，因此有两个合法方案：

**方案 A，推荐 v1：** 将 `PM_SL_COUNT` 的静态上限限制为 16，并在配置测试中明确 `32` 必须编译失败。

**方案 B：** 将位图和所有掩码改为 32 位，并完整检查 `ctz`、移位和最高位行为。

不能保留“配置允许 32，但运行时截断”的状态。

### 7.2 FL 范围

加入约束，保证所有表达式安全：

```text
PM_FL_MAX > MIN_FL
PM_FL_MAX <= 31                 # 1u << PM_FL_MAX 不溢出
FL_COUNT <= 32                  # fl_bitmap 为 uint32_t
PM_MAX_SEGMENTS <= UINT16_MAX   # Pool 字段为 uint16_t
PM_MAX_OBJECTS < UINT16_MAX     # NO_SLOT 保留值
```

`fl_index()` 不得把不可表示的大块静默 clamp 到最高 bin。要么初始化时拒绝超出 FL 能力的 zone，要么显式返回 `NoSpace/InvalidConfig`。

### 7.3 复杂度声明必须诚实

修复后的 `bins_find()` 会在同一 bin 内 first-fit 遍历，因此其实际最坏复杂度不是严格 O(1)：

| 操作 | 当前/目标最坏复杂度 |
|---|---|
| `alloc` | O(该 bin 链长度)，上界 O(zone_size / PM_MIN_BLOCK) |
| `free` | O(1)，前提是块头和相邻 free 链有效 |
| `pause/resume` | O(1) |
| `compact` | O(object_count + moved_bytes) |
| `merge` | O(object_count + moved_bytes) |
| `split` | O(object_count + moved_bytes) |
| `validate` | 当前含 live/free 交叉检查，最坏 O(live_objects * free_blocks) |
| `get_stats` | O(free_blocks)，必须有遍历上限 |

如果产品必须保证严格 O(1) 分配，需要改变 bin 内组织方式，例如增加按块大小的固定容量结构；不能只在文档里继续写 O(1)。

## 8. 整理计划的完整前置验证

方案 A 只有在执行阶段确实不可失败时成立。`compact_impl`、`merge`、`split` 在任何搬移前必须验证：

```text
所有 order list 节点 index 合法且无环
所有 descriptor state == Live
pool_id 正确
generation != 0
address - HEADER 在池范围内
block_size >= PM_MIN_BLOCK
block_size % PM_ALIGNMENT == 0
size <= block_size - HEADER
address + size 不溢出且不越界
所有源块互不重叠
所有目标块互不重叠
目标块不覆盖仍需读取的源块，或执行顺序已证明安全
pinned 屏障满足布局约束
统计量不会溢出
```

执行阶段只允许：

```text
memmove 已通过验证的区间
更新 descriptor 地址和 epoch
写入已验证范围内的块头
重建 bins、链表和统计
```

执行阶段不能再返回普通错误。若 debug 断言失败，应视为库内部 bug 或未满足前置条件，而不是假装事务已回滚。

## 9. `validate()` 和 `get_stats()` 加固

### 9.1 descriptor 与物理块关系

增加以下检查：

```text
d.block_size >= PM_MIN_BLOCK
d.block_size % PM_ALIGNMENT == 0
d.size <= d.block_size - BLOCK_HEADER_SIZE
d.address 对齐
d.address - HEADER 与 d.block_size 完整位于池内
```

避免在比较之前执行可能产生未定义行为的指针减法/加法。对外部可破坏元数据的场景，优先使用 zone offset 和整数范围检查，再转换为指针。

### 9.2 所有链表遍历有限步数

```text
max_order_steps = PM_MAX_OBJECTS + 1
max_free_steps = pool_capacity / PM_MIN_BLOCK + 1

for step in 0 .. max_steps:
    if node == NULL_OFF: break
    if node offset 不在 zone: return CorruptMetadata
    继续检查
else:
    return CorruptMetadata
```

`get_stats()`、`bins_find()`、`merge()`、`split()`、`compact()` 的规划遍历都要考虑环和越界；不能只有 `validate()` 防环，其他接口却可能死循环。

### 9.3 块覆盖完整性

最可靠的校验方式是从 pool start 按物理块头向 pool end 走一遍，同时维护：

```text
covered = 0
每个块都必须 >= PM_MIN_BLOCK，或明确标记为合法 slack
used block 必须对应一个 descriptor
free block 必须恰好出现于一个 bin
所有 gap 必须是合法 fragment
covered 最终必须等于 capacity
```

不能只依赖 `free_total + fragment == free_bytes`，因为被篡改的统计字段可能掩盖缺失区间。

## 10. `set_destroy_fn()` 和对象生命周期

当前 API 允许给任意 live descriptor 设置销毁回调，但注释只承诺 pinned 非平凡对象。请选择并统一一种方案：

### 推荐方案

```text
set_destroy_fn(ref, fn):
    check ref
    if descriptor.flags 不含 PM_PINNED:
        return InvalidRef 或 NotRelocatable
    descriptor.destroy_fn = fn
    return Ok
```

这样可以避免 movable 对象在整理后以不明确定义的字节搬移方式参与析构。

如果必须允许 movable + destroy_fn，则必须新增项目级 trait，证明该类型既可字节搬移又可在最终地址调用析构，并增加构造、压缩、销毁完整测试。

构造函数和销毁回调重入规则必须明确：

- 不得让正在构造/销毁的对象进入维护搬移流程；
- 若允许回调操作其他对象，必须验证不会触发当前池的维护；
- 最好增加 `Constructing/Destroying` 期间的维护禁止状态或 reentrancy guard。

## 11. 测试设计

### 11.1 必须新增的定向测试

1. split：跨界 movable + 上方空洞 + 两个以上 upper movable。
2. split：跨界对象刚好位于边界、刚好跨越边界、接近池尾。
3. merge/split：暂停状态下调用的明确返回值。
4. merge/split：借用计数变化与状态转换的并发模型测试。
5. 重复 `init()` 和非法 `init()` 不破坏旧状态。
6. `PM_SL_COUNT=2/4/8/16` 正常，`32` 按选定方案拒绝或正常运行。
7. `PM_FL_MAX=31`、非法大于 31、超大 zone 的配置测试。
8. descriptor `size > block_size - HEADER` 时 `validate/resolve` 拒绝。
9. 损坏 free list 后 `get_stats` 有限返回。
10. movable 对象设置 destroy callback 的明确拒绝或完整支持。
11. compact 中 movable descriptor 越界、未对齐、size/block 不一致的故障注入。
12. 统计字段、链表、位图、块头分别损坏时的恢复/拒绝行为。

### 11.2 模型测试

建议增加一个不使用 PondMerge 内部结构的简单参考模型：

```text
model = 有序 live intervals + free intervals
随机 alloc/free/compact/split/merge
每一步比较：
    live object 数
    每个对象 payload
    每个对象所属 pool
    可分配性
    free/used 总和
```

随机测试必须固定 seed，并在失败时打印 seed、操作序列和最后布局，确保其他 AI 可以复现。

### 11.3 测试完整性检查

集成代理必须确认：

```text
原有 13 组测试仍存在
R1-R12 仍存在
没有通过降低压力次数、删除 CHECK 或屏蔽失败路径来“修复”测试
Host Debug/Release/ASan/UBSan 都运行新增测试
ESP32 使用同一 suite.cpp 并运行关键新增测试
```

## 12. 自我检测和审查清单

### 12.1 静态检查

- `git diff --check`
- `cppcheck` 告警逐条处理或明确抑制原因
- `-Wall -Wextra -Werror` 下核心组件无新增告警
- Host 和 ESP32 都以 C++17 编译核心库
- 搜索所有 `memmove`，逐个确认源/目标区间证明
- 搜索所有 `PM_LOCK`、状态写入和 `borrow_count` 访问，画出锁边界
- 搜索所有 `memset(&G`、`memset(&pool`，确认不会绕过生命周期检查

### 12.2 动态检查

```sh
tests/run_host.sh
tests/run_host.sh --release
tests/run_host.sh --san 3000
tests/run_host.sh --cppcheck
```

额外运行：

- 固定 seed 的 10000+ 随机压力；
- split crossing 专项测试；
- 配置矩阵编译/运行；
- 故障注入测试；
- 长时间重复 init/deinit；
- ESP32 实机多次复位后重复运行。

### 12.3 结果可信度

每次设备测试必须记录：

```text
芯片型号和 revision
串口设备路径
App version / git commit
ESP-IDF 版本
编译标准
固件 SHA 或烧录时间
检查数量和 failures
```

如果监视器出现 flashed/built checksum mismatch，必须重新烧录后再报告结果。

## 13. 禁止的“伪修复”

- 不得把 `PM_DEBUG` 关闭作为修复内存损坏的手段。
- 不得在 `validate()` 失败时直接清空池或静默重建，除非 API 明确这是恢复操作。
- 不得删除已有测试或减少压力次数来隐藏问题。
- 不得把所有对象标记为 pinned 来绕过搬移算法，除非产品明确接受失去整理能力。
- 不得在 movable 数据上使用临时动态分配来规避覆盖问题。
- 不得只修改 README 而不修实现，也不得只修实现而不更新契约。
- 不得继续声称 allocator 严格 O(1)，除非实现确实消除了 bin 内线性遍历。
- 不得通过扩大 `PM_MAX_*`、放宽边界或忽略错误码来掩盖配置错误。

## 14. 最终验收标准

本轮只有同时满足以下条件才算完成：

1. split crossing 专项测试通过，且没有 payload 被覆盖。
2. merge/split 的 pause、borrow、state、lock 语义在代码、文档和测试中一致。
3. 非法或重复 `init()` 不会破坏已有状态。
4. 所有声明支持的 `PM_SL_COUNT`/`PM_FL_MAX` 配置都能正确工作，声明不支持的配置会在编译期或初始化期明确拒绝。
5. 整理执行前完成全部前置验证，执行阶段不存在未处理的普通失败路径。
6. `validate()` 和 `get_stats()` 对损坏链表有限返回，不死循环、不越界。
7. descriptor payload、物理块、统计、位图和链表关系均有校验。
8. `set_destroy_fn()` 的 movable/pinned 语义明确，并有回归测试。
9. Host Debug、Release、ASan/UBSan、静态检查和 ESP32-S3 实机均通过。
10. 报告中列出每项修复对应的测试、命令、检查数量、提交号和设备固件版本。

## 15. 交付报告模板

完成后要求执行 AI 提交以下摘要：

```text
基线提交：
最终提交：

修改文件：
- ...

问题 -> 修复函数 -> 回归测试：
- split 覆盖 -> ... -> split_crossing_order
- init 重置 -> ... -> init_preserves_live_state
- ...

复杂度变化：
- alloc: ...
- compact: ...
- validate: ...

Host Debug：... checks, ... failures
Host Release：... checks, ... failures
ASan/UBSan：... checks, ... failures
cppcheck：退出码 ...，告警 ...
ESP32：芯片 ...，App version ...，... checks, ... failures

未解决风险：
- ...
```

任何“未解决风险”都必须明确写出，不得用“全部通过”替代设计边界说明。

