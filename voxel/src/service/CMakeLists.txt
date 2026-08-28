# ============================================================================
# 文件: src/service/CMakeLists.txt
# 模块: stmb_service —— 服务门面库
# 用途: 构建静态库 stmb_service,公开链接 stmb_index 与 stmb_store
#       (进而传递依赖 stmb_core),并链接 Threads::Threads(std::mutex 需要)。
# 架构角色: 上层组合层,是 app / tests 唯一需要直接链接的业务库。
# ============================================================================
add_library(stmb_service STATIC
    stmb_service.cpp
)

target_include_directories(stmb_service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(stmb_service
    PUBLIC stmb_index stmb_store stmb_history stmb_persistence stmb_shard
    PRIVATE Threads::Threads
)
