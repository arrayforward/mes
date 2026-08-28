# ============================================================================
# 文件: tests/CMakeLists.txt
# 模块: stmb_tests —— 测试总入口
# 用途: 按「各模块独立单元测试 + 一个整体端到端测试」组织测试目标:
#         - test_types          core   模块单测(只链接 stmb_core)
#         - test_spatial_grid   index  模块单测(只链接 stmb_index)
#         - test_temporal_index index  模块单测(只链接 stmb_index)
#         - test_block_store    store  模块单测(只链接 stmb_store)
#         - test_version_log    history 模块单测(只链接 stmb_history)
#         - test_stmb_service   service 模块单测(链接 stmb_service)
#         - test_e2e            端到端测试(链接 stmb_service)
#       每个可执行文件单独 add_test 注册到 CTest,可独立运行、独立定位失败。
# 架构角色: 测试编排,由顶层 enable_testing() 统一启用。
# ============================================================================

# core 模块单测
add_executable(test_types core/test_types.cpp)
target_link_libraries(test_types PRIVATE stmb_core)
add_test(NAME test_types COMMAND test_types)

# index 模块单测(空间索引 / 时间索引)
add_executable(test_spatial_grid index/test_spatial_grid.cpp)
target_link_libraries(test_spatial_grid PRIVATE stmb_index)
add_test(NAME test_spatial_grid COMMAND test_spatial_grid)

add_executable(test_temporal_index index/test_temporal_index.cpp)
target_link_libraries(test_temporal_index PRIVATE stmb_index)
add_test(NAME test_temporal_index COMMAND test_temporal_index)

# store 模块单测
add_executable(test_block_store store/test_block_store.cpp)
target_link_libraries(test_block_store PRIVATE stmb_store)
add_test(NAME test_block_store COMMAND test_block_store)

# history 模块单测
add_executable(test_version_log history/test_version_log.cpp)
target_link_libraries(test_version_log PRIVATE stmb_history)
add_test(NAME test_version_log COMMAND test_version_log)

# persistence 模块单测(含服务恢复端到端)
add_executable(test_persistence persistence/test_persistence.cpp)
target_link_libraries(test_persistence PRIVATE stmb_service stmb_persistence)
add_test(NAME test_persistence COMMAND test_persistence)

# shard 模块单测(分片归属/延迟加载/换出/重启恢复/海量 smoke)
add_executable(test_shard shard/test_shard.cpp)
target_link_libraries(test_shard PRIVATE stmb_service stmb_shard stmb_persistence)
add_test(NAME test_shard COMMAND test_shard)

# LOD 模块单测(层级归属/分层查询/聚合上卷/未细化检测/分片组合)
add_executable(test_lod lod/test_lod.cpp)
target_link_libraries(test_lod PRIVATE stmb_service)
add_test(NAME test_lod COMMAND test_lod)

# dynamic 模块单测(轨迹/跨源关联/状态推进/沉淀/临时占用/持久化)
add_executable(test_dynamic dynamic/test_dynamic.cpp)
target_link_libraries(test_dynamic PRIVATE stmb_service stmb_dynamic)
add_test(NAME test_dynamic COMMAND test_dynamic)

# arbitration 模块单测(变化分类/周期模式/加权仲裁/观察窗口/空间一致性/持久化)
add_executable(test_arbitration arbitration/test_arbitration.cpp)
target_link_libraries(test_arbitration PRIVATE stmb_service)
add_test(NAME test_arbitration COMMAND test_arbitration)

# vql 模块单测(迷你 JSON/VQL 解析与执行/Function Calling)
add_executable(test_vql vql/test_vql.cpp)
target_link_libraries(test_vql PRIVATE stmb_vql)
add_test(NAME test_vql COMMAND test_vql)

# agent 模块单测(stmb_shell JSON Lines 协议,离线;依赖 fork/wait,仅 POSIX 注册)
if(NOT WIN32)
add_executable(test_shell agent/test_shell.cpp)
target_link_libraries(test_shell PRIVATE stmb_vql)
add_test(NAME test_shell COMMAND test_shell)
endif()

# service 模块单测
add_executable(test_stmb_service service/test_stmb_service.cpp)
target_link_libraries(test_stmb_service PRIVATE stmb_service)
add_test(NAME test_stmb_service COMMAND test_stmb_service)

# 端到端测试
add_executable(test_e2e e2e/test_e2e.cpp)
target_link_libraries(test_e2e PRIVATE stmb_service stmb_vql)
add_test(NAME test_e2e COMMAND test_e2e)

# et_sources 双向桥单测(仅 STMB_WITH_UNISTORE=ON 时构建)
if(STMB_WITH_UNISTORE)
add_executable(test_source_bridge bridge/test_source_bridge.cpp)
target_link_libraries(test_source_bridge PRIVATE stmb_bridge_unistore)
set_target_properties(test_source_bridge PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
add_test(NAME test_source_bridge COMMAND test_source_bridge)
endif()
