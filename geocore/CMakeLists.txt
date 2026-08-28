# ============================================================================
# geocore —— 空间几何内核（参考实现，规范《空间几何内核：算法与架构说明书》v1.0）
#
# 构建（WSL / Linux）：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ctest --test-dir build --output-on-failure
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(geocore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)           # std::variant / shared_mutex / optional
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 内核静态库 ----
add_library(geocore STATIC
    src/anchor_tree.cpp
    src/scene.cpp
    src/kernel.cpp
)
target_include_directories(geocore PUBLIC include)
target_compile_options(geocore PRIVATE -Wall -Wextra)

# ---- 线程库（并发模型：shared_mutex / 多线程测试）----
find_package(Threads REQUIRED)
target_link_libraries(geocore PUBLIC Threads::Threads)

# ---- 测试 ----
# 作为子项目被 add_subdirectory 引入时（如 storage），上层可置
# GEOCORE_BUILD_TESTS=OFF 跳过测试目标，避免注册未构建的可执行文件。
option(GEOCORE_BUILD_TESTS "Build geocore tests" ON)
if(GEOCORE_BUILD_TESTS)
    enable_testing()

    add_executable(unit_tests tests/unit_tests.cpp)
    target_link_libraries(unit_tests PRIVATE geocore)
    add_test(NAME unit_tests COMMAND unit_tests)

    add_executable(e2e_tests tests/e2e_tests.cpp)
    target_link_libraries(e2e_tests PRIVATE geocore)
    add_test(NAME e2e_tests COMMAND e2e_tests)
endif()
