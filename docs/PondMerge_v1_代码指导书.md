# PondMerge v1 代码指导书

## 0. 文档目的

本文档是 PondMerge v1 的实现任务书。实现者应按本文档实现公开语义、数据结构、算法和错误处理；未列出的行为不得自行扩展为新的核心语义。

本文档面向无 MMU 的 32 位 MCU，使用 C++17 子集实现。默认不依赖异常、RTTI、动态链接和操作系统线程。若项目使用 C++11/14，只需替换少量语言特性，不改变核心模型。

## 1. v1 必须保证的语义

1. PondMerge 只管理明确放入 Auto Zone 的对象。
2. 整理、池合并和池拆分都由调用方显式触发，不在 ISR 或后台线程中自动触发。
3. 普通物理指针只能在借用期间使用；借用期间禁止相关池发生搬迁。
4. `pm_ptr<T>` 是逻辑引用，不是普通 `T*`。对象搬迁后，逻辑引用在下一次使用时惰性修正。
5. 对象的逻辑身份在整理、合并和拆分后保持不变。
6. Auto Zone 内的对象不能保存未经处理的自动池物理地址。
7. DMA、外设、未知外部代码和无法暂停的并发访问不得使用可搬迁对象。
8. 整理发现仍有活跃借用时必须返回 `BUSY`，不得无限等待或强行搬迁。
9. v1 不提供 MPU 等硬件越界保护；三区边界属于软件契约。
10. Auto Zone 总容量由启动时的链接布局决定，临时池只能重新利用预留空间。

## 2. 总体内存布局

由链接脚本或启动配置提供以下区域：

~~~text
Chaos Zone   : 外设、DMA、硬件共享缓冲区，PondMerge 永不触碰
Free Zone    : 开发者自行使用，PondMerge 不记录、不整理
Auto Zone    : PondMerge 的固定总内存，内部划分为多个 Pool
Metadata     : 固定地址的 PondMerge 元数据，不参与搬迁
~~~

建议的初始配置：

~~~text
PM_AUTO_START          = linker symbol
PM_AUTO_SIZE           = 256 KiB（按产品 RAM 调整）
PM_SEGMENT_SIZE        = 4 KiB
PM_MAX_SEGMENTS        = PM_AUTO_SIZE / PM_SEGMENT_SIZE
PM_MAX_POOLS           = 16
PM_MAX_OBJECTS         = 1024
PM_ALIGNMENT           = 8 bytes
PM_MAX_ALIGNMENT       = 8 bytes（v1 超过此值返回 InvalidAlignment）
PM_MIN_BLOCK           = 16 bytes
PM_MAX_OBJECT_SIZE     = PM_AUTO_SIZE - metadata overhead
~~~

`PM_SEGMENT_SIZE` 是池空间调整的最小单位，不是对象大小限制。对象可以跨越多个 segment。

没有 MPU 时，链接布局只能减少误用，不能阻止野指针跨区写入。文档、代码审查和 Debug 检查必须承担这部分责任。

## 3. Pool 数据结构

池是 Auto Zone 内一段或多段连续 segment 的逻辑拥有者。v1 允许一个池拥有多个相邻 segment，以便支持扩容和拆分。

建议结构：

~~~cpp
enum class PoolState : uint8_t {
    Empty,
    Running,
    Paused,
    Compacting,
    Merging,
    Splitting,
};

struct Pool {
    uint16_t id;
    PoolState state;
    uint16_t segment_first;
    uint16_t segment_count;
    uint32_t borrow_count;
    uint32_t structure_epoch;
    uint32_t used_bytes;
    uint32_t free_bytes;
    FreeBin bins;
    AddressList address_list;
    CriticalSection lock;
};
~~~

普通运行期间池的 segment 所有权和物理范围保持稳定。合并、拆分是显式结构迁移，完成后对象所属池可以改变。

池元数据必须放在固定的 Metadata 区域，不能放在会被整理的对象区内。

## 4. 对象描述符

每次分配创建一个固定槽位的对象描述符。描述符槽位本身不能移动。

~~~cpp
enum ObjectFlags : uint16_t {
    PM_MOVABLE       = 1u << 0,
    PM_PINNED        = 1u << 1,
    PM_DMA           = 1u << 2,
    PM_EXTERNAL      = 1u << 3,
    PM_ZERO_INIT     = 1u << 4,
};

enum class ObjectState : uint8_t {
    Free,
    Live,
    Destroying,
};

