# ============================================================================
# 文件: src/index/CMakeLists.txt
# 模块: stmb_index —— 索引库(空间格子索引 + 时间索引)
# 用途: 构建静态库 stmb_index,公开链接 stmb_core 以获得核心数据类型,
#       并把本目录头文件路径导出给 service 等上层模块。
# 架构角色: 中间层,依赖 core,被 service 依赖。
# ============================================================================
add_library(stmb_index STATIC
    spatial_grid.cpp
    temporal_index.cpp
)

target_include_directories(stmb_index PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(stmb_index PUBLIC stmb_core)
