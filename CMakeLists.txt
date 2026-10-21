cmake_minimum_required(VERSION 3.15)
project(OCERA CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

find_package(Threads REQUIRED)

# Storage engine sources, shared by the main binary and by tests.
add_library(ocera_storage
    src/storage/disk_manager.cpp
    src/storage/free_space_manager.cpp
    src/storage/database_header.cpp
)
target_include_directories(ocera_storage PUBLIC src)
target_link_libraries(ocera_storage PUBLIC Threads::Threads)

add_executable(ocera src/main.cpp)
target_link_libraries(ocera PRIVATE Threads::Threads ocera_storage)

add_executable(test_storage tests/test_storage.cpp)
target_link_libraries(test_storage PRIVATE ocera_storage)

add_executable(test_header tests/test_header.cpp)
target_link_libraries(test_header PRIVATE ocera_storage)
# SQL front-end (tokenizer, later parser).
add_library(ocera_sql
    src/sql/tokenizer.cpp
    src/sql/parser.cpp
)
target_include_directories(ocera_sql PUBLIC src)

add_executable(test_tokenizer tests/test_tokenizer.cpp)
target_link_libraries(test_tokenizer PRIVATE ocera_sql)

add_executable(test_parser tests/test_parser.cpp)
target_link_libraries(test_parser PRIVATE ocera_sql)

# Transaction / concurrency module (Bhavya). Independent of storage code.
add_library(ocera_txn
    src/txn/lock_manager.cpp
    src/txn/transaction_manager.cpp
    src/txn/deadlock_detector.cpp
)
target_include_directories(ocera_txn PUBLIC src)
target_link_libraries(ocera_txn PUBLIC Threads::Threads)

add_executable(test_lock_manager tests/test_lock_manager.cpp)
target_link_libraries(test_lock_manager PRIVATE ocera_txn)

add_executable(test_transaction_manager tests/test_transaction_manager.cpp)
target_link_libraries(test_transaction_manager PRIVATE ocera_txn)

add_executable(test_deadlock_detector tests/test_deadlock_detector.cpp)
target_link_libraries(test_deadlock_detector PRIVATE ocera_txn)

add_executable(demo_concurrency demo/demo_concurrency.cpp)
target_link_libraries(demo_concurrency PRIVATE ocera_txn)
# B+Tree index (in-memory version for now).
add_library(ocera_index
    src/index/bplus_tree.cpp
)
target_include_directories(ocera_index PUBLIC src)

add_executable(test_bplus_tree tests/test_bplus_tree.cpp)
target_link_libraries(test_bplus_tree PRIVATE ocera_index)

