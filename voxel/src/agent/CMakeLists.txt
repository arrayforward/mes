# ============================================================================
# 文件: src/agent/CMakeLists.txt
# 模块: stmb_shell —— JSON Lines 协议外壳(大模型联调的 C++ 端)
# 用途: 构建可执行文件 stmb_shell,链接 stmb_vql(传递依赖 stmb_service),
#       供 Python 代理以子进程方式驱动。
# 架构角色: 联调架构的 C++ 端点,不参与库依赖图。
# ============================================================================
add_executable(stmb_shell
    shell_main.cpp
)

target_link_libraries(stmb_shell PRIVATE stmb_vql)
