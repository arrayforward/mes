#pragma once

#include <string>

#include "eventstore/record_event_store.h"
#include "storage/backends/file_backend.h"

namespace eventstore {

/// 文件目录后端的事件树存储：RecordEventStore + storage::FileBackend。
/// root 为数据目录（每表一个子目录：schema.json + wal.jsonl）。
class FileStore : public RecordEventStore {
public:
    explicit FileStore(const std::string& root)
        : RecordEventStore(std::make_unique<storage::FileBackend>(root)) {}
};

} // namespace eventstore
