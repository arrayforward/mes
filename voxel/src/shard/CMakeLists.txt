# ============================================================================
# 文件: src/shard/CMakeLists.txt
# 模块: stmb_shard —— 分片管理库
# 用途: 构建静态库 stmb_shard,公开链接 stmb_persistence(传递依赖
#       stmb_history / stmb_core),并导出本目录头文件路径。
# 架构角色: 中间层,依赖 core / history / persistence,被 service 依赖。
# ============================================================================
add_library(stmb_shard STATIC
    shard_manager.cpp
)

target_include_directories(stmb_shard PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(stmb_shard PUBLIC stmb_persistence)
