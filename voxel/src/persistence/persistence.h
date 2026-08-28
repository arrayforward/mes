// ============================================================================
// 文件: persistence.h
// 模块: stmb_persistence(本地文件持久化,依赖 core)
// 用途: 声明 PersistenceManager —— 快照(snapshot)+ 预写日志(WAL)的
//       本地文件持久化方案,不接外部数据库、不引第三方依赖。
// 存储布局(数据目录 dataDir 下):
//   - snapshot.stmb : 全量快照(store 内全部 MemoryBlock + VersionLog 全部
//                     版本记录 + nextId),写入用「临时文件 + rename」保证原子性;
//   - wal.log       : 追加式操作日志,记录快照之后发生的每个变更操作。
// 二进制格式(自实现,小端序):
//   - 文件头:magic u32('STMB')+ formatVersion u32;
//   - 记录帧:{type u8, payloadLen u32, payload bytes..., crc32 u32},
//     crc 覆盖 type+len+payload;CRC32 为标准多项式表驱动自实现;
//   - 字符串:len u32 + bytes;double 按位写;optional 用 1 字节存在标志。
// 容错规则:
//   - magic / formatVersion 错误:判定文件损坏,返回错误(optional 空);
//   - CRC 不匹配或截断的尾部记录:丢弃该记录及其后内容,保留已验证前缀
//     (模拟 crash 写一半的场景);
//   - 文件不存在:返回空数据而非错误(首次启动)。
// 架构角色: 与 store / history 平级的独立部件,由 StmbService 持有调用;
//           自身不感知服务逻辑,只做字节流的编码、落盘与解析。
// ============================================================================
#pragma once

#include "types.h"
#include "dynamic_layer.h"
#include "version_log.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace stmb {

// ----------------------------------------------------------------------------
// 记录类型:WAL 操作记录 + 快照文件内部记录
// ----------------------------------------------------------------------------
enum class RecordType : std::uint8_t {
    // WAL 操作记录
    PutBlock     = 1,   // 完整 MemoryBlock 序列化
    Confirm      = 2,   // {id, now}(Pending 确认 / Changing 确认变更重放同一路径)
    ReportChange = 3,   // {id, newPayload, newConfidence, now}
    Remove       = 4,   // {id}
    ExpireBefore = 5,   // {ts}
    Observe      = 6,   // {id, Observation(payload/confidence/source/t)}(v5 观测管线)
    RegisterSource = 7, // {source, reliability}(v6 来源注册/权重变更)
    // 快照/分片文件内部记录
    SnapBlock    = 16,  // 完整 MemoryBlock 序列化
    SnapVersion  = 17,  // {blockId, BlockVersion 序列化}
    SnapshotEnd  = 18,  // {nextId}(结束标记,携带 id 发生器状态;分片文件中为空载荷)
    // manifest 文件记录
    ManifestMeta  = 19, // 全局参数 + nextId + maxBlockRadius
    ManifestShard = 20, // 单个分片的清单条目(ShardKey + 块 id 列表 + 版本数)
    // v4 新增:动态实例(快照 / dynamic.stmb)
    SnapInstance = 21,  // 完整 DynamicInstance 序列化(含轨迹)
    // v6 新增:来源注册表(快照 / manifest 内的来源小节)
    SnapSource   = 22,  // {source, reliability}
};

// ----------------------------------------------------------------------------
// WAL 记录:解析后的操作(按类型使用对应字段)
// ----------------------------------------------------------------------------
struct WalRecord {
    RecordType  type = RecordType::PutBlock;
    MemoryBlock block;            // PutBlock:完整块数据
    BlockId     id = 0;           // Confirm / ReportChange / Remove / Observe:目标块 id
    std::string payload;          // ReportChange:新负载 / Observe:观测负载
    double      confidence = 0.0; // ReportChange:新置信度 / Observe:观测置信度
    TimeStamp   ts = 0;           // Confirm:now / ReportChange:now / ExpireBefore:阈值 / Observe:观测时刻
    double      weight = 1.0;     // Confirm:确认权重(v5,重放时精确还原加权计数)
    SourceId    source = 0;       // Observe:观测来源(v5)
    bool        hasShardKey = false; // 分片模式下为 true,shard 字段有效
    ShardKey    shard;            // 该操作触达的分片(重放时按需加载)
};

// ----------------------------------------------------------------------------
// 快照数据:loadSnapshot 的返回载体
// ----------------------------------------------------------------------------
struct SnapshotData {
    std::vector<MemoryBlock> blocks;                          // store 内全部块
    std::vector<std::pair<BlockId, BlockVersion>> versions;   // VersionLog 全部版本记录
    BlockId nextId = 1;                                       // id 发生器状态
    std::vector<DynamicInstance> instances;                   // 动态层全部实例(v4)
    InstanceId instanceNextId = 1;                            // 实例 id 发生器状态
    std::vector<std::pair<SourceId, double>> sources;         // 来源注册表(v6)
};

