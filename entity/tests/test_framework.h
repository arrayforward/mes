#pragma once

// 极简测试框架：TEST 注册、CHECK/CHECK_EQ 断言、run_all 执行。
// 零依赖，约 60 行。

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tfw {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

inline int& failure_count() { static int c = 0; return c; }
inline int& check_count() { static int c = 0; return c; }

inline int run_all() {
    int failed_tests = 0;
    for (auto& tc : registry()) {
        int before = failure_count();
        try {
            tc.fn();
        } catch (const std::exception& e) {
            ++failure_count();
            std::printf("  [UNCAUGHT] %s\n", e.what());
        }
        if (failure_count() == before) {
            std::printf("[PASS] %s\n", tc.name.c_str());
        } else {
            ++failed_tests;
            std::printf("[FAIL] %s\n", tc.name.c_str());
        }
    }
    std::printf("---\nchecks: %d, failures: %d, failed tests: %d/%zu\n",
                check_count(), failure_count(), failed_tests, registry().size());
    return failed_tests == 0 ? 0 : 1;
}

} // namespace tfw

#define TEST(name) \
    static void test_fn_##name(); \
    static tfw::Registrar reg_##name(#name, test_fn_##name); \
    static void test_fn_##name()

#define CHECK(cond) \
    do { \
        ++tfw::check_count(); \
        if (!(cond)) { \
            ++tfw::failure_count(); \
            std::printf("  [CHECK FAILED] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        ++tfw::check_count(); \
        auto _va = (a); \
        auto _vb = (b); \
        if (!(_va == _vb)) { \
            ++tfw::failure_count(); \
            std::printf("  [CHECK FAILED] %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        } \
    } while (0)

/// 期望抛出异常类型 ExType 的断言。
#define CHECK_THROWS_AS(stmt, ExType) \
    do { \
        ++tfw::check_count(); \
        bool _thrown = false; \
        try { stmt; } catch (const ExType&) { _thrown = true; } \
        if (!_thrown) { \
            ++tfw::failure_count(); \
            std::printf("  [CHECK FAILED] %s:%d: expected %s from %s\n", \
                        __FILE__, __LINE__, #ExType, #stmt); \
        } \
    } while (0)
