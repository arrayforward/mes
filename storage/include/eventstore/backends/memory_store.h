#pragma once

#include "eventstore/record_event_store.h"
#include "storage/backends/memory_backend.h"

namespace eventstore {

/// 内存后端的事件树存储：统一层 RecordEventStore + storage::MemoryBackend。
class MemoryStore : public RecordEventStore {
public:
    MemoryStore() : RecordEventStore(std::make_unique<storage::MemoryBackend>()) {}
};

} // namespace eventstore
