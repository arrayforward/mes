# ============================================================================
# mse —— 车间世界模型(制造执行系统,四原语:本体/事件/规则/视图)
#
# 依据 docs/mes 五份文档实现:两个动词(POST /events、GET /views/{viewId})、
# 四层校验+决策环、定义层四集合、投影引擎(属性多属)、时空树(voxel)、
# knowledge 协助(属性提取/规则协助推理)、entity 演化桥(非人力实体创建)。
# storage 是所有访问的底层接口(RecordBackend SPI)。
#
# 构建(Windows MinGW / Linux,CMake + Ninja):
#   cmake -S mse -B mse/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build mse/build -j
#   ctest --test-dir mse/build --output-on-failure
#   ./mse/build/mse_demo            # 汽车生产短流程端到端模拟验证
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(mse LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- 兄弟工程引入(相对路径,-D 可覆盖;binary dir 独立)----
set(MSE_STORAGE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/../storage"   CACHE PATH "storage 源码目录")
set(MSE_GEOCORE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/../geocore"   CACHE PATH "geocore 源码目录")
set(MSE_ENTITY_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/../entity"    CACHE PATH "entity 源码目录")
set(MSE_KNOWLEDGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../knowledge" CACHE PATH "knowledge 源码目录")
set(MSE_VOXEL_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/../voxel"     CACHE PATH "voxel 源码目录")

# knowledge 自带 add_subdirectory(storage)(其内部强制 UNISTORE_BUILD_TESTS=OFF),
# 故 unistore 由 knowledge 引入,mse 不再重复添加,避免目标重复。
set(KNOWLEDGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${MSE_KNOWLEDGE_DIR} ${CMAKE_BINARY_DIR}/knowledge EXCLUDE_FROM_ALL)

set(GEOCORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${MSE_GEOCORE_DIR} ${CMAKE_BINARY_DIR}/geocore EXCLUDE_FROM_ALL)

# entity 算法层(与 entity/CMakeLists.txt 同款定义;不 add_subdirectory(entity),
# 避免其再次引入 storage/geocore 造成目标重复)
add_library(entity_algo STATIC ${MSE_ENTITY_DIR}/src/resolver.cpp)
target_include_directories(entity_algo PUBLIC ${MSE_ENTITY_DIR}/include)
target_link_libraries(entity_algo PUBLIC unistore geocore)
target_compile_options(entity_algo PRIVATE -Wall -Wextra)

# ---- mse 测试注册(提前于 voxel 引入:add_test 的目标名在生成期解析,可先于
#      add_executable 注册) ----
enable_testing()
add_test(NAME mse_tests COMMAND mse_tests)
add_test(NAME mse_demo_smoke COMMAND mse_demo)

# voxel 时空记忆块服务(stmb);EXCLUDE_FROM_ALL 只按需构建 stmb_service 链。
# voxel 没有 BUILD_TESTS 开关且无条件注册测试,其测试目标又不参与构建(会变成
# Not Run)。宏作用域仅限此后引入的子目录——屏蔽其 add_test 注册,不影响
# voxel 独立构建与其自身 CI。
macro(add_test)
endmacro()
add_subdirectory(${MSE_VOXEL_DIR} ${CMAKE_BINARY_DIR}/voxel EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)

# ---- wasm3(WASM 规则沙盒引擎,third_party vendored,只用核心解释器:不启用
#      WASI/文件/环境能力——无环境沙盒纪律在引擎配置层就锁死) ----
add_library(wasm3 STATIC
    third_party/wasm3/source/m3_bind.c
    third_party/wasm3/source/m3_code.c
    third_party/wasm3/source/m3_compile.c
    third_party/wasm3/source/m3_core.c
    third_party/wasm3/source/m3_emit.c
    third_party/wasm3/source/m3_env.c
    third_party/wasm3/source/m3_exec.c
    third_party/wasm3/source/m3_function.c
    third_party/wasm3/source/m3_info.c
    third_party/wasm3/source/m3_module.c
    third_party/wasm3/source/m3_optimize.c
    third_party/wasm3/source/m3_parse.c
)
target_include_directories(wasm3 PUBLIC third_party/wasm3/source)

# ---- mse 核心库 ----
add_library(mse STATIC
    src/model.cpp
    src/event_log.cpp
    src/dictionary.cpp
    src/rules.cpp
    src/wasm_sandbox.cpp
    src/projection.cpp
    src/spacetime.cpp
    src/pipeline.cpp
    src/plc_filter.cpp
    src/view_engine.cpp
    src/api.cpp
    src/http_server.cpp
    src/knowledge_assist.cpp
    src/entity_bridge.cpp
    src/seeds.cpp
    src/seeds_wasm.cpp
    src/system.cpp
)
target_include_directories(mse PUBLIC include)
target_link_libraries(mse PUBLIC unistore knowledge entity_algo geocore stmb_service wasm3 Threads::Threads)
if(WIN32)
    target_link_libraries(mse PUBLIC ws2_32)  # HTTP 服务器/客户端
endif()
target_compile_options(mse PRIVATE -Wall -Wextra)

# MinGW 运行库静态链接,exe 可脱离 mingw64/bin 独立运行
set(MSE_STATIC_RUNTIME)
if(MINGW)
    set(MSE_STATIC_RUNTIME -static)  # 全静态(含 winpthread);ws2_32 等系统 DLL 仍为导入表
endif()

# ---- 汽车生产短流程模拟验证 ----
add_executable(mse_demo demo/sim_auto_flow.cpp)
target_link_libraries(mse_demo PRIVATE mse)
target_compile_options(mse_demo PRIVATE -Wall -Wextra)
target_link_options(mse_demo PRIVATE ${MSE_STATIC_RUNTIME})
target_compile_definitions(mse_demo PRIVATE
    MSE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    KNOWLEDGE_ASSETS_DIR="${MSE_KNOWLEDGE_DIR}/assets")

# ---- 人工验证用 HTTP 服务器(只读自省端点 + 静态资源) ----
add_executable(mse_server server/main.cpp)
target_link_libraries(mse_server PRIVATE mse)
target_compile_options(mse_server PRIVATE -Wall -Wextra)
target_link_options(mse_server PRIVATE ${MSE_STATIC_RUNTIME})
target_compile_definitions(mse_server PRIVATE
    MSE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    MSE_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web")

# ---- 测试 ----
add_executable(mse_tests tests/test_main.cpp)
target_link_libraries(mse_tests PRIVATE mse)
target_compile_options(mse_tests PRIVATE -Wall -Wextra)
target_link_options(mse_tests PRIVATE ${MSE_STATIC_RUNTIME})
target_compile_definitions(mse_tests PRIVATE
    MSE_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    KNOWLEDGE_ASSETS_DIR="${MSE_KNOWLEDGE_DIR}/assets")
