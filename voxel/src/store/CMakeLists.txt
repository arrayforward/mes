# ============================================================================
# 文件: src/store/CMakeLists.txt
# 模块: stmb_store —— 存储库(容量上限 + LRU 淘汰)
# 用途: 构建静态库 stmb_store,公开链接 stmb_core;因内部使用 std::mutex,
#       链接 Threads::Threads 以保证 pthread 可用。
# 架构角色: 中间层,依赖 core,被 service 依赖。
# ============================================================================
add_library(stmb_store STATIC
    block_store.cpp
)

target_include_directories(stmb_store PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(stmb_store PUBLIC stmb_core PRIVATE Threads::Threads)
