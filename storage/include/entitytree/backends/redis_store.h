#pragma once

#include <string>

#include "entitytree/record_entity_store.h"
#include "storage/backends/redis_backend.h"

namespace entitytree {

/// Redis 后端的实体搜索树存储：RecordEntityStore + storage::RedisBackend。
/// addr 形式 "host:port"；key_prefix 用于多租户/测试隔离。
class RedisEntityStore : public RecordEntityStore {
public:
    explicit RedisEntityStore(const std::string& addr, std::string key_prefix = "ust")
        : RecordEntityStore(
              std::make_unique<storage::RedisBackend>(addr, std::move(key_prefix))) {}
};

} // namespace entitytree
