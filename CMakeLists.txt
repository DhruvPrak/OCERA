cmake_minimum_required(VERSION 3.15)
project(MiniDB CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

find_package(Threads REQUIRED)

# Storage engine sources, shared by the main binary and by tests.
add_library(minidb_storage
    src/storage/disk_manager.cpp
    src/storage/free_space_manager.cpp
    src/storage/database_header.cpp
)
target_include_directories(minidb_storage PUBLIC src)
target_link_libraries(minidb_storage PUBLIC Threads::Threads)

add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE Threads::Threads minidb_storage)

add_executable(test_storage tests/test_storage.cpp)
target_link_libraries(test_storage PRIVATE minidb_storage)

add_executable(test_header tests/test_header.cpp)
target_link_libraries(test_header PRIVATE minidb_storage)
