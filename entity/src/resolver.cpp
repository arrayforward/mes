#include "entity/resolver.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <geocore/anchor_path.hpp>

#include "entitytree/errors.h"

namespace entity {

using namespace entitytree;

namespace {

/// 解析 anchor_ref 为 AnchorPath 路径码；非法（非路径码字符串）返回 nullopt，
/// 调用方降级为字符串等值（向后兼容旧数据/自由字符串锚点）。
std::optional<geocore::AnchorPath> parse_anchor(const std::string& s) {
    geocore::AnchorPath p;
    if (!geocore::ParseAnchorPath(s, p)) return std::nullopt;
    return p;
}

/// 层级邻近判定（geocore 路径码纯函数）：
///   同一锚点（路径相等）/ 祖先后代（IsPrefixOf）/ LCA 深度 ≥ min_lca_depth；
/// 任一 anchor 非合法路径码时降级为字符串等值，行为与升级前一致。
bool anchors_near(const std::string& a, const std::string& b, int min_lca_depth) {
    auto pa = parse_anchor(a);
    auto pb = parse_anchor(b);
    if (!pa || !pb) return a == b;
    if (*pa == *pb) return true;                               // 同一锚点
    if (pa->IsPrefixOf(*pb) || pb->IsPrefixOf(*pa)) return true;  // 祖先/后代
    return geocore::LcaDepth(*pa, *pb) >= min_lca_depth;       // LCA 邻近
}

/// 侧写某 key 下的去重值集合。
std::unordered_set<std::string> profile_values(const AttrProfile& p, const std::string& key) {
    std::unordered_set<std::string> out;
    if (!p.attributes.contains(key)) return out;
    for (const auto& e : p.attributes[key]) out.insert(e.value("value", ""));
    return out;
}

/// 侧写出现过的全部独立来源。
std::unordered_set<std::string> profile_sources(const AttrProfile& p) {
    std::unordered_set<std::string> out;
    for (auto& [key, entries] : p.attributes.items())
        if (entries.is_array())
            for (const auto& e : entries) out.insert(e.value("source_id", ""));
    return out;
}

/// 侧写全部条目中最新的观测时间（无 ts 记录返回 0）。
int64_t profile_latest_ts(const AttrProfile& p) {
    int64_t latest = 0;
    for (auto& [key, entries] : p.attributes.items())
        if (entries.is_array())
            for (const auto& e : entries)
                latest = std::max(latest, (int64_t)e.value("ts", (int64_t)0));
    return latest;
}

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

} // namespace

Resolver::Resolver(EntityStore& store, ResolverConfig config)
    : store_(store), cfg_(std::move(config)) {}

void Resolver::set_distance_provider(DistanceProvider provider) {
    distance_provider_ = std::move(provider);
}

// 某层时间桶长度（秒）：bucket_sizes 为空时仅 level 0（= bucket_size 单级）
int64_t Resolver::bucket_span(int level) const {
    if (cfg_.bucket_sizes.empty()) return cfg_.bucket_size;
    if (level < 0 || level >= (int)cfg_.bucket_sizes.size()) return 0;
    return cfg_.bucket_sizes[level];
}

int Resolver::level_count() const {
    return cfg_.bucket_sizes.empty() ? 1 : (int)cfg_.bucket_sizes.size();
}

double Resolver::weight_of(const std::string& key) const {
    auto it = cfg_.attribute_weight.find(key);
    return it != cfg_.attribute_weight.end() ? it->second : 1.0;
}

// 来源可靠性：et_sources 记录（entitytree ↔ stmb 共享）；未注册按 1.0。
double Resolver::reliability_of(const std::string& source_id) const {
    if (auto s = store_.get_source(source_id)) return s->reliability;
    return 1.0;
}

// info_score：对每个 attribute_key，取 weight(key) × log1p(该 key 下不同 source_id 数目)。
// 只数来源数，不乘可靠性/置信度——保持简单可解释。
double Resolver::compute_info_score(const AttrProfile& p) const {
    double score = 0.0;
    for (auto& [key, entries] : p.attributes.items()) {
        if (!entries.is_array()) continue;
        std::unordered_set<std::string> sources;
        for (const auto& e : entries) sources.insert(e.value("source_id", ""));
        score += weight_of(key) * std::log1p((double)sources.size());
    }
    return score;
}

std::string Resolver::ingest(const Observation& obs) {
    int64_t span = bucket_span(0);
    int64_t bucket = span > 0 ? obs.timestamp / span : 0;
    // 找到或创建 (anchor_ref, time_bucket) 对应的 level 0 侧写
    auto existing = store_.query_profiles(obs.anchor_ref, bucket, bucket,
                                          std::nullopt, 0);
    AttrProfile p;
    bool is_new = existing.empty();
    if (is_new) {
        p.profile_id = new_id("ap");
        p.anchor_ref = obs.anchor_ref;
        p.time_bucket = bucket;
        p.level = 0;
    } else {
        p = existing[0];
    }

    Observation o = obs;
    if (o.observation_id.empty()) o.observation_id = new_id("ob");
    int64_t oseq = store_.append_observation(o);
    if (is_new) p.seq = oseq;

    // 追加属性条目（带观测时间，供时效衰减取"最新观测时间"）并重算信息量
    p.attributes[o.attribute_key].push_back(
        {{"value", o.value}, {"confidence", o.confidence},
         {"source_id", o.source_id}, {"obs_seq", oseq}, {"ts", o.timestamp}});
    p.info_score = compute_info_score(p);

    // 浮现判定：达阈值且仍在积累中 → emerged，触发解析
    bool emerge = (p.status == "pool" && p.info_score >= cfg_.theta_form);
    if (emerge) p.status = "emerged";
    store_.upsert_profile(p);
    if (emerge) resolve(p.profile_id);
    return p.profile_id;
}

// 候选实体打分：属性一致性（相同 key 值匹配加分、冲突减分，按 attribute_weight 加权，
// 归一化到 0~1）+ 来源独立性加分（侧写带来实体没见过的新来源，每个 +0.05，封顶 0.15）。
double Resolver::score_candidate(const AttrProfile& p, const EntityNode& e) {
    double pos = 0.0, neg = 0.0, norm = 0.0;
    for (auto& [key, entries] : p.attributes.items()) {
        if (!e.attributes.contains(key)) continue;  // 实体无此属性：不计入
        double w = weight_of(key);
        norm += w;
        auto pvals = profile_values(p, key);
        std::unordered_set<std::string> evals;
        for (const auto& en : e.attributes[key]) evals.insert(en.value("value", ""));
        size_t hit = 0;
        for (const auto& v : pvals)
            if (evals.count(v)) ++hit;
        if (hit > 0) pos += w * (double)hit / (double)pvals.size();
        else neg += w;
    }
    // [-1,1] 映射到 [0,1]；无公共 key 时中性 0.5
    double c = norm > 0.0 ? ((pos - neg) / norm + 1.0) / 2.0 : 0.5;
    // 来源独立性加分
    std::unordered_set<std::string> esources;
    for (const auto& ep : store_.profiles_of_entity(e.entity_id)) {
        auto s = profile_sources(ep);
        esources.insert(s.begin(), s.end());
    }
    size_t new_sources = 0;
    for (const auto& s : profile_sources(p))
        if (!esources.count(s)) ++new_sources;
    double bonus = std::min(0.15, 0.05 * (double)new_sources);
    return clamp01(c + bonus);
}

// 侧写全部条目的"有效置信度"均值：confidence × source_reliability（未注册按 1.0）。
// 用于新建/split 绑定的置信度——低可靠来源的观测天然撑起更低的可信度。
void Resolver::resolve(const std::string& profile_id) {
    AttrProfile p = store_.get_profile(profile_id);

    // 1. Blocking 召回候选实体：
    //    a) 共享相同 (attribute_key, value) 的其它已绑定侧写所属的实体；
    //    b) 层级邻近锚点 ±1 桶内的实体（geocore AnchorPath：同锚点/祖先后代/
    //       LCA 邻近，非路径码降级为字符串等值）——先查 level 0 细桶，
    //       无候选逐级回退到更粗 level（粗桶覆盖更长时间跨度，观测稀疏时
    //       常驻实体仍可召回），找到候选即停。
    std::unordered_set<std::string> cand_ids;
    for (auto& [key, entries] : p.attributes.items()) {
        for (const auto& v : profile_values(p, key))
            for (const auto& other : store_.find_profiles_by_attribute(key, v)) {
                if (other.profile_id == p.profile_id) continue;
                if (auto eff = store_.effective_binding(other.profile_id))
                    cand_ids.insert(eff->entity_id);
            }
    }
    const int64_t rep_ts = p.time_bucket * bucket_span(p.level);
    for (int lv = 0; lv < level_count(); ++lv) {
        const int64_t span = bucket_span(lv);
        const int64_t cb = span > 0 ? rep_ts / span : 0;
        for (const auto& nb : store_.query_profiles(std::nullopt, cb - 1, cb + 1,
                                                    std::nullopt, lv)) {
            if (nb.profile_id == p.profile_id) continue;
            if (!anchors_near(nb.anchor_ref, p.anchor_ref, cfg_.min_lca_depth)) continue;
            if (auto eff = store_.effective_binding(nb.profile_id))
                cand_ids.insert(eff->entity_id);
        }
        if (!cand_ids.empty()) break;  // 找到候选即停
    }

    // 2. 轨迹连续性校验（vmax 物理可达；max_speed=0 或未设 provider 时跳过）：
    //    取侧写与候选实体最近侧写的 (anchor, 最新观测时间)，距离 > vmax×Δt
    //    即物理不可能——veto 排除该候选 / penalty 给 score 乘压分因子；
    //    provider 无数据（nullopt）不约束。
    std::unordered_set<std::string> vetoed;
    std::unordered_set<std::string> penalized;
    if (cfg_.max_speed > 0.0 && distance_provider_) {
        const int64_t pts = profile_latest_ts(p);
        if (pts > 0) {
            for (const auto& eid : cand_ids) {
                const AttrProfile* nearest = nullptr;
                AttrProfile nearest_p;
                int64_t best_dt = INT64_MAX;
                for (const auto& ep : store_.profiles_of_entity(eid)) {
                    int64_t dt = profile_latest_ts(ep) - pts;
                    if (dt < 0) dt = -dt;
                    if (dt < best_dt) {
                        best_dt = dt;
                        nearest_p = ep;
                        nearest = &nearest_p;
                    }
                }
                if (!nearest) continue;
                auto d = distance_provider_(p.anchor_ref, nearest->anchor_ref);
                if (d && *d > cfg_.max_speed * (double)best_dt) {
                    // Δt=0 时任何 d>0 即违反（上式已涵盖：vmax×0=0）
                    if (cfg_.trajectory_check == "penalty") penalized.insert(eid);
                    else vetoed.insert(eid);   // 默认 veto
                }
            }
        }
    }
    for (const auto& eid : vetoed) cand_ids.erase(eid);

    // 3. 打分（penalty 候选乘压分因子），取最优候选
    std::string best_id;
    double best = -1.0;
    for (const auto& eid : cand_ids) {
        double s = score_candidate(p, store_.get_entity(eid));
        if (penalized.count(eid)) s *= cfg_.trajectory_penalty;
        if (s > best) {
            best = s;
            best_id = eid;
        }
    }

    // 有效置信度均值（confidence × 来源可靠性）
    auto mean_eff = [this](const AttrProfile& prof) {
        double sum = 0.0;
        size_t n = 0;
        for (auto& [key, entries] : prof.attributes.items()) {
            if (!entries.is_array()) continue;
            for (const auto& e : entries) {
                sum += e.value("confidence", 1.0) *
                       reliability_of(e.value("source_id", ""));
                ++n;
            }
        }
        return n ? sum / (double)n : 1.0;
    };

    // 3. 决策
    if (best_id.empty() || best <= cfg_.theta_split) {
        // 无候选或证据太弱：新建实体（status=candidate），初始绑定置信度取有效置信度均值
        EntityNode e;
        e.entity_id = new_id("en");
        store_.upsert_entity(e);  // 先建行，append_binding 要校验实体存在
        EntityBinding b;
        b.binding_id = new_id("bd");
        b.profile_id = p.profile_id;
        b.entity_id = e.entity_id;
        b.confidence = mean_eff(p);
        b.kind = "merge";
        b.note = "initial: new entity";
        store_.append_binding(b);
        rebuild_entity(e.entity_id);
        p.status = "linked";
        store_.upsert_profile(p);
    } else if (best >= cfg_.theta_merge) {
        // 合并：追加 merge 绑定（confidence = score），重建目标实体
        EntityBinding b;
        b.binding_id = new_id("bd");
        b.profile_id = p.profile_id;
        b.entity_id = best_id;
        b.confidence = best;
        b.kind = "merge";
        b.note = "auto merge";
        store_.append_binding(b);
        rebuild_entity(best_id);
        p.status = "linked";
        store_.upsert_profile(p);
        // 反馈闭环：合并成功 → 涉及来源 reliability 轻微上调
        adjust_sources(p, cfg_.source_reward, profile_latest_ts(p));
    } else {
        // 存疑区：侧写保持 emerged 等更多证据；候选实体标 disputed
        EntityNode e = store_.get_entity(best_id);
        if (e.status == "candidate" || e.status == "confirmed") {
            e.status = "disputed";
            store_.upsert_entity(e);
        }
    }
}

std::string Resolver::split(const std::string& profile_id,
                            const std::string& new_entity_name) {
    AttrProfile p = store_.get_profile(profile_id);
    auto eff = store_.effective_binding(profile_id);
    if (!eff) throw EntityTreeError("split: profile not bound: " + profile_id);
    std::string old_entity = eff->entity_id;

    EntityNode ne;
    ne.entity_id = new_id("en");
    ne.name = new_entity_name;
    ne.type = store_.get_entity(old_entity).type;  // 继承原实体类型
    store_.upsert_entity(ne);

    // 有效置信度均值（confidence × 来源可靠性），与 resolve 新建路径一致
    double sum = 0.0;
    size_t n = 0;
    for (auto& [key, entries] : p.attributes.items()) {
        if (!entries.is_array()) continue;
        for (const auto& e : entries) {
            sum += e.value("confidence", 1.0) * reliability_of(e.value("source_id", ""));
            ++n;
        }
    }

    // 拆分 = 追加指向新实体的 split 绑定（latest wins，历史全保留）
    EntityBinding b;
    b.binding_id = new_id("bd");
    b.profile_id = profile_id;
    b.entity_id = ne.entity_id;
    b.confidence = n ? sum / (double)n : 1.0;
    b.kind = "split";
    b.note = "split from " + old_entity;
    store_.append_binding(b);

    rebuild_entity(ne.entity_id);
    rebuild_entity(old_entity);
    // 反馈闭环：拆分 → 被拆侧写涉及来源 reliability 轻微下调
    adjust_sources(p, -cfg_.source_penalty, profile_latest_ts(p));
    return ne.entity_id;
}

std::vector<std::string> Resolver::rollup(const std::string& entity_id, int level) {
    std::vector<std::string> out;
    if (level < 1 || level >= level_count()) return out;

    // 按 level 层粗桶分组 level-1 层的已链接细桶侧写（代表时间 = 桶起点）
    std::map<int64_t, std::vector<AttrProfile>> groups;
    for (const auto& p : store_.profiles_of_entity(entity_id)) {
        if (p.level != level - 1 || p.status != "linked") continue;
        const int64_t span = bucket_span(level - 1);
        const int64_t rep = span > 0 ? p.time_bucket * span : 0;
        const int64_t cspan = bucket_span(level);
        groups[cspan > 0 ? rep / cspan : 0].push_back(p);
    }
    // 幂等：该粗桶已存在同层侧写则跳过（维护任务可重复跑）
    auto existing = store_.profiles_of_entity(entity_id);
    auto has_coarse = [&](int64_t cbucket) {
        for (const auto& p : existing)
            if (p.level == level && p.time_bucket == cbucket) return true;
        return false;
    };

    for (auto& [cbucket, members] : groups) {
        if (has_coarse(cbucket)) continue;
        AttrProfile cp;
        cp.profile_id = new_id("ap");
        cp.time_bucket = cbucket;
        cp.level = level;
        cp.status = "linked";
        // 代表锚点取最新观测侧写的锚点；seq 取最老成员（排序稳定）
        int64_t latest = -1;
        cp.seq = members[0].seq;
        for (const auto& m : members) {
            cp.seq = std::min(cp.seq, m.seq);
            const int64_t mts = profile_latest_ts(m);
            if (mts >= latest) {
                latest = mts;
                cp.anchor_ref = m.anchor_ref;
            }
            // 属性并集：保留各观测条目（含 ts / source_id / obs_seq）
            for (auto& [key, entries] : m.attributes.items())
                for (const auto& en : entries) cp.attributes[key].push_back(en);
        }
        cp.info_score = compute_info_score(cp);
        store_.upsert_profile(cp);
        // 粗桶侧写挂到同一实体（append-only 绑定，note 标 rollup）
        EntityBinding b;
        b.binding_id = new_id("bd");
        b.profile_id = cp.profile_id;
        b.entity_id = entity_id;
        b.confidence = 1.0;
        b.kind = "merge";
        b.note = "rollup L" + std::to_string(level);
        store_.append_binding(b);
        out.push_back(cp.profile_id);
    }
    if (!out.empty()) rebuild_entity(entity_id);
    return out;
}

void Resolver::rebuild_entity(const std::string& entity_id) {
    EntityNode e = store_.get_entity(entity_id);
    auto profiles = store_.profiles_of_entity(entity_id);

    // merged view 只聚合 level 0 细桶侧写：rollup 粗桶侧写是派生镜像
    // （为召回服务），不是新证据，不计入权重与 credibility（避免重复计分）；
    // 实体若只有粗桶侧写（异常态）则退回全量聚合
    bool has_fine = false;
    for (const auto& p : profiles)
        if (p.level == 0) has_fine = true;
    std::map<std::string, std::map<std::string, double>> acc;
    std::unordered_set<std::string> sources;
    double conf_sum = 0.0;
    size_t conf_n = 0;
    int64_t latest_ts = 0;
    for (const auto& p : profiles) {
        if (has_fine && p.level != 0) continue;
        for (auto& [key, entries] : p.attributes.items()) {
            if (!entries.is_array()) continue;
            for (const auto& en : entries) {
                double eff_conf = en.value("confidence", 1.0) *
                                  reliability_of(en.value("source_id", ""));
                acc[key][en.value("value", "")] += eff_conf;
                sources.insert(en.value("source_id", ""));
                latest_ts = std::max(latest_ts, (int64_t)en.value("ts", (int64_t)0));
            }
        }
        if (auto eb = store_.effective_binding(p.profile_id)) {
            conf_sum += eb->confidence;
            ++conf_n;
        }
    }
    json view = json::object();
    for (auto& [key, vals] : acc) {
        json arr = json::array();
        for (auto& [v, w] : vals) arr.push_back({{"value", v}, {"weight", w}});
        view[key] = arr;
    }
    e.attributes = view;
    e.view_version += 1;
    // credibility 基准值 = 有效绑定置信度均值 × 来源多样性因子（1 - 1/(1+独立来源数)）；
    // updated_at 记录基准时间（最新观测时间）。基准值此后不随时间改写——
    // 时效衰减只在查询时计算（effective_credibility / search 的 now 参数）。
    double avg = conf_n ? conf_sum / (double)conf_n : 0.0;
    double diversity = (double)sources.size() / (1.0 + (double)sources.size());
    e.credibility = avg * diversity;
    e.updated_at = latest_ts;
    store_.upsert_entity(e);
}

// stmb 纪律：effective = base × 0.5^((now − updated_at)/half_life)；
// half_life ≤ 0 或 elapsed ≤ 0 时原样返回 base。存储基准值不改写。
double Resolver::effective_credibility(const std::string& entity_id, int64_t now) {
    EntityNode e = store_.get_entity(entity_id);
    if (cfg_.credibility_half_life <= 0.0 || now <= e.updated_at) return e.credibility;
    return e.credibility *
           std::pow(0.5, (double)(now - e.updated_at) / cfg_.credibility_half_life);
}

std::vector<SearchHit> Resolver::search(
    const std::vector<std::pair<std::string, std::string>>& query_attrs,
    size_t topk, double min_credibility, int64_t now) {
    double total_w = 0.0;
    for (const auto& [k, v] : query_attrs) total_w += weight_of(k);

    // 召回：多路等值查询取并集；打分：命中属性按 attribute_weight 加权
    std::unordered_map<std::string, SearchHit> hits;
    std::unordered_map<std::string, std::unordered_set<std::string>> seen_keys;
    for (const auto& [k, v] : query_attrs) {
        for (const auto& e : store_.find_entities_by_attribute(k, v)) {
            if (!seen_keys[e.entity_id].insert(k).second) continue;  // 同 key 不重复计分
            auto& h = hits[e.entity_id];
            h.entity_id = e.entity_id;
            h.credibility = now > 0 ? effective_credibility(e.entity_id, now)
                                    : e.credibility;
            double w = weight_of(k);
            // 属性新鲜度因子：0.5^((now − 该属性最新观测时间)/attribute_half_life[key])；
            // 未配半衰期 / now=0 / 无 ts 记录时因子 = 1（行为与不加衰减一致）
            double freshness = 1.0;
            int64_t attr_ts = 0;
            auto hl = cfg_.attribute_half_life.find(k);
            if (hl != cfg_.attribute_half_life.end() && hl->second > 0.0 && now > 0) {
                for (const auto& ep : store_.profiles_of_entity(e.entity_id)) {
                    if (!ep.attributes.contains(k)) continue;
                    for (const auto& en : ep.attributes[k])
                        attr_ts = std::max(attr_ts, (int64_t)en.value("ts", (int64_t)0));
                }
                if (attr_ts > 0 && now > attr_ts)
                    freshness = std::pow(0.5, (double)(now - attr_ts) / hl->second);
            }
            h.match_score += w * freshness;
            h.explanation.push_back(
                {{"key", k}, {"value", v}, {"weight", w}, {"hit", true},
                 {"freshness", freshness}, {"latest_ts", attr_ts}});
        }
    }

    std::vector<SearchHit> out;
    for (auto& [eid, h] : hits) {
        if (h.credibility < min_credibility) continue;
        h.match_score = total_w > 0.0 ? h.match_score / total_w : 0.0;
        h.final_score = cfg_.alpha * h.match_score + cfg_.beta * h.credibility;
        out.push_back(std::move(h));
    }
    std::sort(out.begin(), out.end(), [](const SearchHit& a, const SearchHit& b) {
        if (a.final_score != b.final_score) return a.final_score > b.final_score;
        return a.entity_id < b.entity_id;
    });
    if (out.size() > topk) out.resize(topk);
    return out;
}

// 反馈闭环：调整侧写涉及来源的 reliability（clamp 到 [0.05, 1.0]）。
void Resolver::adjust_sources(const AttrProfile& p, double delta, int64_t now) {
    for (const auto& src : profile_sources(p)) {
        SourceRecord rec;
        if (auto s = store_.get_source(src)) rec = *s;
        else rec.source_id = src;
        rec.reliability = std::min(1.0, std::max(0.05, rec.reliability + delta));
        rec.updated_at = now;
        store_.upsert_source(rec);
    }
}

} // namespace entity
