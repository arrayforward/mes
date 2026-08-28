// ============================================================================
// 文件: types.h
// 模块: stmb_core(核心数据类型,无依赖)
// 用途: 定义整个 stmb 服务共享的基础数据类型与几何判定接口:
//         - TimeStamp   : 时间戳(64 位整型,单位毫秒);
//         - TimeRange   : 闭区间时间范围,支持 contains / overlaps;
//         - AABB        : 轴对齐包围盒(三维空间区域),支持 contains / intersects;
//         - CellCoord   : 空间格子坐标(空间索引的键);
//         - BlockKey    : 记忆块逻辑键(格子坐标 + 时间槽),支持 == 与哈希;
//         - MemoryBlock : 记忆块本体(键 + 区域 + 负载 + 时间戳 + 版本 + 状态机
//                         字段 + 置信度相关字段);
//         - BlockState  : 块状态机(Pending -> Stable -> Changing -> Stable);
//         - makeBlockKey: 由 AABB 中心点与时间戳计算 BlockKey 的工具函数;
//         - decayedConfidence: 置信度随时间按半衰期衰减的纯函数。
// 设计思路: 全部类型为 POD 风格的小结构,几何判定以成员函数内聚在类型上;
//           哈希函数对象单独定义,便于直接放入 unordered 容器。
// 架构角色: 全工程最底层模块,被 index / store / history / service / app / tests
//           共同依赖;自身不依赖任何其他 stmb 模块,保证依赖图无环。
// ============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace stmb {

// 时间戳:64 位有符号整型,单位毫秒
using TimeStamp = std::int64_t;

// 记忆块唯一标识:64 位无符号整型,由服务层自增生成
using BlockId = std::uint64_t;

// 观测来源标识(多终端/传感器)
using SourceId = std::uint64_t;

// ----------------------------------------------------------------------------
// 变化分类:观测处理管线对一次观测的归类结果
//   Emergence : 当前为空(或无块)、观测非空 —— 新事物出现;
//   Vanishing : 当前非空、观测为空 —— 消亡;
//   Mutation  : 双方非空但语义不同 —— 属性变更;
//   Seasonal  : 命中已注册周期模式的当前相位 —— 不触发变更;
//   Transient : 窗口内出现又自行恢复的变化 / 观测与现状一致 —— 不产生版本;
//   Conflict  : 观测与多数来源矛盾且未达阈值 —— 挂起仲裁。
// ----------------------------------------------------------------------------
enum class ChangeType {
    Emergence,
    Vanishing,
    Mutation,
    Seasonal,
    Transient,
    Conflict,
};

// 变化类型枚举转字符串(日志/演示输出用)
const char* toString(ChangeType type);

// ----------------------------------------------------------------------------
// 观测:某来源在某时刻对某块的语义观测(payload 空串 = 观测为空地/消失)
// ----------------------------------------------------------------------------
struct Observation {
    std::string payload;        // 观测语义;空串 = 空地/消失
    double      confidence = 1.0;  // 观测置信度
    SourceId    source = 0;     // 观测来源
    TimeStamp   t = 0;          // 观测时刻
};

// ----------------------------------------------------------------------------
// 周期模式:块的语义随时间周期变化(如昼夜/季节),用于 Seasonal 判定
//   相位 = ((t % periodMs) + periodMs) % periodMs,落入某 Phase 的
//   [offsetMs, offsetMs+durationMs) 区间即取该 Phase 的 payload 为预测值
// ----------------------------------------------------------------------------
struct PeriodicPattern {
    struct Phase {
        std::int64_t offsetMs = 0;    // 相位起点(含)
        std::int64_t durationMs = 0;  // 相位时长
        std::string  payload;         // 该相位的语义
    };
    std::int64_t periodMs = 0;   // 周期长度(毫秒)
    std::vector<Phase> phases;   // 相位列表
};

