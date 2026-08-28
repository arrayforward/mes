#include "storage/record_codec.h"

#include <algorithm>

namespace storage {
namespace {

const char* field_type_name(FieldType t) {
    switch (t) {
        case FieldType::kInt: return "int";
        case FieldType::kReal: return "real";
        case FieldType::kText: return "text";
    }
    return "text";
}

FieldType field_type_from(const std::string& s) {
    if (s == "int") return FieldType::kInt;
    if (s == "real") return FieldType::kReal;
    return FieldType::kText;
}

int compare_values(const Value& a, const Value& b) {
    if (is_null(a) || is_null(b)) {
        if (is_null(a) && is_null(b)) return 0;
        return is_null(a) ? -1 : 1;
    }
    bool a_num = std::holds_alternative<int64_t>(a) || std::holds_alternative<double>(a);
    bool b_num = std::holds_alternative<int64_t>(b) || std::holds_alternative<double>(b);
    if (a_num && b_num) {
        double x = as_real(a), y = as_real(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    const std::string& x = as_text(a);
    const std::string& y = as_text(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}

} // namespace

json value_to_json(const Value& v) {
    if (is_null(v)) return json{{"t", "n"}};
    if (auto* i = std::get_if<int64_t>(&v)) return json{{"t", "i"}, {"v", *i}};
    if (auto* d = std::get_if<double>(&v)) return json{{"t", "r"}, {"v", *d}};
    return json{{"t", "s"}, {"v", as_text(v)}};
}

Value value_from_json(const json& j) {
    const std::string& t = j.at("t").get_ref<const std::string&>();
    if (t == "i") return vint(j.at("v").get<int64_t>());
    if (t == "r") return vreal(j.at("v").get<double>());
    if (t == "s") return vtext(j.at("v").get<std::string>());
    return vnull();
}

json record_to_json(const Record& rec) {
    json j = json::object();
    for (const auto& [f, v] : rec) j[f] = value_to_json(v);
    return j;
}

Record record_from_json(const json& j) {
    Record rec;
    for (auto it = j.begin(); it != j.end(); ++it) rec[it.key()] = value_from_json(it.value());
    return rec;
}

json schema_to_json(const TableSchema& s) {
    json fields = json::array();
    for (const auto& f : s.fields)
        fields.push_back({{"name", f.name}, {"type", field_type_name(f.type)}});
    return json{{"name", s.name}, {"fields", fields}, {"pk", s.pk},
                {"auto_seq", s.auto_seq}, {"indexes", s.indexes}};
}

TableSchema schema_from_json(const json& j) {
    TableSchema s;
    s.name = j.at("name").get<std::string>();
    for (const auto& f : j.at("fields"))
        s.fields.push_back({f.at("name").get<std::string>(),
                            field_type_from(f.at("type").get<std::string>())});
    s.pk = j.at("pk").get<std::string>();
    s.auto_seq = j.value("auto_seq", false);
    s.indexes = j.value("indexes", std::vector<std::vector<std::string>>{});
    return s;
}

bool record_matches(const Record& rec, const std::vector<Condition>& conds) {
    for (const auto& c : conds) {
        auto it = rec.find(c.field);
        const Value& v = it != rec.end() ? it->second : vnull();
        bool ok;
        switch (c.op) {
            case Op::Eq:
                ok = (is_null(v) && is_null(c.value)) ||
                     (!is_null(v) && !is_null(c.value) &&
                      value_to_json(v) == value_to_json(c.value));
                break;
            case Op::Le: ok = !is_null(v) && compare_values(v, c.value) <= 0; break;
            case Op::Ge: ok = !is_null(v) && compare_values(v, c.value) >= 0; break;
            case Op::IsNull: ok = is_null(v); break;
            case Op::NotNull: ok = !is_null(v); break;
            default: ok = false;
        }
        if (!ok) return false;
    }
    return true;
}

void sort_records(std::vector<Record>& recs, const std::vector<Ordering>& order) {
    for (int i = (int)order.size() - 1; i >= 0; --i) {
        const Ordering& o = order[(size_t)i];
        std::stable_sort(recs.begin(), recs.end(), [&](const Record& x, const Record& y) {
            auto xv = x.find(o.field), yv = y.find(o.field);
            const Value& a = xv != x.end() ? xv->second : vnull();
            const Value& b = yv != y.end() ? yv->second : vnull();
            int cmp = compare_values(a, b);
            return o.desc ? cmp > 0 : cmp < 0;
        });
    }
}

const std::vector<std::string>* best_eq_index(const TableSchema& schema,
                                              const std::vector<Condition>& conds) {
    std::map<std::string, bool> eq;
    for (const auto& c : conds)
        if (c.op == Op::Eq) eq[c.field] = true;
    const std::vector<std::string>* best = nullptr;
    for (const auto& index : schema.indexes) {
        bool covered = true;
        for (const auto& f : index)
            if (!eq.count(f)) { covered = false; break; }
        if (covered && (!best || index.size() > best->size())) best = &index;
    }
    return best;
}

} // namespace storage
