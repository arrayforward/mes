# ============================================================================
# 文件: src/history/CMakeLists.txt
# 模块: stmb_history —— 版本历史库
# 用途: 构建静态库 stmb_history,公开链接 stmb_core 以获得核心数据类型,
#       并把本目录头文件路径导出给 service 等上层模块。
# 架构角色: 中间层,依赖 core,被 service 依赖;与 index / store 平级。
# ============================================================================
add_library(stmb_history STATIC
    version_log.cpp
)

target_include_directories(stmb_history PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(stmb_history PUBLIC stmb_core)