struct ObjectDesc {
    void*    address;       // 当前 payload 地址
    uint32_t size;
    uint32_t alignment;
    uint16_t pool_id;
    uint16_t flags;
    uint16_t generation;    // 生命周期代际；0 保留为无效值
    uint32_t address_epoch; // 地址或所属池变化时增加
    uint32_t active_borrows;
    uint16_t next_free_slot;
    uint32_t address_order_prev;
    uint32_t address_order_next;
    void (*destroy_fn)(void*);
    ObjectState state;
};
~~~

`object_id` 是描述符槽位索引，不因搬迁改变。`generation` 是对象生命周期代际，只在释放并重新使用槽位时增加，用来防止 ABA 式旧引用误用。`address_epoch` 在对象搬迁或换池时增加，用于让缓存地址惰性失效。两个计数达到最大值后都跳过 0；Debug 模式应报告即将回绕。

## 5. 逻辑指针模型

推荐的公共引用结构如下：

~~~cpp
struct PmRef {
    uint32_t object_index;
    uint16_t generation;
    uint16_t pool_hint;     // 0xFFFF 表示允许跨池
    uint32_t object_offset;
};
~~~

`pool_hint` 只是池内访问限制或快速检查信息，不是对象永久身份。池合并后对象可以换池，`object_index` 仍保持有效。

引用分为两种语义：

- `pm_local_ptr<T>`：`pool_hint` 为具体池 ID，只保证对象在该池内整理后继续有效。若池合并或拆分导致对象换池，解析返回 `PoolChanged`；它不会悄悄跨池。
- `pm_cross_ptr<T>`：`pool_hint` 为 `0xFFFF`，允许对象在池合并或拆分后跟随当前所属池。创建跨池引用必须显式调用 `pm_as_cross()` 或 `pm_cross_ref()`。

底层结构可以共用 `pm_ptr<T>`，但类型别名或包装类必须让这两种构造路径在代码审查中可见。普通业务分配默认返回 `pm_local_ptr<T>`。

`pm_ptr<T>` 还应缓存最近一次解析结果：

~~~cpp
template<class T> class pm_access_proxy;

template<class T>
class pm_ptr {
    PmRef ref_;
    mutable T* cached_address_;
    mutable uint32_t cached_address_epoch_;

    // pm_ptr 实例本身不保证多线程并发使用；跨线程请复制引用或使用外部锁。
public:
    pm_access_proxy<T> operator->() const;
};
~~~

`pm_local_ptr<T>` 和 `pm_cross_ptr<T>` 可以是带不同构造策略的包装类型，也可以是同一实现的强类型别名。无论采用哪种写法，必须保证普通分配默认生成带 `pool_hint` 的 local 版本，只有显式转换才能生成 cross 版本。

解析规则：

1. 检查 `object_index` 是否在范围内。
2. 检查描述符 state == Live，且生命周期 generation 相等。
3. 检查 `object_offset + sizeof(T)` 不溢出并且不超过对象长度。
4. 检查对齐要求。
5. 若缓存 address_epoch 相同，返回缓存地址加 offset。
6. 若 address_epoch 不同，重新读取描述符地址并更新缓存。

对象释放时，引用必须失效。安全路径应提供返回状态的 `try_borrow()` 或 `access()`，避免 `operator->` 无法表达错误。

池内引用必须写入创建它时的 `pool_hint`；池内 API 解析时要求当前 `desc.pool_id == pool_hint`。跨池引用将 `pool_hint` 设为 `0xFFFF`，只检查对象身份，不限制当前所属池。这样普通池内指针不会悄悄变成跨池引用。提供显式转换：

~~~cpp
pm_cross_ptr<Packet> cross = pm_as_cross(local);
~~~

如果需要 C++ 指针语法，`operator->` 应返回一个短生命周期的 access proxy，而不是直接暴露裸地址：

~~~cpp
pm_ptr<Packet> packet;
packet->data[index] = value;
~~~

proxy 在完整表达式期间持有一次借用，因此该语句执行时整理不能发生。每次 `operator->` 至少需要一次版本检查和一次借用计数更新。热循环应显式持有 `pm_access<T>`，避免每个表达式重复加减计数。

## 6. 借用机制与计数器

借用表示“当前代码正在使用真实物理地址”。保存一个 `pm_ptr` 不算借用。

~~~cpp
template<class T>
class pm_access {
public:
    pm_access(pm_access const&) = delete;
    pm_access& operator=(pm_access const&) = delete;
    pm_access(pm_access&&) noexcept;
    ~pm_access();

