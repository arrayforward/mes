# ============================================================================
# 文件: src/app/CMakeLists.txt
# 模块: stmb_demo —— 演示程序
# 用途: 构建可执行文件 stmb_demo,链接服务门面库 stmb_service
#       (其传递依赖会自动带入 index / store / core)。
# 架构角色: 最上层的演示入口,不参与库依赖图。
# ============================================================================
add_executable(stmb_demo
    main.cpp
)

target_link_libraries(stmb_demo PRIVATE stmb_service stmb_vql)
