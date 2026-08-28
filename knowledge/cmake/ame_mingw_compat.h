// ============================================================================
// MinGW 兼容 shim（强制包含 -include，仅作用于 vendored ame 目标）。
// 背景：vendored ame 源码不可修改，而 ucrt MinGW 与 ame 原工具链存在差异：
//   1. 无 POSIX gmtime_r（ame/time/src/time_service.cpp:74）——ucrt 提供
//      参数对调的 gmtime_s，此处包装出 POSIX 语义；
//   2. 严格 -std=c++20 下 <cstdint> 不再被其它头传递包含（json.cpp/uid.h
//      依赖 uint32_t/uint64_t 的传递包含）——此处显式引入；
//   3. 严格标准模式下 <cmath> 不定义 M_PI（storage.cpp 的 haversine 用）——
//      此处按 POSIX 惯例补齐。
// 用法：target_compile_options(<ame_target> PRIVATE -include <本文件路径>)。
// ============================================================================
#pragma once

#include <cstdint>
#include <ctime>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 用 gmtime_s 包装出 gmtime_r 的 POSIX 语义（成功返回 out，失败返回 nullptr）。
static inline struct tm* gmtime_r(const std::time_t* t, struct tm* out) {
    return gmtime_s(out, t) == 0 ? out : nullptr;
}