// ----------------------------------------------------------------------------
// 时间范围:闭区间 [start, end]
// ----------------------------------------------------------------------------
struct TimeRange {
    TimeStamp start = 0;  // 起始时间(含)
    TimeStamp end   = 0;  // 结束时间(含)

    // 判断时间点 t 是否落在闭区间内
    bool contains(TimeStamp t) const;
    // 判断两个时间范围是否存在交集
    bool overlaps(const TimeRange& other) const;
};

// ----------------------------------------------------------------------------
// 轴对齐包围盒:三维空间区域,min/max 各为 (x, y, z)
// ----------------------------------------------------------------------------
struct AABB {
    std::array<double, 3> min{0.0, 0.0, 0.0};  // 各轴最小值
    std::array<double, 3> max{0.0, 0.0, 0.0};  // 各轴最大值

    // 判断点 p 是否在盒内(含边界)
    bool contains(const std::array<double, 3>& p) const;
    // 判断两个 AABB 是否相交(含边界相接)
    bool intersects(const AABB& other) const;
    // 返回盒中心点坐标
    std::array<double, 3> center() const;
};

// ----------------------------------------------------------------------------
// 空间格子坐标:空间索引(SpatialGridIndex)的键
// ----------------------------------------------------------------------------
struct CellCoord {
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;

    bool operator==(const CellCoord& other) const;
};

// CellCoord 的哈希函数对象
struct CellCoordHash {
    std::size_t operator()(const CellCoord& c) const;
};

// ----------------------------------------------------------------------------
// 记忆块逻辑键:由 AABB 中心点所在格子 + 时间戳所在时间槽组成
// ----------------------------------------------------------------------------
struct BlockKey {
    std::int64_t cellX    = 0;  // 中心点所在格子 X
    std::int64_t cellY    = 0;  // 中心点所在格子 Y
    std::int64_t cellZ    = 0;  // 中心点所在格子 Z
    std::int64_t timeSlot = 0;  // 时间戳所在时间槽

    bool operator==(const BlockKey& other) const;
};

// BlockKey 的哈希函数对象
struct BlockKeyHash {
    std::size_t operator()(const BlockKey& k) const;
};

// ----------------------------------------------------------------------------
// 分片键:空间分片坐标(块中心 / shardCellSize)+ 时间桶 + 层级(LOD)维度
// 用于分片持久化:每个 ShardKey 对应一个独立的分片文件;
// level 维度让不同层级的块落入不同分片文件,粗/细层级互不干扰
// ----------------------------------------------------------------------------
struct ShardKey {
    std::int64_t sx = 0;      // 空间分片 X
    std::int64_t sy = 0;      // 空间分片 Y
    std::int64_t sz = 0;      // 空间分片 Z
    std::int64_t tBucket = 0; // 时间桶编号
    std::int64_t level = 0;   // LOD 层级(单尺度模式恒为 0)

    bool operator==(const ShardKey& other) const;
};

// ShardKey 的哈希函数对象
struct ShardKeyHash {
    std::size_t operator()(const ShardKey& k) const;
};

// ----------------------------------------------------------------------------
// 块状态机:Pending(待确认)-> Stable(稳定)-> Changing(变更中)-> Stable(新稳定态)
//   Pending  : 新观测写入,等待独立确认;迟迟得不到确认可由 expire/remove 消亡;
//   Stable   : 确认数达到阈值,当前数据为可信稳定态;
//   Changing : 稳定块收到矛盾观测,候选变更暂存,当前生效数据不变,待确认。
// ----------------------------------------------------------------------------
enum class BlockState {
    Pending,
    Stable,
    Changing,
};

// 状态枚举转字符串(日志/演示输出用)
const char* toString(BlockState state);

