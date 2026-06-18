cmake_minimum_required(VERSION 3.16)
project(MiniDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# Enable warnings for clean code.
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(minidb
    engine/BufferPool.cpp
    engine/DiskManager.cpp
    engine/Engine.cpp
    engine/Index.cpp
    engine/LogManager.cpp
    engine/RecoveryManager.cpp
    engine/main.cpp
)
target_include_directories(minidb PRIVATE engine)
target_link_libraries(minidb PRIVATE Threads::Threads)

add_executable(minidb_cli
    engine/BufferPool.cpp
    engine/DiskManager.cpp
    engine/Engine.cpp
    engine/Index.cpp
    engine/LogManager.cpp
    engine/RecoveryManager.cpp
    engine/cli.cpp
)
target_include_directories(minidb_cli PRIVATE engine)
target_link_libraries(minidb_cli PRIVATE Threads::Threads)


add_executable(minidb_server
    engine/BufferPool.cpp
    engine/DiskManager.cpp
    engine/Engine.cpp
    engine/Index.cpp
    engine/LogManager.cpp
    engine/RecoveryManager.cpp
    engine/server.cpp
)
target_include_directories(minidb_server PRIVATE engine)
target_link_libraries(minidb_server PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(minidb_server PRIVATE ws2_32)
endif()
