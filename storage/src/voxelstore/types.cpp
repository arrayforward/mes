#include "voxelstore/types.h"

namespace voxelstore {

bool Aabb::intersects(const Aabb& o) const {
    return min_x <= o.max_x && max_x >= o.min_x &&
           min_y <= o.max_y && max_y >= o.min_y &&
           min_z <= o.max_z && max_z >= o.min_z;
}

void to_json(json& j, const PatternPhase& p) {
    j = json{{"offset_ms", p.offset_ms}, {"duration_ms", p.duration_ms},
             {"payload", p.payload}};
}
void from_json(const json& j, PatternPhase& p) {
    p.offset_ms = j.value("offset_ms", (int64_t)0);
    p.duration_ms = j.value("duration_ms", (int64_t)0);
    p.payload = j.value("payload", "");
}

void to_json(json& j, const PeriodicPattern& p) {
    j = json{{"period_ms", p.period_ms}, {"phases", p.phases}};
}
void from_json(const json& j, PeriodicPattern& p) {
    p.period_ms = j.value("period_ms", (int64_t)0);
    p.phases = j.value("phases", std::vector<PatternPhase>{});
}

void to_json(json& j, const TrackPoint& p) {
    j = json{{"t", p.t}, {"px", p.px}, {"py", p.py}, {"pz", p.pz},
             {"vx", p.vx}, {"vy", p.vy}, {"vz", p.vz}};
}
void from_json(const json& j, TrackPoint& p) {
    p.t = j.value("t", (int64_t)0);
    p.px = j.value("px", 0.0);
    p.py = j.value("py", 0.0);
    p.pz = j.value("pz", 0.0);
    p.vx = j.value("vx", 0.0);
    p.vy = j.value("vy", 0.0);
    p.vz = j.value("vz", 0.0);
}

} // namespace voxelstore
