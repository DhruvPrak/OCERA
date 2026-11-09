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
    src/storage/buffer_pool_manager.cpp
)
target_include_directories(ocera_storage PUBLIC src)
target_link_libraries(ocera_storage PUBLIC Threads::Threads)

add_executable(ocera src/main.cpp)

target_link_libraries(ocera
    PRIVATE
        ocera_sql_executor
)

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

add_executable(test_buffer_pool tests/test_buffer_pool.cpp)
target_link_libraries(test_buffer_pool PRIVATE ocera_storage)

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

# Catalog and semantic validation (in-memory). Uses the AST from ocera_sql.
add_library(ocera_catalog
    src/catalog/catalog.cpp
    src/catalog/validator.cpp
)
target_include_directories(ocera_catalog PUBLIC src)
target_link_libraries(ocera_catalog PUBLIC ocera_sql)

add_executable(test_catalog tests/test_catalog.cpp)
target_link_libraries(test_catalog PRIVATE ocera_catalog)

# Schema-aware record serialization (INT / TEXT).
add_library(ocera_record_serializer
    src/record/record_serializer.cpp
)

target_include_directories(ocera_record_serializer PUBLIC src)

target_link_libraries(ocera_record_serializer
    PUBLIC
        ocera_catalog
)

add_executable(test_record_serializer
    tests/test_record_serializer.cpp
)

target_link_libraries(test_record_serializer
    PRIVATE
        ocera_record_serializer
)

# Record Page
add_library(ocera_record
    src/record/record_page.cpp
)

target_include_directories(ocera_record PUBLIC src)

add_executable(test_record_page
    tests/test_record_page.cpp
)

target_link_libraries(test_record_page PRIVATE ocera_record)

# Table Heap
add_library(ocera_table_heap
    src/record/table_heap.cpp
)

target_include_directories(ocera_table_heap PUBLIC src)

target_link_libraries(ocera_table_heap
    PUBLIC
        ocera_record
        ocera_storage
)

add_executable(test_table_heap
    tests/test_table_heap.cpp
)

target_link_libraries(test_table_heap
    PRIVATE
        ocera_table_heap
)

# Typed row adapter over RecordSerializer + TableHeap.
# Place this after the existing ocera_table_heap target.
add_library(ocera_record_table
    src/record/record_table.cpp
)
target_include_directories(ocera_record_table PUBLIC src)
target_link_libraries(ocera_record_table
    PUBLIC
        ocera_record_serializer
        ocera_table_heap
)

add_executable(test_record_table
    tests/test_record_table.cpp
)
target_link_libraries(test_record_table PRIVATE ocera_record_table)

# Initial SQL execution layer: CREATE TABLE and INSERT.
add_library(ocera_sql_executor
    src/sql/executor.cpp
)
target_include_directories(ocera_sql_executor PUBLIC src)
target_link_libraries(ocera_sql_executor
    PUBLIC
        ocera_catalog
        ocera_record_table
)

add_executable(test_sql_executor
    tests/test_sql_executor.cpp
)
target_link_libraries(test_sql_executor PRIVATE ocera_sql_executor)

# Multiline SQL statement reader tests.
add_executable(test_statement_reader
    tests/test_statement_reader.cpp
)

target_include_directories(test_statement_reader PRIVATE src)
