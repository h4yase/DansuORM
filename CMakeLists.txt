cmake_minimum_required(VERSION 3.24)

project(dansusql LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version" FORCE)
set(GODOTCPP_USE_STATIC_CPP ON CACHE BOOL "Link the MSVC runtime statically" FORCE)
set(GODOTCPP_ENABLE_TESTING OFF CACHE BOOL "Disable godot-cpp tests" FORCE)
set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "Treat godot-cpp includes as system headers" FORCE)

add_subdirectory(third_party/godot-cpp)

add_library(sqlite3 STATIC
    third_party/sqlite/sqlite3.c
)

target_include_directories(sqlite3 PUBLIC
    third_party/sqlite
)

target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_DQS=0
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_MATH_FUNCTIONS
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
    SQLITE_USE_URI=1
)

if(MSVC)
    set_property(TARGET sqlite3 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(sqlite3 PRIVATE /W0)
endif()

add_library(dansusql SHARED
    src/dansu_db.cpp
    src/dansu_db_domain.cpp
    src/register_types.cpp
)

target_include_directories(dansusql PRIVATE
    src
)

target_link_libraries(dansusql PRIVATE
    godot::cpp
    sqlite3
)

target_compile_definitions(dansusql PRIVATE
    DANSUSQL_SQLITE_VERSION="3.53.3"
)

if(MSVC)
    set_property(TARGET dansusql PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(dansusql PRIVATE /W3 /permissive-)
else()
    target_compile_options(dansusql PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(DANSUSQL_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/demo/bin")
set_target_properties(dansusql PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dansusql.windows.${GODOTCPP_TARGET}.x86_64"
    RUNTIME_OUTPUT_DIRECTORY "${DANSUSQL_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${DANSUSQL_OUTPUT_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib"
)

foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${configuration}" configuration_upper)
    set_target_properties(dansusql PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${configuration_upper}" "${DANSUSQL_OUTPUT_DIR}"
        "LIBRARY_OUTPUT_DIRECTORY_${configuration_upper}" "${DANSUSQL_OUTPUT_DIR}"
        "ARCHIVE_OUTPUT_DIRECTORY_${configuration_upper}" "${CMAKE_CURRENT_BINARY_DIR}/lib"
    )
endforeach()