    T* operator->() const;
    T& operator*() const;
};

template<class T>
class pm_access_proxy {
    pm_access<T> access_;    // proxy 存活到完整表达式结束
public:
    T* operator->() const;
};
~~~

获得借用的流程必须在池锁或短临界区内完成：

~~~text
    lock(pool)
        如果 pool.state != Running，返回 BUSY
        验证引用有效
        borrow_count += 1
        desc.active_borrows += 1
        保存当前物理地址
unlock(pool)
~~~

释放借用时同样在保护区内执行 `borrow_count -= 1` 和 `desc.active_borrows -= 1`。池级计数用于阻止整理，对象级计数用于阻止单个对象被释放。两个计数器都至少使用 32 位，禁止溢出和下溢；异常应触发断言或返回错误。

单线程 MCU 可用关中断实现保护区；多线程系统使用池锁或目标平台提供的原子/临界区接口。不能先读取计数为 0、释放锁、再开始整理，因为这会产生竞争窗口。

## 7. 分配算法：TLSF 风格分离适配

v1 使用 Two-Level Segregated Fit（TLSF 风格）而不是单一 first-fit。目标是：

- 分配和释放具有可预测的近似 O(1) 时间；
- 释放时可立即合并相邻空闲块；
- 不因大量小块搜索整个池；
- 整理前仍保留足够的碎片统计信息。

### 7.1 块格式

每个物理块包含边界标签：

~~~text
前向块大小/状态      4 bytes
后向块大小            4 bytes
payload               8-byte aligned
~~~

最小块大小为 16 字节。剩余空间小于 16 字节时不得拆分，直接并入已分配块。

对象描述符中的 `address` 指向 payload，不指向块头。

### 7.2 Bin 参数

默认：

~~~text
最小可分配块       16 bytes
对齐               8 bytes
SL（二级分类）     4
FL 范围             log2(16) 到 log2(Auto Zone 上限)
~~~

对于 256 KiB Auto Zone，FL 大致覆盖 4 到 18，共 15 个一级范围；每个一级范围有 4 个二级 bin，总计约 60 个 bin。一级和二级非空状态分别用位图保存。

请求大小先加上块头和对齐开销，再映射到 `(FL, SL)`。在目标 bin 没有合适块时，寻找更大一级范围的第一个非空 bin。

建议的映射伪代码如下；实现可以使用平台的 `clz` 指令，也可以使用查表版本：

~~~text
rounded = align_up(request + BLOCK_HEADER_SIZE, PM_ALIGNMENT)
fl = floor_log2(rounded)
base = 1 << fl
sl = ((rounded - base) * SL_COUNT) / base
sl = min(sl, SL_COUNT - 1)
~~~

当 `rounded < 2^MIN_FL` 时使用 `MIN_FL`。从 bin 取块时必须再次检查 `block_size >= rounded`；位图只负责快速找到候选范围，不能替代尺寸检查。

每个 bin 使用双向空闲链表。删除和插入必须同时更新 `fl_bitmap`、对应的 `sl_bitmap[fl]` 和链表头。Debug 模式应在每次变更后验证“位图非空当且仅当链表非空”。

### 7.3 分配流程

~~~text
1. 检查 pool.state == Running
2. 检查 size、alignment、flags
3. 将 size 向上取整到 PM_ALIGNMENT
4. 计算所需块大小
5. 通过位图找到第一个可用 bin
6. 从 bin 取出块
7. 剩余空间 >= PM_MIN_BLOCK 时拆分
8. 写入块头和 ObjectDesc
9. 设置 generation 为非零初值、address_epoch 为 1、active_borrows 为 0
10. 将对象插入 address_order 链表
11. 返回 PmRef
~~~

### 7.4 释放流程

~~~text
1. 验证 object_id、generation、pool 所属关系
2. 如果对象的 active_borrows 不为 0，返回 BUSY
3. 将对象标记为 Destroying，阻止新的引用解析
4. 如有 destroy_fn，在不持有内部池锁的情况下调用析构
5. 重新取得池锁并确认对象仍为 Destroying
6. 从 address_order 链表移除
7. 将块标记为空闲
8. 与前后相邻空闲块合并
9. 插入正确的 TLSF bin
10. 增加对象 generation，设置 state = Free；保留 address_epoch 或重新初始化为非零值
~~~

## 8. 池内压缩算法

