#include <chrono>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

#include "record_conformance.h"
#include "storage/backends/file_backend.h"
#include "storage/backends/memory_backend.h"
#include "storage/backends/redis_backend.h"
#include "storage/backends/sqlite_backend.h"
#ifdef UNISTORE_WITH_MYSQL
#include "storage/backends/mysql_backend.h"
#endif
#ifdef UNISTORE_WITH_PG
#include "storage/backends/pg_backend.h"
#endif
#include "test_framework.h"

using namespace storage;

TEST(memory_record_conformance) {
    MemoryBackend backend;
    run_record_conformance(backend);
}

TEST(sqlite_record_conformance) {
    SqliteBackend backend(":memory:");
    run_record_conformance(backend);
}

TEST(file_record_conformance) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "unistore_test_record_file";
    fs::remove_all(dir);
    {
        FileBackend backend(dir.string());
        run_record_conformance(backend);
    }
    fs::remove_all(dir);
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // 让中文输出不乱码
#endif
    // MySQL / Postgres / Redis 一致性测试：设置环境变量后才注册（同 eventstore_tests）
#ifdef UNISTORE_WITH_MYSQL
    if (const char* dsn = std::getenv("UNISTORE_MYSQL_DSN")) {
        std::string dsn_str = dsn;
        tfw::registry().push_back({"mysql_record_conformance", [dsn_str] {
            MySqlBackend backend(dsn_str);
            run_record_conformance(backend);
        }});
    }
#endif
#ifdef UNISTORE_WITH_PG
    if (const char* ci = std::getenv("UNISTORE_PG_CONNINFO")) {
        std::string ci_str = ci;
        tfw::registry().push_back({"pg_record_conformance", [ci_str] {
            PgBackend backend(ci_str);
            run_record_conformance(backend);
        }});
    }
#endif
    if (const char* addr = std::getenv("UNISTORE_REDIS_ADDR")) {
        std::string addr_str = addr;
        std::string prefix = "utest_rec_" + std::to_string(
            (long long)std::chrono::steady_clock::now().time_since_epoch().count());
        tfw::registry().push_back({"redis_record_conformance", [addr_str, prefix] {
            RedisBackend backend(addr_str, prefix);
            run_record_conformance(backend);
        }});
    }
    return tfw::run_all();
}
