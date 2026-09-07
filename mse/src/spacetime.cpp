// ============================================================================
// mse/spacetime.cpp —— 时空树子系统(车间时空信息管理)
//
// 运行期:stmb::StmbService 做时空记忆块服务(块状态机、版本链 AS OF t);
// 持久化:voxelstore::VoxelStore(可空 = 纯内存)镜像块当前态与版本链。
// 命名锚点层级是 mse 侧薄层:锚点表来自定义层(AnchorEntry),路径段间
// '/' 分隔;锚点块 payload 维护 {"anchor","last_event_id","events":[...]}。
//
// 确定性纪律:时间轴一律用逻辑时钟 event.settle_seq,不读物理时钟;
// 发生时间原文只进块 payload。时空记忆是投影:replay(日志) 可完整重建。
//
// 实现说明:
//   - StmbService 用便捷构造(capacity=4096, cellSize=1.0, timeSlotMs=1),
//     该配置未启用动态层(archiveTimeoutMs=0),reportMoving 返回 0——
//     本体轨迹因此不依赖 stmb 动态实例,而是由锚点块 payload 反查重建
//     (trajectory_of / current_anchor_of),确定性且无需额外成员。
//   - voxelstore 镜像只映射关键字段(id/region/payload/timestamp/version/
//     state/confidence + 版本链 valid_from),其余 stmb 运行期字段(观察
//     窗口、LOD 等)本期不落盘。
// ============================================================================

#include "mse/spacetime.h"

#include <algorithm>
#include <array>
#include <utility>

#include "mse/dictionary.h"
#include "mse/event_log.h"
#include "stmb_service.h"
#include "voxelstore/voxel_store.h"

