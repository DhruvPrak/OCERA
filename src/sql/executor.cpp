#include "sql/executor.h"

#include <exception>
#include <utility>

#include "catalog/validator.h"
#include "sql/parser.h"

namespace ocera {
namespace {

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

void ClearError(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

TableSchema MakeSchema(const sql::CreateTableStatement& statement) {
    TableSchema schema;
    schema.name = statement.table;
    schema.columns.reserve(statement.columns.size());

    for (const auto& column : statement.columns) {
        schema.columns.push_back({column.name, column.type});
    }

    return schema;
}

}  // namespace

SqlExecutor::SqlExecutor(Catalog& catalog, BufferPoolManager& bpm)
    : catalog_(catalog),
      bpm_(bpm) {
}

bool SqlExecutor::Execute(
    const std::string& sql_text,
    RID* inserted_rid,
    std::string* error
) {
    ClearError(error);
    if (inserted_rid != nullptr) {
        *inserted_rid = RID{};
    }

    try {
        const sql::Statement statement = sql::Parse(sql_text);
        return Execute(statement, inserted_rid, error);
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
}

bool SqlExecutor::Execute(
    const sql::Statement& statement,
    RID* inserted_rid,
    std::string* error
) {
    ClearError(error);
    if (inserted_rid != nullptr) {
        *inserted_rid = RID{};
    }

    if (const auto* create =
            std::get_if<sql::CreateTableStatement>(&statement)) {
        return ExecuteCreate(*create, error);
    }

    if (const auto* insert =
            std::get_if<sql::InsertStatement>(&statement)) {
        return ExecuteInsert(*insert, inserted_rid, error);
    }

    SetError(
        error,
        "SQL executor currently supports only CREATE TABLE and INSERT"
    );
    return false;
}

bool SqlExecutor::ExecuteCreate(
    const sql::CreateTableStatement& statement,
    std::string* error
) {
    try {
        Validate(sql::Statement{statement}, catalog_);
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }

    page_id_t first_page_id = INVALID_PAGE_ID;
    if (!TableHeap::Create(bpm_, &first_page_id)) {
        SetError(error, "failed to allocate the table's first record page");
        return false;
    }

    const std::string key = NormalizeIdentifier(statement.table);
    bool runtime_registered = false;

    try {
        auto heap = std::make_unique<TableHeap>(bpm_, first_page_id);
        auto record_table =
            std::make_unique<RecordTable>(MakeSchema(statement), *heap);

        TableRuntime runtime;
        runtime.heap = std::move(heap);
        runtime.record_table = std::move(record_table);

        const auto insertion =
            tables_.emplace(key, std::move(runtime));

        if (!insertion.second) {
            bpm_.DeletePage(first_page_id);
            SetError(error, "table storage is already registered for '" +
                                statement.table + "'");
            return false;
        }

        runtime_registered = true;

        // Register the Catalog schema only after storage is ready. If Catalog
        // registration fails, remove the runtime objects and release the page.
        try {
            catalog_.CreateTable(statement);
        } catch (...) {
            tables_.erase(key);
            runtime_registered = false;
            throw;
        }

        return true;
    } catch (const std::exception& exception) {
        if (!runtime_registered) {
            bpm_.DeletePage(first_page_id);
        }
        SetError(error, exception.what());
        return false;
    }
}

bool SqlExecutor::ExecuteInsert(
    const sql::InsertStatement& statement,
    RID* inserted_rid,
    std::string* error
) {
    try {
        Validate(sql::Statement{statement}, catalog_);
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }

    const std::string key = NormalizeIdentifier(statement.table);
    const auto table_it = tables_.find(key);

    if (table_it == tables_.end() ||
        table_it->second.record_table == nullptr) {
        SetError(
            error,
            "table '" + statement.table +
                "' has no runtime storage managed by this SQL executor"
        );
        return false;
    }

    RID new_rid;
    if (!table_it->second.record_table->Insert(
            statement.values, &new_rid, error)) {
        return false;
    }

    if (inserted_rid != nullptr) {
        *inserted_rid = new_rid;
    }

    return true;
}

bool SqlExecutor::GetRowByRID(
    const std::string& table_name,
    const RID& rid,
    std::vector<sql::Value>* values,
    std::string* error
) const {
    ClearError(error);

    if (values == nullptr) {
        SetError(error, "value output pointer is null");
        return false;
    }

    const auto table_it = tables_.find(NormalizeIdentifier(table_name));
    if (table_it == tables_.end() ||
        table_it->second.record_table == nullptr) {
        SetError(
            error,
            "table '" + table_name +
                "' has no runtime storage managed by this SQL executor"
        );
        return false;
    }

    return table_it->second.record_table->Get(rid, values, error);
}

}  // namespace ocera
