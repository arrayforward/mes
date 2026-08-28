# ============================================================================
# 文件: src/vql/CMakeLists.txt
# 模块: stmb_vql —— VQL 查询语言 + 大模型 Function Calling 接口
# 用途: 构建静态库 stmb_vql(迷你 JSON / VQL 引擎 / 工具派发),
#       公开链接 stmb_service,导出本目录头文件路径。
# 架构角色: 最上层接口模块,依赖 service,被 app / tests 使用。
# ============================================================================
add_library(stmb_vql STATIC
    json.cpp
    vql.cpp
    functions.cpp
)

target_include_directories(stmb_vql PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(stmb_vql PUBLIC stmb_service)
