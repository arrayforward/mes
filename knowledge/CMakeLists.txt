cmake_minimum_required(VERSION 3.20)
project(knowledge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 只读消费 storage 事件侧写系统（unistore）；nlohmann/json 复用其 third_party。
# 作为子项目引入，关闭 storage 自身的测试与 demo。
set(UNISTORE_BUILD_TESTS OFF CACHE BOOL "Build unistore tests and demos" FORCE)
add_subdirectory(D:/manufacture/storage ${CMAKE_BINARY_DIR}/storage)

# vendored ame 子集（UNMODIFIED，见 cmake/ame_mingw_compat.h 的兼容性说明）：
# 词汇-联想层（别名归一/近义词表/RELATED 边/扩散）。逐模块引入，各自独立二进制目录。
foreach(m core storage time vector keyword diffusion)
    add_subdirectory(ame/${m} ${CMAKE_BINARY_DIR}/ame/${m})
endforeach()

# MinGW(ucrt) 无 gmtime_r：以强制包含 shim 的方式修补（ame 文件保持字节不变）。
set(AME_MINGW_COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ame_mingw_compat.h")
foreach(t ame_core ame_storage ame_time ame_vector ame_keyword ame_diffusion)
    target_compile_options(${t} PRIVATE -include "${AME_MINGW_COMPAT}")
endforeach()

# 语义图谱树：概念/类型/推理规则库（"是什么？意味着什么？"）
add_library(knowledge STATIC
    src/types.cpp
    src/conduction.cpp
    src/lexicon.cpp
    src/ontology.cpp
    src/ingest.cpp
    src/diffusion.cpp
    src/reasoner.cpp
    src/engine.cpp
    src/ame_lexicon.cpp
)
target_include_directories(knowledge PUBLIC include)
# unistore 传递 nlohmann/json 与 sqlite3；ame_keyword/ame_diffusion 传递 ame 头文件
target_link_libraries(knowledge PUBLIC unistore ame_keyword ame_diffusion)
target_compile_options(knowledge PRIVATE -Wall -Wextra)

# MinGW 运行库静态链接，exe 可脱离 mingw64/bin 独立运行（与 storage 对齐）
set(KNOWLEDGE_STATIC_RUNTIME -static-libgcc -static-libstdc++)

option(KNOWLEDGE_BUILD_TESTS "Build knowledge tests and demos" ON)
if(KNOWLEDGE_BUILD_TESTS)

# 资产与 demo 数据库的绝对路径，编进测试/示例，免 WorkingDirectory 依赖
set(KNOWLEDGE_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(KNOWLEDGE_DEMO_DB "D:/manufacture/storage/demo_story.db")
set(KNOWLEDGE_AME_LEXICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ame/lexicon")

foreach(t types lexicon ontology ingest diffusion reasoner e2e)
    add_executable(${t}_tests tests/${t}_tests.cpp)
    target_link_libraries(${t}_tests PRIVATE knowledge)
    target_compile_options(${t}_tests PRIVATE -Wall -Wextra)
    target_link_options(${t}_tests PRIVATE ${KNOWLEDGE_STATIC_RUNTIME})
    target_compile_definitions(${t}_tests PRIVATE
        KNOWLEDGE_ASSETS_DIR="${KNOWLEDGE_ASSETS_DIR}"
        KNOWLEDGE_DEMO_DB="${KNOWLEDGE_DEMO_DB}"
        KNOWLEDGE_AME_LEXICON_DIR="${KNOWLEDGE_AME_LEXICON_DIR}")
endforeach()

add_executable(demo_understand examples/demo_understand.cpp)
target_link_libraries(demo_understand PRIVATE knowledge)
target_link_options(demo_understand PRIVATE ${KNOWLEDGE_STATIC_RUNTIME})
target_compile_definitions(demo_understand PRIVATE
    KNOWLEDGE_ASSETS_DIR="${KNOWLEDGE_ASSETS_DIR}"
    KNOWLEDGE_DEMO_DB="${KNOWLEDGE_DEMO_DB}"
    KNOWLEDGE_AME_LEXICON_DIR="${KNOWLEDGE_AME_LEXICON_DIR}")

enable_testing()
foreach(t types lexicon ontology ingest diffusion reasoner e2e)
    add_test(NAME ${t} COMMAND ${t}_tests)
endforeach()

# vendored ame 的六个模块测试（不 add_subdirectory ame/tests：其 CMakeLists 依赖未
# 拷贝的模块；且各测试仅需自身模块库）。测试名为 ame_<t> 避免与 knowledge 测试撞名。
set(AME_TEST_LINK_test_core ame_core)
set(AME_TEST_LINK_test_storage ame_storage)
set(AME_TEST_LINK_test_time ame_time)
set(AME_TEST_LINK_test_vector ame_vector)
set(AME_TEST_LINK_test_keyword ame_keyword)
set(AME_TEST_LINK_test_diffusion ame_diffusion)
foreach(t test_core test_storage test_time test_vector test_keyword test_diffusion)
    add_executable(ame_${t} ame/tests/${t}.cpp)
    target_include_directories(ame_${t} PRIVATE ame/tests)
    target_link_libraries(ame_${t} PRIVATE ${AME_TEST_LINK_${t}})
    target_link_options(ame_${t} PRIVATE ${KNOWLEDGE_STATIC_RUNTIME})
    add_test(NAME ame_${t} COMMAND ame_${t})
endforeach()

endif()  # KNOWLEDGE_BUILD_TESTS
