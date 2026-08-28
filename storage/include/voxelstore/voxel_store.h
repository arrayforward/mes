#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "voxelstore/types.h"

namespace voxelstore {

/// 时空记忆块存储抽象接口。
/// 与事件树的 append-only 不同：块的"当前态"是可变的（状态机/确认计数随观测演化），
/// 版本链才是 append-only 的（seal 只关闭生效区间，不改写历史内容）。
/// 后端映射：数据库（sqlite 先行，mysql/postgres 方言化）/ redis / 文件。
class VoxelStore {
public:
    virtual ~VoxelStore() = default;

    // ---- 块：可变当前态（upsert 语义） ----

    virtual void put_block(const VoxelBlock& b) = 0;
    virtual std::optional<VoxelBlock> get_block(uint64_t id) = 0;
    /// 删除块当前态（版本历史默认保留，可回溯"这里以前是什么"）。返回是否存在。
    virtual bool remove_block(uint64_t id) = 0;
    virtual std::vector<VoxelBlock> all_blocks() = 0;

    /// 时空联合查询：AABB 相交 + 可选时间范围（timestamp 闭区间）+ 可选 LOD 层级。
    /// 三个条件任意组合（std::nullopt = 不限制）。结果按 id 升序。
    virtual std::vector<VoxelBlock> query_blocks(
        std::optional<Aabb> region = {},
        std::optional<int64_t> time_from = {},
        std::optional<int64_t> time_to = {},
        std::optional<int> level = {}) = 0;

    // ---- 版本历史（append-only + seal） ----

    virtual void append_version(const VoxelVersion& v) = 0;
    /// 封存某块当前生效版本：为其填上 valid_to（版本链闭环）。
    virtual void seal_version(uint64_t block_id, int64_t valid_to) = 0;
    /// 某块的全部历史版本，按版本号升序。
    virtual std::vector<VoxelVersion> versions_of(uint64_t block_id) = 0;
    /// 时间回溯：返回时刻 t 生效的版本（valid_from <= t < valid_to）。基类通用实现。
    std::optional<VoxelVersion> version_at(uint64_t block_id, int64_t t);

    // ---- 动态实例 ----

    virtual void put_instance(const VoxelInstance& i) = 0;
    virtual std::optional<VoxelInstance> get_instance(uint64_t id) = 0;
    virtual std::vector<VoxelInstance> all_instances() = 0;

    // ---- id 发生器 / 全局参数 ----

    virtual void set_meta(const std::string& key, int64_t value) = 0;
    virtual int64_t get_meta(const std::string& key, int64_t default_value = 0) = 0;
};

} // namespace voxelstore
