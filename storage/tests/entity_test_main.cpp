#include <chrono>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

#include "entity_conformance.h"
#include "entitytree/backends/file_store.h"
#include "entitytree/backends/memory_store.h"
#include "entitytree/backends/redis_store.h"
#include "entitytree/backends/sql_store.h"
#include "test_framework.h"
#ifdef UNISTORE_WITH_MYSQL
#include "entitytree/backends/mysql_store.h"
#endif
#ifdef UNISTORE_WITH_PG
#include "entitytree/backends/pg_store.h"
#endif

using namespace entitytree;

TEST(memory_entity_conformance) {
    MemoryEntityStore store;
    run_entity_conformance(store);
}

TEST(sqlite_entity_conformance) {
    SqlEntityStore store(":memory:");
    run_entity_conformance(store);
}

TEST(file_entity_conformance) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "entitytree_test_file";
    fs::remove_all(dir);
    {
        FileEntityStore store(dir.string());
        run_entity_conformance(store);
    }
    fs::remove_all(dir);
}

// 持久化样本数据（纯存储写入，算法层在 entity 组件）
static void write_sample(EntityStore& store) {
    store.append_observation(Observation{"ob-p1", "plate_number", "沪A12345", 1.0,
                                         "cam1", "gate-1", 100, "", 0});
    AttrProfile p;
    p.profile_id = "ap-p1";
    p.anchor_ref = "gate-1";
    p.time_bucket = 0;
    p.attributes["plate_number"] = json::array(
        {json{{"value", "沪A12345"}, {"confidence", 1.0}, {"source_id", "cam1"},
              {"obs_seq", 1}, {"ts", 100}}});
    p.status = "linked";
    p.seq = 1;
    store.upsert_profile(p);
    EntityNode e;
    e.entity_id = "en-p1";
    e.attributes["plate_number"] = json::array({{{"value", "沪A12345"}, {"weight", 1.0}}});
    e.view_version = 1;
    e.credibility = 0.5;
    e.updated_at = 100;
    store.upsert_entity(e);
    store.append_binding(EntityBinding{"bd-p1", "ap-p1", "en-p1", 1.0, "merge", "", 0});
    store.upsert_source(SourceRecord{"cam1", 0.9, 100, json::object()});
}

static void check_sample(EntityStore& store) {
    auto e = store.get_entity("en-p1");
    CHECK(e.attributes.contains("plate_number"));
    CHECK_EQ(e.updated_at, (int64_t)100);
    CHECK_EQ(store.observations_of("gate-1").size(), (size_t)1);
    CHECK_EQ(store.profiles_of_entity("en-p1").size(), (size_t)1);
    CHECK_EQ(store.find_entities_by_attribute("plate_number", "沪A12345").size(),
             (size_t)1);
    auto s = store.get_source("cam1");
    CHECK(s.has_value());
    CHECK(s->reliability > 0.89 && s->reliability < 0.91);
}

TEST(file_entity_persistence) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "entitytree_test_file_persist";
    fs::remove_all(dir);
    {
        FileEntityStore store(dir.string());
        write_sample(store);
    }
    {
        FileEntityStore store(dir.string());  // 重新打开：schema + WAL 回放
        check_sample(store);
    }
    fs::remove_all(dir);
}

TEST(sqlite_entity_file_persistence) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "entitytree_test_persist";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto dbfile = (dir / "test.db").string();
    {
        SqlEntityStore store(dbfile);
        write_sample(store);
    }
    {
        SqlEntityStore store(dbfile);  // 重新打开，数据仍在
        check_sample(store);
    }
    fs::remove_all(dir);
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    // MySQL / Postgres 一致性测试：设置环境变量后才注册
#ifdef UNISTORE_WITH_MYSQL
    if (const char* dsn = std::getenv("UNISTORE_MYSQL_DSN")) {
        std::string dsn_str = dsn;
        tfw::registry().push_back({"mysql_entity_conformance", [dsn_str] {
            MySqlEntityStore store(dsn_str);
            run_entity_conformance(store);
        }});
    }
#endif
#ifdef UNISTORE_WITH_PG
    if (const char* ci = std::getenv("UNISTORE_PG_CONNINFO")) {
        std::string ci_str = ci;
        tfw::registry().push_back({"pg_entity_conformance", [ci_str] {
            PgEntityStore store(ci_str);
            run_entity_conformance(store);
        }});
    }
#endif
    // Redis 一致性测试：设置 UNISTORE_REDIS_ADDR="host:port" 后注册
    if (const char* addr = std::getenv("UNISTORE_REDIS_ADDR")) {
        std::string addr_str = addr;
        std::string prefix = "utest_et_" + std::to_string(
            (long long)std::chrono::steady_clock::now().time_since_epoch().count());
        tfw::registry().push_back({"redis_entity_conformance", [addr_str, prefix] {
            RedisEntityStore store(addr_str, prefix);
            run_entity_conformance(store);
        }});
    }
    return tfw::run_all();
}