// ----------------------------------------------------------------------------
// 记忆块本体:索引中只存 BlockId,块数据只在 BlockStore 中存一份
// ----------------------------------------------------------------------------
struct MemoryBlock {
    BlockId     id = 0;         // 唯一标识(服务层自增生成)
    BlockKey    key;            // 逻辑键(格子 + 时间槽)
    AABB        region;         // 绑定的三维空间区域
    std::string payload;        // 负载数据(当前生效)
    TimeStamp   timestamp  = 0; // 绑定的时间点
    std::uint32_t version  = 0; // 版本号(写入时置 1,确认变更后 +1)
    TimeStamp   lastAccess = 0; // 最近访问时间(信息性字段,LRU 由 store 内部链表维护)

    BlockState  state = BlockState::Pending; // 状态机当前状态(新块默认待确认)
    double      confidence = 1.0;            // 基准置信度(0~1,初始观测置信度;不被衰减改写)
    double      confirmations = 0.0;         // 加权确认计数(按来源可靠性累加,v5 起为 double)
    TimeStamp   lastUpdate = 0;              // 最近一次状态/数据更新的时间(衰减起算点)

    // 候选变更(仅 Changing 状态有效):确认后替换当前生效数据
    std::optional<std::string> pendingPayload;
    std::optional<double>      pendingConfidence;

    // ---- 观测管线字段(v5)----
    // 观察窗口(仅候选挂起时有效):挂起时刻(-1 = 无挂起)、累计权重、来源集合
    TimeStamp windowStart = -1;
    double    windowWeight = 0.0;
    std::unordered_set<SourceId> windowSources;
    // 已注册的周期模式(Seasonal 判定用)
    std::optional<PeriodicPattern> pattern;
    // 空间一致性标记:命中校验规则(悬空/垂直冲突)时置位,仅标记不拒绝
    bool suspect = false;

    // ---- LOD(多尺度金字塔)字段 ----
    int  level = -1;           // 所属层级(-1 = 写入时自动判定;0 = 最粗)
    bool isSummary = false;    // 是否为聚合摘要块(buildSummaries 上卷生成)
    int  sourceLevel = -1;     // 摘要块的来源层级(普通块为 -1)
    bool hasFinerData = false; // 查询时填充:该区域是否存在更细层级数据(不落盘)

    // 查询时填充:与块区域相交的活跃动态实例 id(临时占用标记,不落盘)
    std::vector<std::uint64_t> temporarilyOccupiedBy;
};

// ----------------------------------------------------------------------------
// 工具函数:由空间区域的中心点与时间戳计算 BlockKey
//   cellSize   : 空间格子边长(与 SpatialGridIndex 保持一致)
//   timeSlotMs : 时间槽宽度,单位毫秒
// ----------------------------------------------------------------------------
BlockKey makeBlockKey(const AABB& region, TimeStamp timestamp,
                      double cellSize, TimeStamp timeSlotMs);

// ----------------------------------------------------------------------------
// 工具函数:由空间区域中心点与时间戳计算所属分片键
//   shardCellSize   : 空间分片边长(cellSize 的整数倍)
//   shardTimeSpanMs : 时间桶跨度(毫秒)
//   level           : LOD 层级(单尺度模式传 0)
// ----------------------------------------------------------------------------
ShardKey makeShardKey(const AABB& region, TimeStamp timestamp,
                      double shardCellSize, TimeStamp shardTimeSpanMs,
                      int level = 0);

// ----------------------------------------------------------------------------
// 工具函数:置信度随时间按半衰期衰减
//   base       : 基准置信度(存储值,不被改写)
//   lastUpdate : 最近一次更新时间(衰减起算点)
//   now        : 当前时间
//   halfLifeMs : 半衰期(毫秒);<= 0 表示不衰减
// 返回:base * 0.5^((now-lastUpdate)/halfLifeMs);elapsed<=0 或 halfLifeMs<=0 时返回 base
// ----------------------------------------------------------------------------
double decayedConfidence(double base, TimeStamp lastUpdate, TimeStamp now,
                         std::int64_t halfLifeMs);

}  // namespace stmb
