#include "record_conformance.h"

#include "storage/record_backend.h"
#include "test_framework.h"

using namespace storage;

namespace {

// 按 id 收集查询结果的主键集合（顺序无关的断言用 size + 命中判断）
bool has_id(const std::vector<Record>& rows, const std::string& id) {
    for (const auto& r : rows)
        if (as_text(r.at("id")) == id) return true;
    return false;
}

} // namespace

void run_record_conformance(RecordBackend& backend) {
    TableSchema s;
    s.name = "stations";
    s.fields = {{"id", FieldType::kText}, {"path", FieldType::kText},
                {"load", FieldType::kInt}, {"note", FieldType::kText}};
    s.pk = "id";
    backend.create_table(s);

    backend.put("stations", {{"id", vtext("st-001")},
                             {"path", vtext("总装车间/总装线/工位1")},
                             {"load", vint(10)}, {"note", vtext("白班")}});
    backend.put("stations", {{"id", vtext("st-002")},
                             {"path", vtext("总装车间/总装线/工位2")},
                             {"load", vint(20)}, {"note", vnull()}});
    backend.put("stations", {{"id", vtext("st-003")},
                             {"path", vtext("总装车间/涂装线/工位1")},
                             {"load", vint(30)}, {"note", vtext("夜班")}});
    backend.put("stations", {{"id", vtext("st-004")},
                             {"path", vtext("冲压车间/冲压线/工位1")},
                             {"load", vint(40)}, {"note", vtext("白班")}});

    // ============ Prefix：锚点路径层级查询 ============
    {
        // 前缀命中：一条线下全部工位
        auto rows = backend.query("stations",
                                  {{"path", Op::Prefix, vtext("总装车间/总装线")}});
        CHECK_EQ(rows.size(), (size_t)2);
        CHECK(has_id(rows, "st-001"));
        CHECK(has_id(rows, "st-002"));
    }
    {
        // 车间级前缀：命中车间内全部线
        auto rows = backend.query("stations",
                                  {{"path", Op::Prefix, vtext("总装车间")}});
        CHECK_EQ(rows.size(), (size_t)3);
    }
    {
        // UTF-8 中文短前缀
        auto rows = backend.query("stations",
                                  {{"path", Op::Prefix, vtext("冲压")}});
        CHECK_EQ(rows.size(), (size_t)1);
        CHECK(has_id(rows, "st-004"));
    }
    {
        // 前缀不命中：不存在的线 / 不存在的车间
        CHECK(backend.query("stations",
                            {{"path", Op::Prefix, vtext("总装车间/总装线3")}}).empty());
        CHECK(backend.query("stations",
                            {{"path", Op::Prefix, vtext("焊装车间")}}).empty());
    }
    {
        // 按 id 前缀检索
        auto rows = backend.query("stations", {{"id", Op::Prefix, vtext("st-00")}});
        CHECK_EQ(rows.size(), (size_t)4);
        rows = backend.query("stations", {{"id", Op::Prefix, vtext("st-002")}});
        CHECK_EQ(rows.size(), (size_t)1);
        CHECK(has_id(rows, "st-002"));
    }
    {
        // 整串作前缀 = 精确命中；且不会误命中更长的兄弟值
        auto rows = backend.query(
            "stations", {{"path", Op::Prefix, vtext("总装车间/涂装线/工位1")}});
        CHECK_EQ(rows.size(), (size_t)1);
        CHECK(has_id(rows, "st-003"));
    }

    // ============ Ne：不等于 ============
    {
        // 普通不等：命中其余 3 行
        auto rows = backend.query(
            "stations", {{"path", Op::Ne, vtext("总装车间/涂装线/工位1")}});
        CHECK_EQ(rows.size(), (size_t)3);
        CHECK(!has_id(rows, "st-003"));
    }
    {
        // Ne 与 NULL 行：note != "白班" 只命中 "夜班"；
        // note 为 NULL 的 st-002 不命中（与 Le/Ge 一致：NULL 行不参与比较）
        auto rows = backend.query("stations", {{"note", Op::Ne, vtext("白班")}});
        CHECK_EQ(rows.size(), (size_t)1);
        CHECK(has_id(rows, "st-003"));
        // 对照：Eq 命中两个白班，IsNull 命中 st-002
        CHECK_EQ(backend.query("stations", {{"note", Op::Eq, vtext("白班")}}).size(),
                 (size_t)2);
        CHECK_EQ(backend.query("stations", {{"note", Op::IsNull, vnull()}}).size(),
                 (size_t)1);
    }
    {
        // Ne 数值字段
        auto rows = backend.query("stations", {{"load", Op::Ne, vint(20)}});
        CHECK_EQ(rows.size(), (size_t)3);
        CHECK(!has_id(rows, "st-002"));
    }

    // ============ 组合：Prefix + Ne ============
    {
        // 总装车间内、但排除涂装线
        auto rows = backend.query("stations",
                                  {{"path", Op::Prefix, vtext("总装车间")},
                                   {"path", Op::Ne, vtext("总装车间/涂装线/工位1")}});
        CHECK_EQ(rows.size(), (size_t)2);
        CHECK(has_id(rows, "st-001"));
        CHECK(has_id(rows, "st-002"));
    }
}
