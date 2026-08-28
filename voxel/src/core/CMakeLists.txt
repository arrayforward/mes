# ============================================================================
# 文件: src/core/CMakeLists.txt
# 模块: stmb_core —— 核心数据类型库
# 用途: 把 types.cpp 构建为静态库 stmb_core,并把本目录作为 PUBLIC 头文件路径
#       导出,使 index / store / service 等上层模块可以直接 #include "types.h"。
# 架构角色: 全工程最底层的模块,自身不依赖任何其他 stmb 模块。
# ============================================================================
add_library(stmb_core STATIC
    types.cpp
)

target_include_directories(stmb_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