namespace mse {

namespace {

// 按 '/' 切段(保留空段;"a/b/" → ["a","b",""])
std::vector<std::string> split_anchor(const std::string& path) {
    std::vector<std::string> segs;
    std::string cur;
    for (char ch : path) {
        if (ch == '/') {
            segs.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    segs.push_back(cur);
    return segs;
}

// 宽容解析 json(失败返回 discarded,不抛)
json parse_lenient(const std::string& s) {
    return json::parse(s, nullptr, false);
}

} // namespace

// ----------------------------------------------------------------------------
// 构造 / 析构
// ----------------------------------------------------------------------------

SpacetimeTree::SpacetimeTree(DefinitionLayer& defs, voxelstore::VoxelStore* voxel_store)
    : defs_(defs), store_(voxel_store) {
    svc_ = new stmb::StmbService(4096, 1.0, 1);  // 便捷构造:确认阈值 1,不衰减
}

SpacetimeTree::~SpacetimeTree() {
    delete svc_;
}

// ----------------------------------------------------------------------------
// 精度匹配(纯函数)
// ----------------------------------------------------------------------------

bool SpacetimeTree::anchor_matches(const std::string& query,
                                   const std::string& event_anchor) {
    if (query.empty()) return true;  // 空 query 命中一切
    const auto q = split_anchor(query);
    const auto ev = split_anchor(event_anchor);
    if (q.size() > ev.size()) return false;  // query 段数须 ≤ event 段数
    // 前 n-1 段严格相等
    for (std::size_t i = 0; i + 1 < q.size(); ++i) {
        if (q[i] != ev[i]) return false;
    }
    // 末段:相等,或 query 末段是 event 对应段的字符串前缀
    const std::string& q_last = q.back();
    const std::string& e_last = ev[q.size() - 1];
    if (q_last == e_last) return true;
    return e_last.size() > q_last.size() &&
           e_last.compare(0, q_last.size(), q_last) == 0;
}

bool SpacetimeTree::in_slice(const std::string& slice_anchor,
                             const SpaceRef& event_space) {
    if (slice_anchor.empty()) return true;  // 空切片 = 不限
    switch (event_space.kind) {
        case SpaceRef::Kind::kAnchor:
            return anchor_matches(slice_anchor, event_space.anchor);
        case SpaceRef::Kind::kRawText:
            return false;  // 模糊原文只存档,不参与锚点切片匹配
        case SpaceRef::Kind::kCoord:
            return false;  // 本期不做坐标-锚点换算
    }
    return false;
}

// ----------------------------------------------------------------------------
// 内部辅助:锚点空间区域 / 块定位 / voxelstore 镜像
// ----------------------------------------------------------------------------

namespace {

// 锚点的空间区域:定义层锚点表 meta.aabb(6 元数组)有则用,否则零 AABB
stmb::AABB anchor_region(const DefinitionLayer& defs, const std::string& path) {
    stmb::AABB region;  // 默认零 AABB
    const AnchorEntry* entry = defs.find_anchor(path);
    if (!entry) return region;
    auto it = entry->meta.find("aabb");
    if (it == entry->meta.end() || !it->is_array() || it->size() != 6) return region;
    bool ok = true;
    std::array<double, 6> v{};
    for (std::size_t i = 0; i < 6; ++i) {
        if (!(*it)[i].is_number()) {
            ok = false;
            break;
        }
        v[i] = (*it)[i].get<double>();
    }
    if (!ok) return region;
    region.min = {v[0], v[1], v[2]};
    region.max = {v[3], v[4], v[5]};
    return region;
}

// stmb 块 → voxelstore 块(只镜像关键字段,见文件头说明)
voxelstore::VoxelBlock to_voxel_block(const stmb::MemoryBlock& b) {
    voxelstore::VoxelBlock vb;
    vb.id = b.id;
    vb.cell_x = b.key.cellX;
    vb.cell_y = b.key.cellY;
    vb.cell_z = b.key.cellZ;
    vb.time_slot = b.key.timeSlot;
    vb.region.min_x = b.region.min[0];
    vb.region.min_y = b.region.min[1];
    vb.region.min_z = b.region.min[2];
    vb.region.max_x = b.region.max[0];
    vb.region.max_y = b.region.max[1];
    vb.region.max_z = b.region.max[2];
    vb.payload = b.payload;
    vb.timestamp = b.timestamp;
    vb.version = b.version;
    vb.last_access = b.lastAccess;
    vb.state = stmb::toString(b.state);
    vb.confidence = b.confidence;
    vb.confirmations = b.confirmations;
    vb.last_update = b.lastUpdate;
    return vb;
}

} // namespace

// ----------------------------------------------------------------------------
// 存证:把一条已结算事件写入时空记忆
// ----------------------------------------------------------------------------

void SpacetimeTree::record_event(const Event& e) {
    // 仅锚点形态且非空才进时空树(坐标/模糊原文本期不落块)
    if (e.space.kind != SpaceRef::Kind::kAnchor || e.space.anchor.empty()) return;
    const std::string& anchor = e.space.anchor;
    const stmb::TimeStamp ts = e.settle_seq;  // 逻辑时钟,不读物理时钟

    // 事件摘要条目(进块 payload 的 events 数组,append 不限长——演示规模)
    json entry = {
        {"event_id", e.event_id},
        {"type", e.type},
        {"occur_time", e.occur_time},
        {"targets", json::array()},
    };
    for (const auto& [id, kvs] : e.writes) entry["targets"].push_back(id);

    const stmb::AABB region = anchor_region(defs_, anchor);

    // ---- a) 锚点块 ----
    auto bit = anchor_blocks_.find(anchor);
    if (bit == anchor_blocks_.end()) {
        // 首次:put 新块(payload 含首条事件)
        json payload = {
            {"anchor", anchor},
            {"last_event_id", e.event_id},
            {"events", json::array({entry})},
        };
        stmb::MemoryBlock block;
        block.region = region;
        block.payload = payload.dump();
        block.timestamp = ts;
        block.confidence = 1.0;
        svc_->put(std::move(block));  // 返回的是被淘汰块,新块 id 需反查

        // put 不返回新块 id:按 (区域, 时间槽) 反查并核对锚点名
        // (settle_seq 全局单调 → 该时间槽内只有本块)
        stmb::BlockId new_id = 0;
        for (const auto& hit : svc_->query(region, stmb::TimeRange{ts, ts})) {
            const json p = parse_lenient(hit.payload);
            if (!p.is_discarded() && p.value("anchor", "") == anchor) {
                new_id = hit.id;
                break;
            }
        }
        if (new_id == 0) return;  // 理论不发生(容量内);防御性放弃
        anchor_blocks_[anchor] = new_id;

        // 镜像 voxelstore:块当前态 + 首版本(valid_from = ts)
        if (store_) {
            if (auto cur = svc_->get(new_id)) {
                store_->put_block(to_voxel_block(*cur));
                voxelstore::VoxelVersion ver;
                ver.block_id = new_id;
                ver.version = cur->version;
                ver.payload = cur->payload;
                ver.confidence = cur->confidence;
                ver.state = stmb::toString(cur->state);
                ver.valid_from = ts;
                store_->append_version(ver);
            }
        }
    } else {
        // 已有块:payload events 数组 append,走 状态机+版本链 更新
        const stmb::BlockId id = bit->second;
        auto cur = svc_->get(id);
        if (!cur) {
            // 块被淘汰(LRU,超出演示规模才有):索引摘除后按新块重建,
            // 历史 events 随块丢失——容量内的确定性行为不受影响
            anchor_blocks_.erase(bit);
            record_event(e);
            return;
        }
        json payload = parse_lenient(cur->payload);
        if (payload.is_discarded()) return;  // 块内容损坏,防御性放弃
        payload["events"].push_back(entry);
        payload["last_event_id"] = e.event_id;

        // reportChange 只接受 Stable 块:Pending 先确认到 Stable
        if (cur->state != stmb::BlockState::Stable) svc_->confirm(id, ts);
        svc_->reportChange(id, payload.dump(), 1.0, ts);
        svc_->confirm(id, ts);  // 确认变更:封存旧版本、候选生效、归档新版本

        // 镜像 voxelstore:块当前态 + seal 旧版本 + append 新版本
        if (store_) {
            if (auto now = svc_->get(id)) {
                store_->put_block(to_voxel_block(*now));
                store_->seal_version(id, ts);
                voxelstore::VoxelVersion ver;
                ver.block_id = id;
                ver.version = now->version;
                ver.payload = now->payload;
                ver.confidence = now->confidence;
                ver.state = stmb::toString(now->state);
                ver.valid_from = ts;
                store_->append_version(ver);
            }
        }
    }

    // ---- b) 本体轨迹:单目标事件 = 一次过点观测 ----
    if (e.writes.size() == 1) {
        const std::string& oid = e.writes.begin()->first;
        stmb::InstanceId iid = 0;
        auto iit = ontology_instance_.find(oid);
        if (iit != ontology_instance_.end()) iid = iit->second;
        const std::array<double, 3> zero{0.0, 0.0, 0.0};
        // 位置取锚点 AABB(零 AABB 时中心即 (0,0,0))
        const stmb::InstanceId nid =
            svc_->reportMoving(iid, "ontology", region, zero, ts, 0);
        if (nid == 0) return;  // 动态层未启用(便捷构造):轨迹由块 payload 反查
        ontology_instance_[oid] = nid;

        // 镜像 voxelstore:动态实例当前态 + 轨迹
        if (store_) {
            voxelstore::VoxelInstance vi;
            vi.id = nid;
            vi.class_label = "ontology";
            vi.bounds.min_x = region.min[0];
            vi.bounds.min_y = region.min[1];
            vi.bounds.min_z = region.min[2];
            vi.bounds.max_x = region.max[0];
            vi.bounds.max_y = region.max[1];
            vi.bounds.max_z = region.max[2];
            for (const auto& tp : svc_->trajectoryOf(nid)) {
                voxelstore::TrackPoint vt;
                vt.t = tp.t;
                vt.px = tp.position[0];
                vt.py = tp.position[1];
                vt.pz = tp.position[2];
                vt.vx = tp.velocity[0];
                vt.vy = tp.velocity[1];
                vt.vz = tp.velocity[2];
                vi.trajectory.push_back(vt);
            }
            if (!vi.trajectory.empty()) vi.latest = vi.trajectory.back();
            vi.state = "Active";
            vi.last_seen = ts;
            store_->put_instance(vi);
        }
    }
}

// ----------------------------------------------------------------------------
// 重建:清空运行期服务与索引,逐事件重放(投影可丢弃可重建)
// ----------------------------------------------------------------------------

void SpacetimeTree::replay(const EventLog& log) {
    delete svc_;
    svc_ = new stmb::StmbService(4096, 1.0, 1);
    anchor_blocks_.clear();
    ontology_instance_.clear();
    for (const auto& e : log.all()) record_event(e);
}

// ----------------------------------------------------------------------------
// 查询
// ----------------------------------------------------------------------------

std::vector<json> SpacetimeTree::query_region(const std::string& anchor_path,
                                              int64_t from_seq,
                                              int64_t to_seq) const {
    std::vector<json> out;
    for (const auto& [anchor, block_id] : anchor_blocks_) {
        if (!anchor_matches(anchor_path, anchor)) continue;  // 含下钻
        auto block = svc_->get(block_id);
        if (!block) continue;
        const json payload = parse_lenient(block->payload);
        if (payload.is_discarded()) continue;
        auto eit = payload.find("events");
        if (eit == payload.end() || !eit->is_array()) continue;
        for (const auto& ev : *eit) {
            const int64_t seq = ev.value("event_id", (int64_t)0);
            if (from_seq != 0 && seq < from_seq) continue;  // 0 = 不限
            if (to_seq != 0 && seq > to_seq) continue;
            json item = ev;
            item["anchor"] = anchor;  // 附带块锚点(下钻来源可辨)
            out.push_back(std::move(item));
        }
    }
    return out;  // anchor_blocks_ 为 std::map:锚点字典序,块内 event_id 升序
}

std::optional<json> SpacetimeTree::anchor_as_of(const std::string& anchor_path,
                                                int64_t settle_seq) const {
    auto it = anchor_blocks_.find(anchor_path);
    if (it == anchor_blocks_.end()) return std::nullopt;
    auto ver = svc_->getAt(it->second, settle_seq);
    if (!ver) return std::nullopt;
    const json payload = parse_lenient(ver->payload);
    if (payload.is_discarded()) return std::nullopt;
    return payload;
}

std::vector<std::pair<int64_t, std::string>>
SpacetimeTree::trajectory_of(const std::string& ontology_id) const {
    // stmb 轨迹点只有坐标没有锚点名:反向扫全部锚点块的事件流,
    // 收集 targets 含该本体的 (event_id, 块锚点),按 event_id 升序
    std::vector<std::pair<int64_t, std::string>> out;
    for (const auto& [anchor, block_id] : anchor_blocks_) {
        auto block = svc_->get(block_id);
        if (!block) continue;
        const json payload = parse_lenient(block->payload);
        if (payload.is_discarded()) continue;
        auto eit = payload.find("events");
        if (eit == payload.end() || !eit->is_array()) continue;
        for (const auto& ev : *eit) {
            auto tit = ev.find("targets");
            if (tit == ev.end() || !tit->is_array()) continue;
            bool hits = false;
            for (const auto& t : *tit) {
                if (t.is_string() && t.get<std::string>() == ontology_id) {
                    hits = true;
                    break;
                }
            }
            if (hits) out.emplace_back(ev.value("event_id", (int64_t)0), anchor);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

std::optional<std::string>
SpacetimeTree::current_anchor_of(const std::string& ontology_id) const {
    const auto traj = trajectory_of(ontology_id);
    if (traj.empty()) return std::nullopt;
    return traj.back().second;  // 最后一次过点
}

} // namespace mse