v1 使用“地址顺序稳定打包”的全量压缩算法。整理只在池已暂停且 `borrow_count == 0` 时执行。

### 8.1 无固定对象时

1. 从 `address_order` 链表头开始遍历所有存活对象。
2. `cursor` 从池起始地址开始；v1 所有块按 8 字节对齐，块头为 8 字节，因此 payload 同样满足 8 字节对齐。
3. 如果对象当前块地址不等于 cursor，使用 `memmove(cursor, old_block, block_size)` 搬迁整个块（包括块头）。
4. 搬迁完成后更新 ObjectDesc 的 address 和 address_epoch。
5. cursor 移过当前块的完整大小。
6. 遍历结束后，在 cursor 到池末尾之间创建一个大空闲块。
7. 清空并重建全部 TLSF bin。

保持原地址顺序可以减少不必要的移动，也让压缩结果确定、便于测试。

### 8.2 存在 pinned 对象时

固定对象是不可穿越的屏障。将池划分为两个 pinned 对象之间的区间，对每个区间单独打包可搬迁对象：

1. 找到区间内的第一个 movable 对象。
2. 将对象按地址顺序向区间起点压紧。
3. 不得越过下一个 pinned 对象的起始地址。
4. 在区间末尾生成空闲块。
5. 固定对象本身不移动，保留其 alignment 和原地址。

如果 pinned 对象之间的空闲空间小于最小块，保留为不可用碎片并计入统计。

压缩前必须先建立只读规划结果，包含每个 movable 对象的 `old_block`、`new_block` 和 `block_size`。规划阶段发现任意目标地址会碰到 pinned 屏障、越过池边界或不满足 alignment 时，整个操作返回错误，不能只完成一部分。执行阶段按地址从低到高调用 `memmove`；所有地址写入成功后再一次性重建地址链表和 TLSF bins。

### 8.3 压缩复杂度

设对象数量为 N，实际搬迁字节数为 M：

~~~text
元数据扫描       O(N)
地址顺序打包     O(N)
数据搬迁         O(M)
~~~

压缩没有隐藏线程，也没有在普通访问路径中执行。实现必须记录 `objects_moved`、`bytes_moved`、`largest_free_block` 和耗时计数，便于产品估算维护窗口。

压缩执行期间禁止调用普通分配、释放、借用和池拓扑操作。建议使用 `CompactionGuard` 在构造时把状态设为 `Compacting`，在析构时恢复状态；发生校验失败时恢复到 `Paused` 并要求调用方显式 `resume`，防止错误路径把池留在可写但元数据未完成的状态。

## 9. 整理状态机

推荐公开接口：

~~~cpp
enum class PmStatus {
    Ok,
    Busy,
    NoSpace,
    InvalidPool,
    InvalidRef,
    InvalidAlignment,
    PinnedConflict,
    NotRelocatable,
    PoolChanged,
    CorruptMetadata,
};

PmStatus pm_pause(PoolId);
PmStatus pm_resume(PoolId);
PmStatus pm_compact(PoolId);
~~~

`pm_compact()` 可以内部完成暂停和恢复，也可以提供由调用方控制生命周期的低层接口。两种接口必须共享同一套状态检查。

`pm_destroy_pool()` 只能销毁 `Empty` 池。池中仍有存活对象、active borrow 或未完成的拓扑操作时，必须返回 `Busy` 或相应错误，不能直接回收 segment。

整理遇到以下情况必须停止并返回错误：

- `borrow_count != 0`；
- 元数据校验失败；
- 对象 alignment 无法满足；
- pinned 屏障导致目标对象无处放置；
- 发现不可搬迁对象未标记。

## 10. 池合并算法

v1 的合并只处理两个物理相邻的池，把源池的 segment 所有权转移给目标池。

~~~text
1. 验证源池和目标池存在且不相同
2. 暂停所有相关池
3. 确认所有相关池 borrow_count == 0
4. v1 只允许物理相邻池合并；确认合并后的 segment 范围连续
5. 确认所有 pinned 对象仍位于其原物理地址，不允许搬迁 pinned 对象
6. 将源池 segment 所有权转移给目标池
7. 更新源池对象的 pool_id；即使物理地址不变，也增加 address_epoch，使池归属缓存失效
8. 对合并后的完整范围执行一次 compact，使 movable 对象在 pinned 屏障内重新排列
9. 重建目标池 address_order 和 TLSF bins
10. 源池变为 Empty，目标池 structure_epoch 增加
11. 恢复池状态
~~~

