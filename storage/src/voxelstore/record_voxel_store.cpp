#include "voxelstore/record_voxel_store.h"

namespace voxelstore {
namespace {

using namespace storage;

TableSchema kBlocks{
    "voxel_blocks",
    {{"id", FieldType::kInt},
     {"cell_x", FieldType::kInt}, {"cell_y", FieldType::kInt},
     {"cell_z", FieldType::kInt}, {"time_slot", FieldType::kInt},
     {"min_x", FieldType::kReal}, {"min_y", FieldType::kReal}, {"min_z", FieldType::kReal},
     {"max_x", FieldType::kReal}, {"max_y", FieldType::kReal}, {"max_z", FieldType::kReal},
     {"payload", FieldType::kText}, {"timestamp", FieldType::kInt},
     {"version", FieldType::kInt}, {"last_access", FieldType::kInt},
     {"state", FieldType::kText}, {"confidence", FieldType::kReal},
     {"confirmations", FieldType::kReal}, {"last_update", FieldType::kInt},
     {"pending_payload", FieldType::kText}, {"pending_confidence", FieldType::kReal},
     {"window_start", FieldType::kInt}, {"window_weight", FieldType::kReal},
     {"window_sources", FieldType::kText}, {"pattern", FieldType::kText},
     {"suspect", FieldType::kInt}, {"level", FieldType::kInt},
     {"is_summary", FieldType::kInt}, {"source_level", FieldType::kInt}},
    "id", false,
    {{"timestamp"}, {"level"}, {"cell_x", "cell_y", "cell_z"}},
};

TableSchema kVersions{
    "voxel_versions",
    {{"seq", FieldType::kInt}, {"block_id", FieldType::kInt},
     {"version", FieldType::kInt}, {"payload", FieldType::kText},
     {"confidence", FieldType::kReal}, {"state", FieldType::kText},
     {"valid_from", FieldType::kInt}, {"valid_to", FieldType::kInt}},
    "seq", true,
    {{"block_id"}},
};

TableSchema kInstances{
    "voxel_instances",
    {{"id", FieldType::kInt}, {"class_label", FieldType::kText},
     {"min_x", FieldType::kReal}, {"min_y", FieldType::kReal}, {"min_z", FieldType::kReal},
     {"max_x", FieldType::kReal}, {"max_y", FieldType::kReal}, {"max_z", FieldType::kReal},
     {"latest_t", FieldType::kInt},
     {"latest_px", FieldType::kReal}, {"latest_py", FieldType::kReal},
     {"latest_pz", FieldType::kReal}, {"latest_vx", FieldType::kReal},
     {"latest_vy", FieldType::kReal}, {"latest_vz", FieldType::kReal},
     {"trajectory", FieldType::kText}, {"sources", FieldType::kText},
     {"confidence", FieldType::kReal}, {"state", FieldType::kText},
     {"last_seen", FieldType::kInt}, {"stationary_since", FieldType::kInt}},
    "id", false,
    {{"state"}, {"last_seen"}},
};

TableSchema kMeta{
    "voxel_meta",
    {{"key", FieldType::kText}, {"value", FieldType::kInt}},
    "key", false, {},
};

// ---- Record <-> 领域结构 ----

Record to_record(const VoxelBlock& b) {
    return {
        {"id", vint((int64_t)b.id)},
        {"cell_x", vint(b.cell_x)}, {"cell_y", vint(b.cell_y)},
        {"cell_z", vint(b.cell_z)}, {"time_slot", vint(b.time_slot)},
        {"min_x", vreal(b.region.min_x)}, {"min_y", vreal(b.region.min_y)},
        {"min_z", vreal(b.region.min_z)}, {"max_x", vreal(b.region.max_x)},
        {"max_y", vreal(b.region.max_y)}, {"max_z", vreal(b.region.max_z)},
        {"payload", vtext(b.payload)}, {"timestamp", vint(b.timestamp)},
        {"version", vint(b.version)}, {"last_access", vint(b.last_access)},
        {"state", vtext(b.state)}, {"confidence", vreal(b.confidence)},
        {"confirmations", vreal(b.confirmations)}, {"last_update", vint(b.last_update)},
        {"pending_payload", vopt(b.pending_payload)},
        {"pending_confidence", vopt(b.pending_confidence)},
        {"window_start", vint(b.window_start)}, {"window_weight", vreal(b.window_weight)},
        {"window_sources", vtext(json(b.window_sources).dump())},
        {"pattern", b.pattern ? vtext(json(*b.pattern).dump()) : vnull()},
        {"suspect", vint(b.suspect ? 1 : 0)}, {"level", vint(b.level)},
        {"is_summary", vint(b.is_summary ? 1 : 0)}, {"source_level", vint(b.source_level)},
    };
}

VoxelBlock block_from(const Record& r) {
    VoxelBlock b;
    b.id = (uint64_t)as_int(r.at("id"));
    b.cell_x = as_int(r.at("cell_x"));
    b.cell_y = as_int(r.at("cell_y"));
    b.cell_z = as_int(r.at("cell_z"));
    b.time_slot = as_int(r.at("time_slot"));
    b.region = Aabb{as_real(r.at("min_x")), as_real(r.at("min_y")), as_real(r.at("min_z")),
                    as_real(r.at("max_x")), as_real(r.at("max_y")), as_real(r.at("max_z"))};
    b.payload = as_text(r.at("payload"));
    b.timestamp = as_int(r.at("timestamp"));
    b.version = (uint32_t)as_int(r.at("version"));
    b.last_access = as_int(r.at("last_access"));
    b.state = as_text(r.at("state"));
    b.confidence = as_real(r.at("confidence"));
    b.confirmations = as_real(r.at("confirmations"));
    b.last_update = as_int(r.at("last_update"));
    if (!is_null(r.at("pending_payload"))) b.pending_payload = as_text(r.at("pending_payload"));
    if (!is_null(r.at("pending_confidence")))
        b.pending_confidence = as_real(r.at("pending_confidence"));
    b.window_start = as_int(r.at("window_start"));
    b.window_weight = as_real(r.at("window_weight"));
    b.window_sources = json::parse(as_text(r.at("window_sources"))).get<std::vector<uint64_t>>();
    if (!is_null(r.at("pattern")))
        b.pattern = json::parse(as_text(r.at("pattern"))).get<PeriodicPattern>();
    b.suspect = as_int(r.at("suspect")) != 0;
    b.level = (int)as_int(r.at("level"));
    b.is_summary = as_int(r.at("is_summary")) != 0;
    b.source_level = (int)as_int(r.at("source_level"));
    return b;
}

Record to_record(const VoxelVersion& v) {
    return {{"block_id", vint((int64_t)v.block_id)}, {"version", vint(v.version)},
            {"payload", vtext(v.payload)}, {"confidence", vreal(v.confidence)},
            {"state", vtext(v.state)}, {"valid_from", vint(v.valid_from)},
            {"valid_to", vopt(v.valid_to)}};
}

VoxelVersion version_from(const Record& r) {
    VoxelVersion v;
    v.block_id = (uint64_t)as_int(r.at("block_id"));
    v.version = (uint32_t)as_int(r.at("version"));
    v.payload = as_text(r.at("payload"));
    v.confidence = as_real(r.at("confidence"));
    v.state = as_text(r.at("state"));
    v.valid_from = as_int(r.at("valid_from"));
    if (!is_null(r.at("valid_to"))) v.valid_to = as_int(r.at("valid_to"));
    return v;
}

Record to_record(const VoxelInstance& i) {
    return {
        {"id", vint((int64_t)i.id)}, {"class_label", vtext(i.class_label)},
        {"min_x", vreal(i.bounds.min_x)}, {"min_y", vreal(i.bounds.min_y)},
        {"min_z", vreal(i.bounds.min_z)}, {"max_x", vreal(i.bounds.max_x)},
        {"max_y", vreal(i.bounds.max_y)}, {"max_z", vreal(i.bounds.max_z)},
        {"latest_t", vint(i.latest.t)},
        {"latest_px", vreal(i.latest.px)}, {"latest_py", vreal(i.latest.py)},
        {"latest_pz", vreal(i.latest.pz)}, {"latest_vx", vreal(i.latest.vx)},
        {"latest_vy", vreal(i.latest.vy)}, {"latest_vz", vreal(i.latest.vz)},
        {"trajectory", vtext(json(i.trajectory).dump())},
        {"sources", vtext(json(i.sources).dump())},
        {"confidence", vreal(i.confidence)}, {"state", vtext(i.state)},
        {"last_seen", vint(i.last_seen)}, {"stationary_since", vint(i.stationary_since)},
    };
}

VoxelInstance instance_from(const Record& r) {
    VoxelInstance i;
    i.id = (uint64_t)as_int(r.at("id"));
    i.class_label = as_text(r.at("class_label"));
    i.bounds = Aabb{as_real(r.at("min_x")), as_real(r.at("min_y")), as_real(r.at("min_z")),
                    as_real(r.at("max_x")), as_real(r.at("max_y")), as_real(r.at("max_z"))};
    i.latest = TrackPoint{as_int(r.at("latest_t")),
                          as_real(r.at("latest_px")), as_real(r.at("latest_py")),
                          as_real(r.at("latest_pz")), as_real(r.at("latest_vx")),
                          as_real(r.at("latest_vy")), as_real(r.at("latest_vz"))};
    i.trajectory = json::parse(as_text(r.at("trajectory"))).get<std::vector<TrackPoint>>();
    i.sources = json::parse(as_text(r.at("sources"))).get<std::vector<uint64_t>>();
    i.confidence = as_real(r.at("confidence"));
    i.state = as_text(r.at("state"));
    i.last_seen = as_int(r.at("last_seen"));
    i.stationary_since = as_int(r.at("stationary_since"));
    return i;
}

} // namespace

RecordVoxelStore::RecordVoxelStore(std::unique_ptr<storage::RecordBackend> backend)
    : backend_(std::move(backend)) {
    backend_->create_table(kBlocks);
    backend_->create_table(kVersions);
    backend_->create_table(kInstances);
    backend_->create_table(kMeta);
}

// ---- 块：可变当前态 ----

void RecordVoxelStore::put_block(const VoxelBlock& b) {
    backend_->put(kBlocks.name, to_record(b));
}

std::optional<VoxelBlock> RecordVoxelStore::get_block(uint64_t id) {
    auto r = backend_->get(kBlocks.name, vint((int64_t)id));
    if (!r) return std::nullopt;
    return block_from(*r);
}

bool RecordVoxelStore::remove_block(uint64_t id) {
    return backend_->remove(kBlocks.name, vint((int64_t)id));
}

std::vector<VoxelBlock> RecordVoxelStore::all_blocks() {
    std::vector<VoxelBlock> out;
    for (const auto& r : backend_->query(kBlocks.name, {}, {{"id", false}}))
        out.push_back(block_from(r));
    return out;
}

std::vector<VoxelBlock> RecordVoxelStore::query_blocks(std::optional<Aabb> region,
                                                       std::optional<int64_t> time_from,
                                                       std::optional<int64_t> time_to,
                                                       std::optional<int> level) {
    std::vector<storage::Condition> conds;
    if (region) {
        // AABB 相交：block.min <= query.max 且 block.max >= query.min（逐轴）
        conds.push_back({"min_x", Op::Le, vreal(region->max_x)});
        conds.push_back({"max_x", Op::Ge, vreal(region->min_x)});
        conds.push_back({"min_y", Op::Le, vreal(region->max_y)});
        conds.push_back({"max_y", Op::Ge, vreal(region->min_y)});
        conds.push_back({"min_z", Op::Le, vreal(region->max_z)});
        conds.push_back({"max_z", Op::Ge, vreal(region->min_z)});
    }
    if (time_from) conds.push_back({"timestamp", Op::Ge, vint(*time_from)});
    if (time_to) conds.push_back({"timestamp", Op::Le, vint(*time_to)});
    if (level) conds.push_back({"level", Op::Eq, vint(*level)});
    std::vector<VoxelBlock> out;
    for (const auto& r : backend_->query(kBlocks.name, conds, {{"id", false}}))
        out.push_back(block_from(r));
    return out;
}

// ---- 版本历史 ----

void RecordVoxelStore::append_version(const VoxelVersion& v) {
    backend_->append(kVersions.name, to_record(v));
}

void RecordVoxelStore::seal_version(uint64_t block_id, int64_t valid_to) {
    // 关闭当前生效版本（valid_to 为空的行填上 valid_to）；只此一种"修改"
    backend_->update_where(kVersions.name,
                           {{"block_id", Op::Eq, vint((int64_t)block_id)},
                            {"valid_to", Op::IsNull, vnull()}},
                           {{"valid_to", vint(valid_to)}});
}

std::vector<VoxelVersion> RecordVoxelStore::versions_of(uint64_t block_id) {
    std::vector<VoxelVersion> out;
    for (const auto& r : backend_->query(kVersions.name,
                                         {{"block_id", Op::Eq, vint((int64_t)block_id)}},
                                         {{"version", false}}))
        out.push_back(version_from(r));
    return out;
}

// ---- 动态实例 ----

void RecordVoxelStore::put_instance(const VoxelInstance& i) {
    backend_->put(kInstances.name, to_record(i));
}

std::optional<VoxelInstance> RecordVoxelStore::get_instance(uint64_t id) {
    auto r = backend_->get(kInstances.name, vint((int64_t)id));
    if (!r) return std::nullopt;
    return instance_from(*r);
}

std::vector<VoxelInstance> RecordVoxelStore::all_instances() {
    std::vector<VoxelInstance> out;
    for (const auto& r : backend_->query(kInstances.name, {}, {{"id", false}}))
        out.push_back(instance_from(r));
    return out;
}

// ---- meta ----

void RecordVoxelStore::set_meta(const std::string& key, int64_t value) {
    backend_->put(kMeta.name, {{"key", vtext(key)}, {"value", vint(value)}});
}

int64_t RecordVoxelStore::get_meta(const std::string& key, int64_t default_value) {
    auto r = backend_->get(kMeta.name, vtext(key));
    if (!r) return default_value;
    return as_int(r->at("value"));
}

} // namespace voxelstore
