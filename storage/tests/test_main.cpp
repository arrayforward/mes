#include <chrono>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

#include "conformance.h"
#include "eventstore/backends/file_store.h"
#include "eventstore/backends/memory_store.h"
#include "eventstore/backends/redis_store.h"
#include "eventstore/backends/sql_store.h"
#ifdef UNISTORE_WITH_MYSQL
#include "eventstore/backends/mysql_store.h"
#endif
#ifdef UNISTORE_WITH_PG
#include "eventstore/backends/pg_store.h"
#endif
#include "test_framework.h"

using namespace eventstore;

TEST(memory_conformance) {
    MemoryStore store;
    run_conformance_suite(store);
}

TEST(sqlite_memory_conformance) {
    SqlStore store(":memory:");
    run_conformance_suite(store);
}

TEST(file_conformance) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "eventstore_test_file";
    fs::remove_all(dir);
    {
        FileStore store(dir.string());
        run_conformance_suite(store);
    }
    fs::remove_all(dir);
}

TEST(file_persistence) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "eventstore_test_file_persist";
    fs::remove_all(dir);
    std::string profile_id;
    std::string narrative_id;
    int64_t seq1 = 0;
    {
        FileStore store(dir.string());
        auto na = Narrative::create("fact", "文件持久化");
        store.append_narrative(na);
        narrative_id = na.narrative_id;
        auto ev = Event::create(na.narrative_id, "落笔");
        store.append_event(ev);
        auto p = Profile::create(ev.event_id, "active", TimeRef::real("2026-08-27T10:00:00Z"),
                                 "北京", "张三", "签署", "合同");
        seq1 = store.append_profile(p);
        profile_id = p.profile_id;
    }
    {
        FileStore store(dir.string());  // 重新打开：schema + WAL 回放
        auto got = store.get_profile(profile_id);
        CHECK_EQ(got.subject, "张三");
        CHECK_EQ(got.time.kind, "real");
        // 回放下 seq 继续递增
        auto ev2 = Event::create(narrative_id, "第二笔");
        store.append_event(ev2);
        auto p2 = Profile::create(ev2.event_id, "active", TimeRef::real("2026-08-27T11:00:00Z"),
                                  "北京", "张三", "签署", "补充协议");
        int64_t seq2 = store.append_profile(p2);
        CHECK(seq2 > seq1);
    }
    fs::remove_all(dir);
}

TEST(sqlite_file_persistence) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "eventstore_test_persist";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto dbfile = (dir / "test.db").string();
    std::string profile_id;
    std::string entity_id;
    {
        SqlStore store(dbfile);
        auto na = Narrative::create("fact", "持久化测试");
        store.append_narrative(na);
        auto ev = Event::create(na.narrative_id, "签署合同");
        store.append_event(ev);
        auto p = Profile::create(ev.event_id, "active", TimeRef::real("2026-08-27T10:00:00Z"),
                                 "北京", "张三", "签署", "合同");
        store.append_profile(p);
        profile_id = p.profile_id;
        auto e = Entity::create("张三", "person");
        store.append_entity(e);
        entity_id = e.entity_id;
        store.append_binding(Binding::create(profile_id, "subject", entity_id));
    }
    {
        SqlStore store(dbfile);  // 重新打开，数据仍在
        auto got = store.get_profile(profile_id);
        CHECK_EQ(got.subject, "张三");
        CHECK_EQ(got.time.kind, "real");
        auto b = store.effective_binding(profile_id, "subject");
        CHECK(b && b->entity_id == entity_id);
    }
    fs::remove_all(dir);
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // 让中文输出不乱码
#endif
    // MySQL / Postgres 一致性测试：设置环境变量后才注册
    //   UNISTORE_MYSQL_DSN="mysql://user:pass@host:3306/dbname"
    //   UNISTORE_PG_CONNINFO="host=... user=... password=... dbname=..."
#ifdef UNISTORE_WITH_MYSQL
    if (const char* dsn = std::getenv("UNISTORE_MYSQL_DSN")) {
        std::string dsn_str = dsn;
        tfw::registry().push_back({"mysql_conformance", [dsn_str] {
            MySqlStore store(dsn_str);
            run_conformance_suite(store);
        }});
    }
#endif
#ifdef UNISTORE_WITH_PG
    if (const char* ci = std::getenv("UNISTORE_PG_CONNINFO")) {
        std::string ci_str = ci;
        tfw::registry().push_back({"pg_conformance", [ci_str] {
            PgStore store(ci_str);
            run_conformance_suite(store);
        }});
    }
#endif
    // Redis 一致性测试：设置 UNISTORE_REDIS_ADDR="host:port" 后注册
    // （每次运行用随机 key 前缀隔离，不污染服务器上的既有数据）
    if (const char* addr = std::getenv("UNISTORE_REDIS_ADDR")) {
        std::string addr_str = addr;
        std::string prefix = "utest_es_" + std::to_string(
            (long long)std::chrono::steady_clock::now().time_since_epoch().count());
        tfw::registry().push_back({"redis_conformance", [addr_str, prefix] {
            RedisStore store(addr_str, prefix);
            run_conformance_suite(store);
        }});
    }
    return tfw::run_all();
}
