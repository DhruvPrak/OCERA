#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "common/rid.h"
#include "index/bplus_tree.h"
#include "record/record_table.h"
#include "record/table_heap.h"
#include "sql/ast.h"
#include "storage/buffer_pool_manager.h"

namespace ocera {

struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<std::vector<sql::Value>> rows;
};

class SqlExecutor {
public:
    SqlExecutor(Catalog& catalog, BufferPoolManager& bpm);

    bool Execute(
        const std::string& sql_text,
        RID* inserted_rid = nullptr,
        std::string* error = nullptr,
        QueryResult* query_result = nullptr
    );
    // Rebuild runtime table objects using persisted Catalog metadata.
    bool RestoreTables(std::string* error = nullptr);
    bool Execute(
        const sql::Statement& statement,
        RID* inserted_rid = nullptr,
        std::string* error = nullptr,
        QueryResult* query_result = nullptr
    );

    bool GetRowByRID(
        const std::string& table_name,
        const RID& rid,
        std::vector<sql::Value>* values,
        std::string* error = nullptr
    ) const;

private:
    struct TableRuntime {
        std::unique_ptr<TableHeap> heap;
        std::unique_ptr<RecordTable> record_table;
    };

    bool ExecuteCreate(const sql::CreateTableStatement& statement, std::string* error);
    bool ExecuteCreateIndex(const sql::CreateIndexStatement& statement, std::string* error);
    bool ExecuteInsert(const sql::InsertStatement& statement, RID* inserted_rid, std::string* error);
    bool ExecuteSelect(const sql::SelectStatement& statement, QueryResult* query_result, std::string* error);
    bool ExecuteUpdate(const sql::UpdateStatement& statement, std::string* error);
    bool ExecuteDelete(const sql::DeleteStatement& statement, std::string* error);

    Catalog& catalog_;
    BufferPoolManager& bpm_;
    std::map<std::string, TableRuntime> tables_;
    std::map<std::string, std::unique_ptr<BPlusTree>> indexes_;
};

}  // namespace ocera
