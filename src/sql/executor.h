#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "common/rid.h"
#include "record/record_table.h"
#include "record/table_heap.h"
#include "sql/ast.h"
#include "storage/buffer_pool_manager.h"

namespace ocera {

// Executes the initial SQL subset: CREATE TABLE and INSERT.
// Catalog and table storage are owned by the caller/executor respectively;
// neither catalog metadata nor this runtime registry is persisted.
class SqlExecutor {
public:
    SqlExecutor(Catalog& catalog, BufferPoolManager& bpm);

    // Parse and execute one SQL statement.
    // Returns false for parse, semantic, storage, or unsupported-statement errors.
    bool Execute(
        const std::string& sql_text,
        RID* inserted_rid = nullptr,
        std::string* error = nullptr
    );

    // Execute an already-parsed AST statement.
    bool Execute(
        const sql::Statement& statement,
        RID* inserted_rid = nullptr,
        std::string* error = nullptr
    );

    // Read a row by its RID. This is a storage-level inspection/access helper,
    // not implementation of the SQL SELECT statement.
    bool GetRowByRID(
        const std::string& table_name,
        const RID& rid,
        std::vector<sql::Value>* values,
        std::string* error = nullptr
    ) const;

private:
    struct TableRuntime {
        // Declaration order matters: record_table is destroyed before heap.
        std::unique_ptr<TableHeap> heap;
        std::unique_ptr<RecordTable> record_table;
    };

    bool ExecuteCreate(
        const sql::CreateTableStatement& statement,
        std::string* error
    );

    bool ExecuteInsert(
        const sql::InsertStatement& statement,
        RID* inserted_rid,
        std::string* error
    );

    Catalog& catalog_;
    BufferPoolManager& bpm_;
    std::map<std::string, TableRuntime> tables_;
};

}  // namespace ocera
