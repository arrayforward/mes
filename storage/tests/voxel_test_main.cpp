#include <chrono>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

#include "test_framework.h"
#include "voxel_conformance.h"
#include "voxelstore/backends/file_voxel_store.h"
#include "voxelstore/backends/memory_voxel_store.h"
#include "voxelstore/backends/redis_voxel_store.h"
#include "voxelstore/backends/sql_voxel_store.h"
#ifdef UNISTORE_WITH_MYSQL
#include "voxelstore/backends/mysql_voxel_store.h"
#endif
#ifdef UNISTORE_WITH_PG
#include "voxelstore/backends/pg_voxel_store.h"
#endif

using namespace voxelstore;

TEST(memory_voxel_conformance) {
    MemoryVoxelStore store;
    run_voxel_conformance(store);
}

TEST(sqlite_voxel_conformance) {
    SqlVoxelStore store(":memory:");
    run_voxel_conformance(store);
}

TEST(file_voxel_conformance) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "voxelstore_test_file";
    fs::remove_all(dir);
    {
        FileVoxelStore store(dir.string());
        run_voxel_conformance(store);
    }
    fs::remove_all(dir);
}

TEST(file_voxel_persistence) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "voxelstore_test_file_persist";
    fs::remove_all(dir);
    {
        FileVoxelStore store(dir.string());
        VoxelBlock b;
        b.id = 7;
        b.region = Aabb{0, 0, 0, 5, 5, 5};
        b.payload = "文件持久化块";
        b.timestamp = 1000;
        b.state = "Stable";
        store.put_block(b);
        store.append_version(VoxelVersion{7, 1, "文件持久化块", 1.0, "Stable", 1000,
                                          std::nullopt});
        store.set_meta("next_id", 8);
    }
    {
        FileVoxelStore store(dir.string());  // 重新打开：schema + WAL 回放
        auto got = store.get_block(7);
        CHECK(got.has_value());
        CHECK_EQ(got->payload, "文件持久化块");
        CHECK(store.version_at(7, 1500).has_value());
        CHECK_EQ(store.get_meta("next_id", 0), (int64_t)8);
    }
    fs::remove_all(dir);
}

TEST(sqlite_voxel_file_persistence) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "voxelstore_test_persist";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto dbfile = (dir / "test.db").string();
    {
        SqlVoxelStore store(dbfile);
        VoxelBlock b;
        b.id = 7;
        b.region = Aabb{0, 0, 0, 5, 5, 5};
        b.payload = "持久化块";
        b.timestamp = 1000;
        b.state = "Stable";
        store.put_block(b);
        store.append_version(VoxelVersion{7, 1, "持久化块", 1.0, "Stable", 1000,
                                          std::nullopt});
        store.set_meta("next_id", 8);
    }
    {
        SqlVoxelStore store(dbfile);  // 重新打开，数据仍在
        auto got = store.get_block(7);
        CHECK(got.has_value());
        CHECK_EQ(got->payload, "持久化块");
        auto at = store.version_at(7, 1500);
        CHECK(at.has_value());
        CHECK_EQ(store.get_meta("next_id", 0), (int64_t)8);
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
        tfw::registry().push_back({"mysql_voxel_conformance", [dsn_str] {
            MySqlVoxelStore store(dsn_str);
            run_voxel_conformance(store);
        }});
    }
#endif
#ifdef UNISTORE_WITH_PG
    if (const char* ci = std::getenv("UNISTORE_PG_CONNINFO")) {
        std::string ci_str = ci;
        tfw::registry().push_back({"pg_voxel_conformance", [ci_str] {
            PgVoxelStore store(ci_str);
            run_voxel_conformance(store);
        }});
    }
#endif
    // Redis 一致性测试：设置 UNISTORE_REDIS_ADDR="host:port" 后注册
    if (const char* addr = std::getenv("UNISTORE_REDIS_ADDR")) {
        std::string addr_str = addr;
        std::string prefix = "utest_vx_" + std::to_string(
            (long long)std::chrono::steady_clock::now().time_since_epoch().count());
        tfw::registry().push_back({"redis_voxel_conformance", [addr_str, prefix] {
            RedisVoxelStore store(addr_str, prefix);
            run_voxel_conformance(store);
        }});
    }
    return tfw::run_all();
}
