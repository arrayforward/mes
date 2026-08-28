// ============================================================================
// check.hpp —— 极简测试框架（无外部依赖）
// ============================================================================
//
// 【本文件的实现思路】
// 参考实现刻意不引入 gtest 等第三方依赖：一个全局计数器 + 宏，失败时
// 打印 文件:行号 与表达式，main 末尾 REPORT() 汇总并以退出码报告结果。
// 用法：
//   CHECK(expr)                 —— 断言为真
//   CHECK_NEAR(a, b, tol)       —— 绝对误差
//   CHECK_REL(a, b, rtol)       —— 相对误差（精度测试 T-P 系列用）
// ============================================================================
#pragma once

#include <cmath>
#include <cstdio>

namespace geocore::test {

inline int& PassCount() { static int n = 0; return n; }
inline int& FailCount() { static int n = 0; return n; }

inline void Report(const char* suite) {
    std::printf("[%s] passed=%d failed=%d\n", suite, PassCount(), FailCount());
}

inline int ExitCode() { return FailCount() == 0 ? 0 : 1; }

} // namespace geocore::test

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (expr) {                                                        \
            ++::geocore::test::PassCount();                                \
        } else {                                                           \
            ++::geocore::test::FailCount();                                \
            std::printf("FAIL %s:%d: CHECK(%s)\n", __FILE__, __LINE__,     \
                        #expr);                                            \
        }                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                              \
    do {                                                                   \
        double _va = (a), _vb = (b);                                       \
        if (std::fabs(_va - _vb) <= (tol)) {                               \
            ++::geocore::test::PassCount();                                \
        } else {                                                           \
            ++::geocore::test::FailCount();                                \
            std::printf("FAIL %s:%d: CHECK_NEAR(%s, %s, %g): %g vs %g\n",  \
                        __FILE__, __LINE__, #a, #b, (double)(tol), _va,    \
                        _vb);                                              \
        }                                                                  \
    } while (0)

#define CHECK_REL(a, b, rtol)                                              \
    do {                                                                   \
        double _va = (a), _vb = (b);                                       \
        double _scale = std::fabs(_vb) > 1e-300 ? std::fabs(_vb) : 1.0;    \
        if (std::fabs(_va - _vb) <= (rtol) * _scale) {                     \
            ++::geocore::test::PassCount();                                \
        } else {                                                           \
            ++::geocore::test::FailCount();                                \
            std::printf("FAIL %s:%d: CHECK_REL(%s, %s, %g): %g vs %g\n",   \
                        __FILE__, __LINE__, #a, #b, (double)(rtol), _va,   \
                        _vb);                                              \
        }                                                                  \
    } while (0)

#define TEST_MAIN(suite_name)                                              \
    int main() {                                                           \
        RunAll();                                                          \
        ::geocore::test::Report(suite_name);                               \
        return ::geocore::test::ExitCode();                                \
    }