如果池不相邻、存在无法保留物理地址的 pinned 对象，或合并后容量规划失败，应先返回 `PinnedConflict` 或 `NO_SPACE`。不得边迁移边覆盖。

## 11. 池拆分算法

拆分用于把一个长期过大的池划分为两个池，释放部分 segment 给其他用途。

~~~text
1. 暂停源池
2. 确认 borrow_count == 0
3. 选择按 segment 对齐的拆分边界
4. 识别跨越边界的对象
5. 先生成迁移计划；将跨界的 movable 对象迁移到选定侧的可用空间
6. 检查 pinned 对象是否跨越边界；跨越时不能移动该对象，必须重新选择边界
7. 创建新池并转移 segment 所有权
8. 更新受影响对象的 pool_id 和 address；即使地址不变，也增加 address_epoch
9. 分别重建两个池的地址链表和 TLSF bins
10. 增加相关 structure_epoch
11. 恢复池状态
~~~

拆分边界只能落在 segment 边界。没有足够的预留 segment，或者 pinned 对象无法满足边界要求时返回 `NO_SPACE` 或 `PinnedConflict`。

拆分必须是事务式的：先在只读阶段计算两侧容量、跨界对象数量和临时空间需求；规划失败时不改变任何对象和池所有权。只有规划成功后才执行搬迁。执行失败时应保留源池为 `Paused`，返回 `CorruptMetadata` 或具体搬迁错误，由调用方决定是否复位，而不是继续让业务访问半完成的两个池。

## 12. 临时池

临时池是预留的普通 Auto Zone 池，用于：

- 拆分时暂存跨边界对象；
- 业务升级或批量重排期间保存中间数据。

临时池不改变 Auto Zone 总容量。临时池中的对象仍必须遵守可搬迁规则；如果数据正在被 DMA 或外部驱动使用，不能因为放入临时池就变得可搬迁。

## 13. 不可搬迁对象规则

以下对象必须放入 Chaos Zone，或者在 Auto Zone 中标记为 `PM_PINNED`：

- DMA 正在读写的源/目标缓冲区；
- 显示、音频、摄像头、网络控制器正在使用的零拷贝缓冲区；
- ISR 或其他线程仍保存普通物理指针的数据；
- 已把地址交给无法暂停或无法追踪的外部库/驱动的数据；
- 要求固定物理地址、固定 RAM bank 或特殊对齐的数据；
- 含有自动池裸地址且没有重定位处理的数据；
- 正在进行无锁并发访问的数据。

普通 `volatile` 类型并不自动等于不可搬迁，但只要其访问者是外部硬件或无法暂停的执行上下文，就必须固定。

v1 默认不扫描和修复对象内部的裸指针。对象内部引用必须使用 `pm_ptr` 或相对偏移。非平凡 C++ 对象的自定义 relocate hook 不属于 v1 必须实现的功能；需要此能力的对象应先标记为 `PM_PINNED`。

## 14. C++ 对象可搬迁约束

默认只允许以下类型进入 `PM_MOVABLE`：

- 平凡可复制类型；
- 不保存自动池绝对地址；
- 不依赖固定物理地址；
- 不被 DMA 或不可暂停上下文使用。

非平凡类型必须标记为 `PM_PINNED`。v1 的 movable 对象只允许满足项目定义的 trivially relocatable trait 的类型；不能仅依赖 `std::is_trivially_copyable`，还必须确认内部没有自动池裸地址。

不要仅以 `std::is_trivially_copyable` 判断安全性；它无法判断一个结构体内部是否保存了失效的裸地址。

## 15. Debug、Release 和完整性检查

Debug 必须支持：

- 引用 generation 检查；
- 对象边界和 alignment 检查；
- 池归属检查；
- borrow_count 状态检查；
- double free 和 invalid free 检查；
- 空闲链表一致性检查；
- 对象链表与描述符反向一致性检查；
- 压缩前后 live bytes 总和检查。

每个池应提供 `pm_validate(pool)`，至少验证：

~~~text
所有对象不重叠
所有对象属于池 segment
所有对象地址满足 alignment
空闲块不重叠且已入正确 bin
used_bytes + free_bytes == pool capacity - overhead
address_order 链表无环
~~~

Release 可以关闭昂贵的全量校验，但不能关闭 generation、状态和关键边界检查。

## 16. 建议的公共 API

API 名称可以按项目规范调整，但语义应保持一致：

~~~cpp
PoolId   pm_create_pool(PoolConfig const&);
PmStatus pm_destroy_pool(PoolId);

