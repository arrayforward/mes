# ============================================================================
# 文件: src/bridge/CMakeLists.txt
# 模块: stmb_bridge_unistore —— stmb SourceRegistry <-> entitytree et_sources
#       双向共享桥(仅 STMB_WITH_UNISTORE=ON 时由顶层引入)。
# 说明: unistore 头文件要求 C++20(storage 工程标准),桥目标单独提标准,
#       与 stmb 主体(C++17)混链无碍(同一编译器)。
# ============================================================================
add_library(stmb_bridge_unistore STATIC unistore_source_bridge.cpp)
target_include_directories(stmb_bridge_unistore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(stmb_bridge_unistore PUBLIC stmb_service unistore)
set_target_properties(stmb_bridge_unistore PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
