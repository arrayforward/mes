# ============================================================================
# 文件: CMakeLists.txt(顶层)
# 项目: stmb —— 时空记忆块服务(Spatio-Temporal Memory Block Service)
# 用途: 顶层构建脚本,统一定义 C++ 标准、编译选项,并把各模块子目录纳入构建。
# 设计思路:
#   1. 全工程强制 C++17,关闭编译器扩展,保证可移植性;
#   2. 统一开启 -Wall -Wextra(MSVC 用 /W4),要求零警告;
#   3. 按 src/core -> src/index -> src/store -> src/service -> src/app 的顺序
#      逐层 add_subdirectory,模块之间以静态库 + target_link_libraries 表达依赖;
#   4. enable_testing() 开启 CTest,测试目标在 tests/ 中注册。
# 架构角色: 构建系统的根,只做全局设置与子目录编排,不包含任何源码。
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(stmb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# MinGW 运行库静态链接,exe 可脱离 mingw64/bin 独立运行(与 storage 同一纪律)
if(MINGW)
    add_link_options(-static-libgcc -static-libstdc++)
endif()

find_package(Threads REQUIRED)

# ---- 可选:entitytree et_sources 双向桥(默认 OFF,保持零第三方依赖) ----
# 开启:cmake -DSTMB_WITH_UNISTORE=ON [-DUNISTORE_DIR=/path/to/storage]
option(STMB_WITH_UNISTORE "Build et_sources bridge against unistore (entitytree)" OFF)
if(STMB_WITH_UNISTORE)
    set(UNISTORE_DIR "D:/manufacture/storage" CACHE PATH "unistore(storage) 源码目录")
    enable_language(C)  # unistore 携带 vendored sqlite3(C 源)
    set(UNISTORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)  # storage 测试由其自带 build 跑
    add_subdirectory(${UNISTORE_DIR} ${CMAKE_BINARY_DIR}/unistore EXCLUDE_FROM_ALL)
    add_subdirectory(src/bridge)
endif()

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/index)
add_subdirectory(src/store)
add_subdirectory(src/history)
add_subdirectory(src/dynamic)
add_subdirectory(src/persistence)
add_subdirectory(src/shard)
add_subdirectory(src/service)
add_subdirectory(src/vql)
add_subdirectory(src/agent)
add_subdirectory(src/app)
add_subdirectory(tests)
