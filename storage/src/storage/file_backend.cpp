#include "storage/backends/file_backend.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "storage/record_codec.h"

namespace storage {

FileBackend::FileBackend(const std::string& root) : root_(root) {
    std::filesystem::create_directories(root_);
    load();
}

std::string FileBackend::table_dir(const std::string& table) const {
    return root_ + "/" + table;
}

void FileBackend::wal_log(const std::string& table, const json& op) {
    std::ofstream out(table_dir(table) + "/wal.jsonl", std::ios::app);
    if (!out) throw std::runtime_error("file backend: cannot open wal for " + table);
    out << op.dump() << "\n";
    out.flush();
    if (!out) throw std::runtime_error("file backend: wal write failed for " + table);
}

void FileBackend::load() {
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(root_)) {
        if (!entry.is_directory()) continue;
        std::string dir = entry.path().string();
        std::string schema_file = dir + "/schema.json";
        if (!fs::exists(schema_file)) continue;
        std::ifstream in(schema_file);
        TableSchema schema = schema_from_json(json::parse(in));
        schemas_.emplace(schema.name, schema);
        mem_.create_table(schema);
        // 回放 WAL：put = 原样插入；remove = 按主键删除
        std::string wal_file = dir + "/wal.jsonl";
        if (!fs::exists(wal_file)) continue;
        std::ifstream wal(wal_file);
        std::string line;
        while (std::getline(wal, line)) {
            if (line.empty()) continue;
            json op = json::parse(line);
            const std::string& type = op.at("op").get_ref<const std::string&>();
            if (type == "put") {
                mem_.insert_raw(schema.name, record_from_json(op.at("rec")));
            } else if (type == "remove") {
                mem_.remove(schema.name, value_from_json(op.at("pk")));
            }
        }
    }
}

void FileBackend::create_table(const TableSchema& schema) {
    if (schemas_.count(schema.name)) return;  // 幂等
    schemas_.emplace(schema.name, schema);
    mem_.create_table(schema);
    std::filesystem::create_directories(table_dir(schema.name));
    std::ofstream out(table_dir(schema.name) + "/schema.json");
    if (!out) throw std::runtime_error("file backend: cannot write schema for " + schema.name);
    out << schema_to_json(schema).dump(2) << "\n";
}

void FileBackend::put(const std::string& table, const Record& rec) {
    mem_.put(table, rec);
    wal_log(table, json{{"op", "put"}, {"rec", record_to_json(rec)}});
}

int64_t FileBackend::append(const std::string& table, Record rec) {
    int64_t seq = mem_.append(table, rec);
    rec[schemas_.at(table).pk] = vint(seq);  // 日志落最终整行（含分配的 seq）
    wal_log(table, json{{"op", "put"}, {"rec", record_to_json(rec)}});
    return seq;
}

std::optional<Record> FileBackend::get(const std::string& table, const Value& pk) {
    return mem_.get(table, pk);
}

bool FileBackend::remove(const std::string& table, const Value& pk) {
    if (!mem_.remove(table, pk)) return false;
    wal_log(table, json{{"op", "remove"}, {"pk", value_to_json(pk)}});
    return true;
}

std::vector<Record> FileBackend::query(const std::string& table,
                                       std::vector<Condition> conds,
                                       std::vector<Ordering> order,
                                       int limit) {
    return mem_.query(table, std::move(conds), std::move(order), limit);
}

void FileBackend::update_where(const std::string& table,
                               std::vector<Condition> conds,
                               Record patch) {
    // 先查出命中行，整行套用 patch 后按 put 形式落日志（回放无需条件求值）
    auto rows = mem_.query(table, conds, {}, 0);
    for (auto& rec : rows) {
        for (const auto& [f, v] : patch) rec[f] = v;
        mem_.insert_raw(table, rec);
        wal_log(table, json{{"op", "put"}, {"rec", record_to_json(rec)}});
    }
}

} // namespace storage
