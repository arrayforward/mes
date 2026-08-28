// ============================================================================
// 文件: version_log.h
// 模块: stmb_history(版本历史,依赖 core)
// 用途: 声明 BlockVersion(一个历史版本快照)与 VersionLog(按块归档的
//       版本日志),支持「时间回溯」:回答"某个块在过去时刻 t 是什么内容"。
// 设计思路:
//   1. 每个版本携带生效区间 [validFrom, validTo),validTo 为空表示当前生效;
//   2. 变更确认时,服务层先 seal 封存旧版本(填 validTo),再 archive 新版本,
//      形成连续不间断的版本链;
//   3. 块被删除/淘汰/过期时历史默认保留(可回溯"这里以前是什么"),
//      需要释放空间时由调用方显式 drop;
//   4. 内部结构为 unordered_map<BlockId, vector<BlockVersion>>,版本按
//      归档顺序天然升序,historyOf 返回前再排序兜底。
// 架构角色: 与索引/存储平级的独立部件,由 StmbService 持有并维护一致性。
// 与其他模块关系: 依赖 core 的 BlockId / TimeStamp / BlockState;线程安全由
//                 服务层的统一互斥锁保证,本类自身不加锁。
// ============================================================================
#pragma once

#include "types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace stmb {

// ----------------------------------------------------------------------------
// 历史版本快照:某块在 [validFrom, validTo) 区间内生效的数据
// ----------------------------------------------------------------------------
struct BlockVersion {
    std::uint32_t version = 0;             // 版本号(从 1 开始)
    std::string   payload;                 // 该版本生效的负载
    double        confidence = 1.0;        // 该版本生效的基准置信度
    BlockState    state = BlockState::Pending;  // 归档时的状态
    TimeStamp     validFrom = 0;           // 生效起始时间(含)
    std::optional<TimeStamp> validTo;      // 生效结束时间(不含);空 = 当前生效
};

class VersionLog {
public:
    // 归档一个新版本(追加到该块的版本链末尾)
    void archive(BlockId id, const BlockVersion& version);

    // 封存某块的当前生效版本:为其填上 validTo(版本链闭环)
    void seal(BlockId id, TimeStamp validTo);

    // 返回某块的全部历史版本(按版本号升序)
    std::vector<BlockVersion> historyOf(BlockId id) const;

    // 时间回溯:返回时刻 t 生效的版本(validFrom <= t < validTo)
    std::optional<BlockVersion> getAt(BlockId id, TimeStamp t) const;

    // 清除某块的全部历史(手动清理;块消亡时默认保留)
    void drop(BlockId id);

    // 返回当前有历史记录的块数(测试/观测用)
    std::size_t trackedCount() const;

    // 返回全部块的全部版本记录(扁平化为 (id, 版本) 对,持久化快照用)
    std::vector<std::pair<BlockId, BlockVersion>> allVersions() const;

private:
    std::unordered_map<BlockId, std::vector<BlockVersion>> entries_;  // 块 id -> 版本链
};

}  // namespace stmb
