#pragma once

#include <string>

#include "entitytree/record_entity_store.h"
#include "storage/backends/file_backend.h"

namespace entitytree {

/// 文件目录后端的实体搜索树存储：RecordEntityStore + storage::FileBackend。
/// root 为数据目录（每表一个子目录：schema.json + wal.jsonl）。
class FileEntityStore : public RecordEntityStore {
public:
    explicit FileEntityStore(const std::string& root)
        : RecordEntityStore(std::make_unique<storage::FileBackend>(root)) {}
};

} // namespace entitytree
