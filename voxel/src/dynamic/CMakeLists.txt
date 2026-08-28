# ============================================================================
# 文件: src/dynamic/CMakeLists.txt
# 模块: stmb_dynamic —— 动态实例轨迹层
# 用途: 构建静态库 stmb_dynamic,公开链接 stmb_core,导出本目录头文件路径。
# 架构角色: 与 store / history 平级的独立部件,被 persistence(序列化)与
#           service 依赖;自身只依赖 core。
# ============================================================================
add_library(stmb_dynamic STATIC
    dynamic_layer.cpp
)

target_include_directories(stmb_dynamic PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(stmb_dynamic PUBLIC stmb_core)
