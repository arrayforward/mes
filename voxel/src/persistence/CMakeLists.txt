# ============================================================================
# 文件: src/persistence/CMakeLists.txt
# 模块: stmb_persistence —— 本地文件持久化库(快照 + WAL)
# 用途: 构建静态库 stmb_persistence,公开链接 stmb_history(传递依赖
#       stmb_core,且需要 BlockVersion 类型),并导出本目录头文件路径。
# 架构角色: 中间层,依赖 core / history,被 service 依赖。
# ============================================================================
add_library(stmb_persistence STATIC
    persistence.cpp
)

target_include_directories(stmb_persistence PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(stmb_persistence PUBLIC stmb_history stmb_dynamic)