// ----------------------------------------------------------------------------
// 分片数据:单个分片文件的内容(自包含)
// ----------------------------------------------------------------------------
struct ShardData {
    std::vector<MemoryBlock> blocks;                          // 该分片全部块
    std::vector<std::pair<BlockId, BlockVersion>> versions;   // 该分片相关版本记录
};

// ----------------------------------------------------------------------------
// manifest 清单:全局参数 + nextId + 已存在分片列表
// ----------------------------------------------------------------------------
struct ManifestShardEntry {
    ShardKey              key;           // 分片键
    std::vector<BlockId>  blockIds;      // 该分片全部块 id(含仅历史留存的)
    std::uint32_t         versionCount = 0;  // 该分片版本记录数
};

struct ManifestData {
    bool          present = false;        // manifest 文件是否存在且解析成功
    std::uint64_t capacity = 0;           // 存储容量上限
    double        cellSize = 1.0;         // 空间格子边长
    TimeStamp     timeSlotMs = 1;         // 时间槽宽度
    std::int64_t  confirmThreshold = 1;   // 确认阈值
    std::int64_t  decayHalfLifeMs = 0;    // 置信度半衰期
    double        shardCellSize = 0.0;    // 空间分片边长
    TimeStamp     shardTimeSpanMs = 0;    // 时间桶跨度
    std::uint64_t maxLoadedShards = 0;    // 内存分片上限
    BlockId       nextId = 1;             // id 发生器状态
    double        maxBlockRadius = 0.0;   // 历史最大块半径(查询扩边用)
    std::uint64_t levelCount = 0;         // LOD 层级数(0/1 = 单尺度)
    std::vector<ManifestShardEntry> shards;  // 已存在分片列表
    std::vector<std::pair<SourceId, double>> sources;  // 来源注册表(v6)
};

class PersistenceManager {
public:
    // 全量快照:原子写入 snapshot.stmb(临时文件 + rename);目录不存在自动创建;
    // instances/instanceNextId 为 v4 动态层内容,sources 为 v6 来源注册表
    // (缺省空,兼容旧调用)
    static bool saveSnapshot(
        const std::string& dir,
        const std::vector<MemoryBlock>& blocks,
        const std::vector<std::pair<BlockId, BlockVersion>>& versions,
        BlockId nextId,
        const std::vector<DynamicInstance>& instances = {},
        InstanceId instanceNextId = 1,
        const std::vector<std::pair<SourceId, double>>& sources = {});

    // 读取快照;文件不存在返回空数据,magic/version 错误或内容损坏返回空 optional
    static std::optional<SnapshotData> loadSnapshot(const std::string& dir);

    // 追加一条 WAL 记录(文件不存在则先写文件头);目录不存在自动创建
    static bool appendWal(const std::string& dir, const WalRecord& record);

    // 重放 WAL:返回解析后的记录列表;
    // 文件不存在返回空列表;magic/version 错误返回空 optional;
    // CRC 错误/截断尾部:保留已验证前缀
    static std::optional<std::vector<WalRecord>> replayWal(const std::string& dir);

    // 检查点:saveSnapshot + 截断 wal.log(只保留文件头)
    static bool checkpoint(
        const std::string& dir,
        const std::vector<MemoryBlock>& blocks,
        const std::vector<std::pair<BlockId, BlockVersion>>& versions,
        BlockId nextId,
        const std::vector<DynamicInstance>& instances = {},
        InstanceId instanceNextId = 1,
        const std::vector<std::pair<SourceId, double>>& sources = {});

    // ---- 分片模式(formatVersion 2)----

    // 分片文件名:"<sx>_<sy>_<sz>_<tBucket>.stmb"(位于 dir/shards/ 下)
    static std::string shardFileName(const ShardKey& key);

    // 写单个分片文件(原子写,临时文件 + rename);目录自动创建
    static bool saveShardFile(const std::string& dir, const ShardKey& key,
                              const ShardData& data);

    // 读单个分片文件;不存在返回空分片数据,损坏返回空 optional
    static std::optional<ShardData> loadShardFile(const std::string& dir,
                                                  const ShardKey& key);

    // 写全局清单 manifest.stmb(原子写)
    static bool saveManifest(const std::string& dir, const ManifestData& manifest);

    // 读全局清单;文件不存在返回 present=false 的默认值,损坏返回空 optional
    static std::optional<ManifestData> loadManifest(const std::string& dir);

    // 只截断 wal.log(写空文件头),分片模式 checkpoint 的最后一步
    static bool truncateWal(const std::string& dir);

    // 分片模式下动态层单独落盘 dynamic.stmb(实例位置随时间漂移,
    // 不归属固定空间分片);原子写;目录自动创建
    static bool saveDynamic(const std::string& dir,
                            const std::vector<DynamicInstance>& instances,
                            InstanceId instanceNextId);

    // 读 dynamic.stmb;不存在返回空数据,损坏返回空 optional
    static std::optional<std::pair<std::vector<DynamicInstance>, InstanceId>>
        loadDynamic(const std::string& dir);
};

}  // namespace stmb
