#pragma once

#include "entitytree/record_entity_store.h"
#include "storage/backends/memory_backend.h"

namespace entitytree {

/// 内存后端的实体搜索树存储：统一层 RecordEntityStore + storage::MemoryBackend。
class MemoryEntityStore : public RecordEntityStore {
public:
    MemoryEntityStore() : RecordEntityStore(std::make_unique<storage::MemoryBackend>()) {}
};

} // namespace entitytree