template<class T, class... Args>
Result<pm_local_ptr<T>> pm_make(PoolId, Args&&...);

PmStatus pm_free(pm_ref);

template<class T>
Result<pm_access<T>> pm_borrow(pm_local_ptr<T> const&);

template<class T>
Result<pm_access<T>> pm_try_borrow(pm_cross_ptr<T> const&);

template<class T>
Result<pm_access<T>> pm_borrow(pm_cross_ptr<T> const&);

template<class T>
pm_cross_ptr<T> pm_as_cross(pm_local_ptr<T> const&);

PmStatus pm_compact(PoolId);
PmStatus pm_merge(PoolId source, PoolId target);
PmStatus pm_split(PoolId source, SplitPlan const&);

PoolStats pm_get_stats(PoolId);
PmStatus pm_validate(PoolId);
~~~

`pm_make()` 对 movable 类型应在最终地址构造，且要求项目定义的 trivially relocatable trait。v1 的 `memmove` 只适用于这类可搬迁对象；含资源句柄、非平凡析构或自定义搬迁语义的 C++ 对象默认使用 `PM_PINNED`。如需支持 pinned 的非平凡对象，描述符应保存 destroy_fn，并要求调用方保证没有其他逻辑引用正在访问该对象。

## 17. 错误结果

所有管理操作都应返回明确结果：

~~~text
OK
BUSY
NO_SPACE
INVALID_POOL
INVALID_REFERENCE
INVALID_ALIGNMENT
PINNED_BLOCK
NOT_RELOCATABLE
ALREADY_PAUSED
CORRUPT_METADATA
~~~

不能整理时，PondMerge 必须失败返回，不能强行移动数据。

## 18. 测试与验收标准

必须实现以下测试组：

1. 连续分配、释放、重复利用。
2. 随机分配/释放 10000 次后执行整理。
3. 整理前后所有 `pm_ptr` 仍指向同一个逻辑对象。
4. 整理后活跃的 `pm_access` 使整理返回 `BUSY`。
5. generation/address_epoch 不匹配、double free、越界 offset 都能被捕获。
6. pinned 对象前后都有碎片时，压缩不会越过 pinned 地址。
7. DMA、外部对象和固定地址对象不会被搬迁。
8. 池合并后源对象引用仍可解析。
9. 池拆分后跨边界对象引用仍可解析。
10. Auto Zone 满、临时池满、segment 不足时返回确定错误。
11. 所有对象大小、对齐、最大值边界测试。
12. Debug 下每个公共操作后调用 `pm_validate()`。

压力测试应记录：

~~~text
最大活跃对象数
最大 borrow_count
最大碎片率
最大搬迁字节数
最大整理耗时
最大元数据占用
~~~

## 19. 实现顺序

建议按以下顺序提交代码，避免同时引入多个不易定位的问题：

1. 固定 Metadata、Auto Zone 和 segment 管理。
2. ObjectDesc、generation、address_epoch 和句柄校验。
3. TLSF 风格分配、释放和相邻块合并。
4. `pm_ref`、`pm_ptr` 和惰性解析。
5. RAII `pm_access` 与 borrow_count。
6. 无 pinned 对象的池内压缩。
7. pinned 屏障和区间压缩。
8. 池暂停、状态机和错误码。
9. 池合并。
10. 池拆分和临时池。
11. Debug 完整性检查、统计和压力测试。

每个阶段完成后先通过单元测试和 `pm_validate()`，再进入下一阶段。

## 20. 明确的非目标

PondMerge v1 不负责：

- 监控并修复任意裸指针；
- 在中断上下文中整理；
- 自动判断 DMA 是否完成；
- 自动分析对象内部指针；
- 把 Auto Zone 扩展到链接布局以外；
- 提供硬件级内存隔离；
- 保证一次完整压缩满足任意硬实时上限。

## 21. 最终实现定义

PondMerge v1 是一个固定 Auto Zone 上的、基于对象描述符的可搬迁内存池系统。

对象通过稳定的逻辑编号被引用；实际地址保存在固定元数据中。池内压缩使用地址顺序稳定打包，空闲块使用 TLSF 风格分离适配管理。普通物理指针通过 RAII 借用、对象级计数器和池级计数器保护；长期保存的 `pm_ptr` 在对象搬迁后按 address_epoch 惰性更新。池合并和拆分通过稳定对象编号保持引用关系，临时池只使用预留 Auto Zone 空间。
