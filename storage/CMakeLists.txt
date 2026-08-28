cmake_minimum_required(VERSION 3.20)
project(unified_storage LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# vendored sqlite3（单线程使用，SQLITE_THREADSAFE=0 避免线程库依赖）
add_library(sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=0)

# 统一数据存储层 + 领域模型（事件树 eventstore、时空块 voxelstore、实体搜索树 entitytree）
add_library(unistore STATIC
    src/storage/memory_backend.cpp
    src/storage/sqlite_backend.cpp
    src/storage/file_backend.cpp
    src/storage/record_codec.cpp
    src/storage/redis/resp_client.cpp
    src/storage/redis_backend.cpp
    src/model.cpp
    src/event_store.cpp
    src/eventstore/record_event_store.cpp
    src/voxelstore/types.cpp
    src/voxelstore/voxel_store.cpp
    src/voxelstore/record_voxel_store.cpp
    src/entitytree/model.cpp
    src/entitytree/record_entity_store.cpp
)
target_include_directories(unistore PUBLIC include third_party)
target_link_libraries(unistore PUBLIC sqlite3)
if(WIN32)
    target_link_libraries(unistore PUBLIC ws2_32)  # redis 后端的 Winsock2
endif()
target_compile_options(unistore PRIVATE -Wall -Wextra)

# ---- 可选原生数据库后端（找到库才编译，不用 ODBC；Linux/Windows 均可） ----

option(UNISTORE_WITH_MYSQL "Build MySQL backend when libmysqlclient is found" ON)
option(UNISTORE_WITH_PG "Build PostgreSQL backend when libpq is found" ON)

if(UNISTORE_WITH_MYSQL)
    # Debian: libmysqlclient-dev 或 libmariadb-dev（/usr/include/mysql|mariadb）
    find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql mariadb)
    find_library(MYSQL_LIBRARY NAMES mysqlclient mariadb mariadbclient libmysql)
    if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
        target_sources(unistore PRIVATE src/storage/mysql_backend.cpp)
        target_include_directories(unistore PUBLIC ${MYSQL_INCLUDE_DIR})
        target_link_libraries(unistore PUBLIC ${MYSQL_LIBRARY})
        target_compile_definitions(unistore PUBLIC UNISTORE_WITH_MYSQL)
        message(STATUS "MySQL backend: enabled (${MYSQL_LIBRARY})")
    else()
        message(STATUS "MySQL backend: skipped (libmysqlclient not found)")
    endif()
endif()

if(UNISTORE_WITH_PG)
    # Debian: libpq-dev（/usr/include/postgresql）
    find_path(PG_INCLUDE_DIR NAMES libpq-fe.h PATH_SUFFIXES postgresql pgsql)
    find_library(PG_LIBRARY NAMES pq libpq)
    if(PG_INCLUDE_DIR AND PG_LIBRARY)
        target_sources(unistore PRIVATE src/storage/pg_backend.cpp)
        target_include_directories(unistore PUBLIC ${PG_INCLUDE_DIR})
        target_link_libraries(unistore PUBLIC ${PG_LIBRARY})
        target_compile_definitions(unistore PUBLIC UNISTORE_WITH_PG)
        message(STATUS "PostgreSQL backend: enabled (${PG_LIBRARY})")
    else()
        message(STATUS "PostgreSQL backend: skipped (libpq not found)")
    endif()
endif()

# MinGW 运行库静态链接，exe 可脱离 mingw64/bin 独立运行
set(EVENTSTORE_STATIC_RUNTIME -static-libgcc -static-libstdc++)

# 测试与 demo：独立构建默认 ON；作为子项目被 add_subdirectory 引入时
# （如 entity 组件）由上层置 OFF，避免注册未构建的可执行文件。
option(UNISTORE_BUILD_TESTS "Build unistore tests and demos" ON)
if(UNISTORE_BUILD_TESTS)

add_executable(eventstore_tests tests/test_main.cpp tests/conformance.cpp)
target_link_libraries(eventstore_tests PRIVATE unistore)
target_compile_options(eventstore_tests PRIVATE -Wall -Wextra)
target_link_options(eventstore_tests PRIVATE ${EVENTSTORE_STATIC_RUNTIME})

add_executable(voxel_tests tests/voxel_test_main.cpp tests/voxel_conformance.cpp)
target_link_libraries(voxel_tests PRIVATE unistore)
target_compile_options(voxel_tests PRIVATE -Wall -Wextra)
target_link_options(voxel_tests PRIVATE ${EVENTSTORE_STATIC_RUNTIME})

add_executable(entity_tests tests/entity_test_main.cpp tests/entity_conformance.cpp)
target_link_libraries(entity_tests PRIVATE unistore)
target_compile_options(entity_tests PRIVATE -Wall -Wextra)
target_link_options(entity_tests PRIVATE ${EVENTSTORE_STATIC_RUNTIME})

add_executable(demo_story examples/demo_story.cpp)
target_link_libraries(demo_story PRIVATE unistore)
target_link_options(demo_story PRIVATE ${EVENTSTORE_STATIC_RUNTIME})

add_executable(demo_voxel examples/demo_voxel.cpp)
target_link_libraries(demo_voxel PRIVATE unistore)
target_link_options(demo_voxel PRIVATE ${EVENTSTORE_STATIC_RUNTIME})

enable_testing()
add_test(NAME eventstore_conformance COMMAND eventstore_tests)
add_test(NAME voxel_conformance COMMAND voxel_tests)
add_test(NAME entity_conformance COMMAND entity_tests)

endif()  # UNISTORE_BUILD_TESTS
