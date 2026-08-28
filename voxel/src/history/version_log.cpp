// ============================================================================
// 文件: version_log.cpp
// 模块: stmb_history(版本历史,依赖 core)
// 用途: 实现 VersionLog 的归档、封存、查询与清理。
// 设计思路:
//   1. archive 简单追加:版本号由服务层保证单调递增,故向量天然按版本升序;
//   2. seal 只作用于版本链末尾(当前生效版本),把空的 validTo 填上;
//   3. getAt 顺序扫描版本链,返回满足 validFrom <= t < validTo 的版本
//      (validTo 为空视为无穷大,即当前生效版本);
//   4. historyOf 返回副本并显式按版本号排序,对外承诺升序而不依赖内部约定。
// 架构角色: VersionLog 的唯一实现文件。
// ============================================================================
#include "version_log.h"

#include <algorithm>

namespace stmb {

// 伪代码:
//   1. 在 entries_ 中找到(不存在则自动创建)该块的版本链;
//   2. 把版本快照追加到链尾。
void VersionLog::archive(BlockId id, const BlockVersion& version) {
    entries_[id].push_back(version);
}

// 伪代码:
//   1. 查找该块的版本链,不存在或为空则直接返回;
//   2. 取链尾版本(当前生效版本),仅当其 validTo 为空时填入封存时间,
//      避免重复封存覆盖第一次的结束时刻。
void VersionLog::seal(BlockId id, TimeStamp validTo) {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.empty()) {
        return;
    }
    BlockVersion& latest = it->second.back();
    if (!latest.validTo.has_value()) {
        latest.validTo = validTo;
    }
}

// 伪代码:
//   1. 查找该块的版本链,不存在则返回空 vector;
//   2. 复制整条链并按版本号升序排序(兜底,正常归档顺序本身即升序);
//   3. 返回排序后的副本。
std::vector<BlockVersion> VersionLog::historyOf(BlockId id) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return {};
    }
    std::vector<BlockVersion> result = it->second;
    std::sort(result.begin(), result.end(),
              [](const BlockVersion& a, const BlockVersion& b) {
                  return a.version < b.version;
              });
    return result;
}

// 伪代码:
//   1. 查找该块的版本链,不存在则返回空;
//   2. 顺序扫描:若 validFrom <= t 且 (validTo 为空或 t < validTo),
//      返回该版本副本;
//   3. 全部不匹配则返回空。
std::optional<BlockVersion> VersionLog::getAt(BlockId id, TimeStamp t) const {
    auto it = entries_.find(id);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    for (const BlockVersion& v : it->second) {
        if (v.validFrom <= t && (!v.validTo.has_value() || t < *v.validTo)) {
            return v;
        }
    }
    return std::nullopt;
}

// 伪代码:
//   1. 直接从 entries_ 中擦除该块的整条版本链(不存在则无操作)。
void VersionLog::drop(BlockId id) {
    entries_.erase(id);
}

// 伪代码:
//   1. 直接返回 entries_ 的大小(有历史记录的块数)。
std::size_t VersionLog::trackedCount() const {
    return entries_.size();
}

// 伪代码:
//   1. 遍历 entries_:对每块依次遍历其版本链,把 (id, 版本) 对追加到结果;
//   2. 返回扁平化列表(同一块内的版本顺序保持链序,块之间顺序不保证)。
std::vector<std::pair<BlockId, BlockVersion>> VersionLog::allVersions() const {
    std::vector<std::pair<BlockId, BlockVersion>> result;
    for (const auto& [id, chain] : entries_) {
        for (const BlockVersion& v : chain) {
            result.emplace_back(id, v);
        }
    }
    return result;
}

}  // namespace stmb
