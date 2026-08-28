# ============================================================================
# bench —— mechanical-heart 心跳 tick 基准测试
# 模拟一拍心跳：观测 → entity 浮现 → geocore 锚定 → eventstore 存证
#              → voxelstore 时空块 → knowledge 语义理解
# 构建（Windows MinGW / Linux，CMake + Ninja）：
#   cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build bench/build -j
#   ./bench/build/tick_bench --ticks=1000 --backend=memory
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mechanical_heart_bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 兄弟工程引入（相对路径，-D 可覆盖；binary dir 独立）----
set(BENCH_STORAGE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/../storage"   CACHE PATH "storage 源码目录")
set(BENCH_GEOCORE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/../geocore"   CACHE PATH "geocore 源码目录")
set(BENCH_ENTITY_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/../entity"    CACHE PATH "entity 源码目录")
set(BENCH_KNOWLEDGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../knowledge" CACHE PATH "knowledge 源码目录")

# knowledge 自带 add_subdirectory(storage)（其 CMakeLists 内部强制 UNISTORE_BUILD_TESTS=OFF），
# 故 storage 由 knowledge 引入，bench 不再重复添加，避免目标重复。
set(KNOWLEDGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)  # knowledge 测试由其自带 build 负责
add_subdirectory(${BENCH_KNOWLEDGE_DIR} ${CMAKE_BINARY_DIR}/knowledge EXCLUDE_FROM_ALL)

set(GEOCORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)    # geocore 测试同理
add_subdirectory(${BENCH_GEOCORE_DIR} ${CMAKE_BINARY_DIR}/geocore EXCLUDE_FROM_ALL)

# ---- entity 算法层（与 entity/CMakeLists.txt 同款定义；
#      不 add_subdirectory(entity)，避免其再次引入 storage/geocore 造成目标重复）----
add_library(entity_algo STATIC ${BENCH_ENTITY_DIR}/src/resolver.cpp)
target_include_directories(entity_algo PUBLIC ${BENCH_ENTITY_DIR}/include)
target_link_libraries(entity_algo PUBLIC unistore geocore)
target_compile_options(entity_algo PRIVATE -Wall -Wextra)

# ---- tick 基准可执行文件 ----
add_executable(tick_bench tick_bench.cpp)
target_link_libraries(tick_bench PRIVATE entity_algo knowledge)
target_compile_options(tick_bench PRIVATE -Wall -Wextra)
target_compile_definitions(tick_bench PRIVATE
    KNOWLEDGE_ASSETS_DIR="${BENCH_KNOWLEDGE_DIR}/assets"
    KNOWLEDGE_AME_LEXICON_DIR="${BENCH_KNOWLEDGE_DIR}/ame/lexicon")

# MinGW 运行库静态链接，exe 可脱离 mingw64/bin 独立运行
if(MINGW)
    target_link_options(tick_bench PRIVATE -static-libgcc -static-libstdc++)
endif()

enable_testing()
add_test(NAME tick_bench_smoke COMMAND tick_bench --ticks=200 --warmup=20 --backend=memory)
