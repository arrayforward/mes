#pragma once

#include <string>

#include "eventstore/record_event_store.h"
#include "storage/backends/redis_backend.h"

namespace eventstore {

/// Redis 后端的事件树存储：RecordEventStore + storage::RedisBackend。
/// addr 形式 "host:port"；key_prefix 用于多租户/测试隔离。
class RedisStore : public RecordEventStore {
public:
    explicit RedisStore(const std::string& addr, std::string key_prefix = "ust")
        : RecordEventStore(
              std::make_unique<storage::RedisBackend>(addr, std::move(key_prefix))) {}
};

} // namespace eventstore
