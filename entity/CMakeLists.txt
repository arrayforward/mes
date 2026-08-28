# ============================================================================
# entity —— 实体搜索树的算法与变化逻辑层
#
# 职责划分：本组件 = 算法（浮现/合并/拆分/检索排序/时效衰减/来源可靠性闭环）；
# storage（entitytree）= 增删改查；geocore = 空间几何（锚点路径码邻近）。
#
# 构建（Windows MinGW / Linux，CMake + Ninja）：
#   cmake -B build -G Ninja
#   cmake --build build
#   ctest --test-dir build
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(entity LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 兄弟工程引入（本地绝对路径，-D 可覆盖；binary dir 独立） ----
set(ENTITY_STORAGE_DIR "D:/manufacture/storage" CACHE PATH "storage 源码目录")
set(ENTITY_GEOCORE_DIR "D:/manufacture/geocore" CACHE PATH "geocore 源码目录")

set(UNISTORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)  # storage 测试由其自带 build 跑
add_subdirectory(${ENTITY_STORAGE_DIR} ${CMAKE_BINARY_DIR}/storage EXCLUDE_FROM_ALL)

set(GEOCORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)   # geocore 测试同理
add_subdirectory(${ENTITY_GEOCORE_DIR} ${CMAKE_BINARY_DIR}/geocore EXCLUDE_FROM_ALL)

# ---- 算法层静态库 ----
add_library(entity_algo STATIC src/resolver.cpp)
target_include_directories(entity_algo PUBLIC include)
target_link_libraries(entity_algo PUBLIC unistore geocore)
target_compile_options(entity_algo PRIVATE -Wall -Wextra)

# MinGW 运行库静态链接，exe 可脱离 mingw64/bin 独立运行
set(ENTITY_STATIC_RUNTIME -static-libgcc -static-libstdc++)

add_executable(entity_tests tests/resolver_test_main.cpp tests/resolver_conformance.cpp)
target_link_libraries(entity_tests PRIVATE entity_algo)
target_compile_options(entity_tests PRIVATE -Wall -Wextra)
target_link_options(entity_tests PRIVATE ${ENTITY_STATIC_RUNTIME})

add_executable(demo_entity examples/demo_entity.cpp)
target_link_libraries(demo_entity PRIVATE entity_algo)
target_link_options(demo_entity PRIVATE ${ENTITY_STATIC_RUNTIME})

enable_testing()
add_test(NAME entity_resolver COMMAND entity_tests)
