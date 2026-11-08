#include "sql/executor.h"

#include <exception>
#include <utility>
#include <vector>

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

bool CompareValues(
    const sql::Value& left,
    sql::CompareOp op,
    const sql::Value& right
) {
    int comparison = 0;

    if (left.type == sql::Value::Type::INTEGER) {
        comparison =
            left.int_value < right.int_value ? -1 :
            left.int_value > right.int_value ? 1 : 0;
    } else {
        comparison =
            left.string_value < right.string_value ? -1 :
            left.string_value > right.string_value ? 1 : 0;
    }

    switch (op) {
        case sql::CompareOp::EQ: return comparison == 0;
        case sql::CompareOp::NE: return comparison != 0;
        case sql::CompareOp::LT: return comparison < 0;
        case sql::CompareOp::GT: return comparison > 0;
        case sql::CompareOp::LE: return comparison <= 0;
        case sql::CompareOp::GE: return comparison >= 0;
    }

    return false;
}


bool ValuesEqual(const sql::Value& left, const sql::Value& right) {
    if (left.type != right.type) return false;
    return left.type == sql::Value::Type::INTEGER
        ? left.int_value == right.int_value
        : left.string_value == right.string_value;
}

bool RowsConflictOnKey(const KeySchema& key, const TableSchema& schema,
                       const std::vector<sql::Value>& left,
                       const std::vector<sql::Value>& right) {
    for (const auto& name : key.columns) {
        const int index = schema.ColumnIndex(name);
        if (index < 0 || static_cast<std::size_t>(index) >= left.size() ||
            static_cast<std::size_t>(index) >= right.size()) return false;
        if (!ValuesEqual(left[static_cast<std::size_t>(index)],
                         right[static_cast<std::size_t>(index)])) return false;
    }
    return true;
}

bool ValidateKeyConstraints(const TableSchema& schema,
                            const std::vector<std::vector<sql::Value>>& rows,
                            std::string* error) {
    for (const auto& key : schema.key_constraints) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            for (std::size_t j = i + 1; j < rows.size(); ++j) {
                if (RowsConflictOnKey(key, schema, rows[i], rows[j])) {
                    std::string columns;
                    for (const auto& name : key.columns) {
                        if (!columns.empty()) columns += ", ";
                        columns += name;
                    }
                    const std::string kind =
                        key.type == sql::KeyConstraintType::PRIMARY_KEY
                            ? "PRIMARY KEY" : "UNIQUE";
                    SetError(error, kind + " constraint violation on (" +
                                    columns + ") in table '" + schema.name + "'");
                    return false;
                }
            }
        }
    }
    return true;
}

using ValueRows = std::vector<std::vector<sql::Value>>;
using StoredRows = std::vector<std::pair<RID, std::vector<sql::Value>>>;

bool RowsMatchColumns(const TableSchema& left_schema,
                      const std::vector<std::string>& left_columns,
                      const std::vector<sql::Value>& left_row,
                      const TableSchema& right_schema,
                      const std::vector<std::string>& right_columns,
                      const std::vector<sql::Value>& right_row) {
    if (left_columns.size() != right_columns.size()) return false;
    for (std::size_t i = 0; i < left_columns.size(); ++i) {
        const int left_index = left_schema.ColumnIndex(left_columns[i]);
        const int right_index = right_schema.ColumnIndex(right_columns[i]);
        if (left_index < 0 || right_index < 0 ||
            static_cast<std::size_t>(left_index) >= left_row.size() ||
            static_cast<std::size_t>(right_index) >= right_row.size() ||
            !ValuesEqual(left_row[static_cast<std::size_t>(left_index)],
                         right_row[static_cast<std::size_t>(right_index)])) {
            return false;
        }
    }
    return true;
}

bool TupleExists(const TableSchema& source_schema,
                 const std::vector<std::string>& source_columns,
                 const std::vector<sql::Value>& source_row,
                 const TableSchema& target_schema,
                 const std::vector<std::string>& target_columns,
                 const ValueRows& target_rows) {
    for (const auto& target_row : target_rows) {
        if (RowsMatchColumns(source_schema, source_columns, source_row,
                             target_schema, target_columns, target_row)) {
            return true;
        }
    }
    return false;
}

template <typename ScanRows>
bool ValidateOutgoingForeignKeys(const TableSchema& child_schema,
                                 const ValueRows& child_rows,
                                 const Catalog& catalog,
                                 ScanRows&& scan_rows,
                                 std::string* error) {
    for (const auto& foreign_key : child_schema.foreign_key_constraints) {
        const TableSchema* parent_schema =
            catalog.GetTable(foreign_key.referenced_table);
        if (parent_schema == nullptr) {
            SetError(error, "referenced table '" + foreign_key.referenced_table +
                            "' does not exist in the Catalog");
            return false;
        }

        ValueRows scanned_parent_rows;
        const ValueRows* parent_rows = &scanned_parent_rows;
        if (NormalizeIdentifier(child_schema.name) ==
            NormalizeIdentifier(parent_schema->name)) {
            // Use the candidate row set for self-referencing constraints.
            parent_rows = &child_rows;
        } else {
            StoredRows stored_parent_rows;
            if (!scan_rows(parent_schema->name, &stored_parent_rows, error)) {
                return false;
            }
            scanned_parent_rows.reserve(stored_parent_rows.size());
            for (const auto& stored : stored_parent_rows) {
                scanned_parent_rows.push_back(stored.second);
            }
        }

        for (const auto& child_row : child_rows) {
            if (!TupleExists(child_schema, foreign_key.columns, child_row,
                             *parent_schema, foreign_key.referenced_columns,
                             *parent_rows)) {
                SetError(error, "FOREIGN KEY constraint violation on table '" +
                                child_schema.name + "': referenced row does not exist in table '" +
                                parent_schema->name + "'");
                return false;
            }
        }
    }
    return true;
}

template <typename ScanRows>
bool ValidateIncomingForeignKeys(const TableSchema& parent_schema,
                                 const ValueRows& parent_rows,
                                 const Catalog& catalog,
                                 ScanRows&& scan_rows,
                                 std::string* error) {
    for (const auto& child_table_name : catalog.TableNames()) {
        const TableSchema* child_schema = catalog.GetTable(child_table_name);
        if (child_schema == nullptr) continue;

        for (const auto& foreign_key : child_schema->foreign_key_constraints) {
            if (NormalizeIdentifier(foreign_key.referenced_table) !=
                NormalizeIdentifier(parent_schema.name)) {
                continue;
            }

            ValueRows scanned_child_rows;
            const ValueRows* child_rows = &scanned_child_rows;
            if (NormalizeIdentifier(child_schema->name) ==
                NormalizeIdentifier(parent_schema.name)) {
                // A self-reference must be checked against the same candidate set.
                child_rows = &parent_rows;
            } else {
                StoredRows stored_child_rows;
                if (!scan_rows(child_schema->name, &stored_child_rows, error)) {
                    return false;
                }
                scanned_child_rows.reserve(stored_child_rows.size());
                for (const auto& stored : stored_child_rows) {
                    scanned_child_rows.push_back(stored.second);
                }
            }

            for (const auto& child_row : *child_rows) {
                if (!TupleExists(*child_schema, foreign_key.columns, child_row,
                                 parent_schema, foreign_key.referenced_columns,
                                 parent_rows)) {
                    SetError(error, "FOREIGN KEY RESTRICT violation: table '" +
                                    parent_schema.name + "' has rows referenced by table '" +
                                    child_schema->name + "'");
                    return false;
                }
            }
        }
    }
    return true;
}

bool MatchesCondition(
    const sql::Condition& condition,
    const TableSchema& schema,
    const std::vector<sql::Value>& row
) {
    // AND groups are evaluated first. OR joins those groups.
    bool current_and_group = true;
    bool overall_result = false;

    for (std::size_t i = 0; i < condition.comparisons.size(); ++i) {
        const auto& comparison = condition.comparisons[i];
        const int column_index = schema.ColumnIndex(comparison.column);

        if (column_index < 0 ||
            static_cast<std::size_t>(column_index) >= row.size()) {
            return false;
        }

        const bool matches = CompareValues(
            row[static_cast<std::size_t>(column_index)],
            comparison.op,
            comparison.value
        );

        current_and_group = current_and_group && matches;

        const bool end_of_group =
            i + 1 == condition.comparisons.size() ||
            condition.connectors[i] == sql::LogicalOp::OR;

        if (end_of_group) {
            overall_result = overall_result || current_and_group;
            current_and_group = true;
        }
    }

    return overall_result;
}

}  // namespace

SqlExecutor::SqlExecutor(Catalog& catalog, BufferPoolManager& bpm)
    : catalog_(catalog),
      bpm_(bpm) {
}

bool SqlExecutor::Execute(
    const std::string& sql_text,
    RID* inserted_rid,
    std::string* error,
    QueryResult* query_result
) {
    ClearError(error);

    if (inserted_rid != nullptr) {
        *inserted_rid = RID{};
    }

    if (query_result != nullptr) {
        *query_result = QueryResult{};
    }

    try {
        const sql::Statement statement = sql::Parse(sql_text);
        return Execute(statement, inserted_rid, error, query_result);
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
}

bool SqlExecutor::Execute(
    const sql::Statement& statement,
    RID* inserted_rid,
    std::string* error,
    QueryResult* query_result
) {
    ClearError(error);

    if (inserted_rid != nullptr) {
        *inserted_rid = RID{};
    }

    if (query_result != nullptr) {
        *query_result = QueryResult{};
    }

    if (const auto* create =
            std::get_if<sql::CreateTableStatement>(&statement)) {
        return ExecuteCreate(*create, error);
    }

    if (const auto* insert =
            std::get_if<sql::InsertStatement>(&statement)) {
        return ExecuteInsert(*insert, inserted_rid, error);
    }

    if (const auto* select =
            std::get_if<sql::SelectStatement>(&statement)) {
        if (query_result == nullptr) {
            QueryResult discarded;
            return ExecuteSelect(*select, &discarded, error);
        }

        return ExecuteSelect(*select, query_result, error);
    }

    if (const auto* update =
            std::get_if<sql::UpdateStatement>(&statement)) {
        return ExecuteUpdate(*update, error);
    }

    if (const auto* remove =
            std::get_if<sql::DeleteStatement>(&statement)) {
        return ExecuteDelete(*remove, error);
    }

    SetError(error, "unsupported SQL statement");
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

        const auto insertion = tables_.emplace(key, std::move(runtime));

        if (!insertion.second) {
            bpm_.DeletePage(first_page_id);
            SetError(
                error,
                "table storage is already registered for '" +
                    statement.table + "'"
            );
            return false;
        }

        runtime_registered = true;

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
    auto scan_table_rows = [this](
        const std::string& table_name,
        StoredRows* rows,
        std::string* scan_error
    ) -> bool {
        const auto it = tables_.find(NormalizeIdentifier(table_name));
        if (it == tables_.end() || it->second.record_table == nullptr) {
            SetError(scan_error, "table '" + table_name +
                                "' has no runtime storage managed by this SQL executor");
            return false;
        }
        return it->second.record_table->Scan(rows, scan_error);
    };

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

    const TableSchema* schema = catalog_.GetTable(statement.table);
    std::vector<std::pair<RID, std::vector<sql::Value>>> stored_rows;
    if (schema == nullptr ||
        !table_it->second.record_table->Scan(&stored_rows, error)) {
        if (schema == nullptr) SetError(error, "table schema is unavailable");
        return false;
    }
    std::vector<std::vector<sql::Value>> rows;
    rows.reserve(stored_rows.size() + 1);
    for (const auto& stored : stored_rows) rows.push_back(stored.second);
    rows.push_back(statement.values);
    if (!ValidateKeyConstraints(*schema, rows, error) ||
        !ValidateOutgoingForeignKeys(*schema, rows, catalog_, scan_table_rows, error)) {
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

bool SqlExecutor::ExecuteSelect(
    const sql::SelectStatement& statement,
    QueryResult* query_result,
    std::string* error
) {
    if (query_result == nullptr) {
        SetError(error, "query result output pointer is null");
        return false;
    }

    try {
        Validate(sql::Statement{statement}, catalog_);
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }

    const TableSchema* schema = catalog_.GetTable(statement.table);

    if (schema == nullptr) {
        SetError(error, "table '" + statement.table + "' does not exist");
        return false;
    }

    const auto table_it =
        tables_.find(NormalizeIdentifier(statement.table));

    if (table_it == tables_.end() ||
        table_it->second.record_table == nullptr) {
        SetError(
            error,
            "table '" + statement.table +
                "' has no runtime storage managed by this SQL executor"
        );
        return false;
    }

    std::vector<std::pair<RID, std::vector<sql::Value>>> stored_rows;

    if (!table_it->second.record_table->Scan(&stored_rows, error)) {
        return false;
    }

    try {
        std::vector<std::size_t> projection;

        if (statement.select_all) {
            for (std::size_t i = 0; i < schema->columns.size(); ++i) {
                projection.push_back(i);
            }
        } else {
            for (const auto& name : statement.columns) {
                const int index = schema->ColumnIndex(name);

                if (index < 0) {
                    SetError(error, "unknown projected column '" + name + "'");
                    return false;
                }

                projection.push_back(static_cast<std::size_t>(index));
            }
        }

        QueryResult result;

        for (const std::size_t index : projection) {
            result.column_names.push_back(schema->columns[index].name);
        }

        for (const auto& stored : stored_rows) {
            const auto& row = stored.second;

            if (statement.where.has_value() &&
                !MatchesCondition(*statement.where, *schema, row)) {
                continue;
            }

            std::vector<sql::Value> projected_row;
            projected_row.reserve(projection.size());

            for (const std::size_t index : projection) {
                projected_row.push_back(row[index]);
            }

            result.rows.push_back(std::move(projected_row));
        }

        *query_result = std::move(result);
        return true;
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
}

bool SqlExecutor::ExecuteUpdate(
    const sql::UpdateStatement& statement,
    std::string* error
) {
    auto scan_table_rows = [this](
        const std::string& table_name,
        StoredRows* rows,
        std::string* scan_error
    ) -> bool {
        const auto it = tables_.find(NormalizeIdentifier(table_name));
        if (it == tables_.end() || it->second.record_table == nullptr) {
            SetError(scan_error, "table '" + table_name +
                                "' has no runtime storage managed by this SQL executor");
            return false;
        }
        return it->second.record_table->Scan(rows, scan_error);
    };

    try {
        Validate(sql::Statement{statement}, catalog_);
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }

    const TableSchema* schema = catalog_.GetTable(statement.table);

    if (schema == nullptr) {
        SetError(error, "table '" + statement.table + "' does not exist");
        return false;
    }

    const auto table_it =
        tables_.find(NormalizeIdentifier(statement.table));

    if (table_it == tables_.end() ||
        table_it->second.record_table == nullptr) {
        SetError(
            error,
            "table '" + statement.table +
                "' has no runtime storage managed by this SQL executor"
        );
        return false;
    }

    std::vector<std::pair<RID, std::vector<sql::Value>>> stored_rows;

    if (!table_it->second.record_table->Scan(&stored_rows, error)) {
        return false;
    }

    try {
        std::vector<std::vector<sql::Value>> resulting_rows;
        resulting_rows.reserve(stored_rows.size());
        std::vector<std::pair<RID, std::vector<sql::Value>>> updates;

        for (const auto& stored : stored_rows) {
            const RID& rid = stored.first;
            const auto& original_row = stored.second;
            std::vector<sql::Value> updated_row = original_row;

            if (!statement.where.has_value() ||
                MatchesCondition(*statement.where, *schema, original_row)) {
                for (const auto& assignment : statement.assignments) {
                    const int column_index = schema->ColumnIndex(assignment.column);
                    if (column_index < 0 ||
                        static_cast<std::size_t>(column_index) >= updated_row.size()) {
                        SetError(error, "unknown UPDATE column '" + assignment.column + "'");
                        return false;
                    }
                    updated_row[static_cast<std::size_t>(column_index)] = assignment.value;
                }
                bool changed = false;
                for (std::size_t i = 0; i < updated_row.size(); ++i) {
                    if (!ValuesEqual(updated_row[i], original_row[i])) {
                        changed = true;
                        break;
                    }
                }
                if (changed) updates.emplace_back(rid, updated_row);
            }
            resulting_rows.push_back(std::move(updated_row));
        }

        if (!ValidateKeyConstraints(*schema, resulting_rows, error) ||
            !ValidateOutgoingForeignKeys(*schema, resulting_rows, catalog_,
                                         scan_table_rows, error) ||
            !ValidateIncomingForeignKeys(*schema, resulting_rows, catalog_,
                                         scan_table_rows, error)) {
            return false;
        }

        for (const auto& update : updates) {
            if (!table_it->second.record_table->Update(update.first, update.second, error)) {
                if (error != nullptr && error->empty())
                    SetError(error, "failed to update a matching row");
                return false;
            }
        }
        return true;
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
}

bool SqlExecutor::ExecuteDelete(
    const sql::DeleteStatement& statement,
    std::string* error
) {
    auto scan_table_rows = [this](
        const std::string& table_name,
        StoredRows* rows,
        std::string* scan_error
    ) -> bool {
        const auto it = tables_.find(NormalizeIdentifier(table_name));
        if (it == tables_.end() || it->second.record_table == nullptr) {
            SetError(scan_error, "table '" + table_name +
                                "' has no runtime storage managed by this SQL executor");
            return false;
        }
        return it->second.record_table->Scan(rows, scan_error);
    };

    try {
        Validate(sql::Statement{statement}, catalog_);
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }

    const TableSchema* schema = catalog_.GetTable(statement.table);

    if (schema == nullptr) {
        SetError(error, "table '" + statement.table + "' does not exist");
        return false;
    }

    const auto table_it =
        tables_.find(NormalizeIdentifier(statement.table));

    if (table_it == tables_.end() ||
        table_it->second.record_table == nullptr) {
        SetError(
            error,
            "table '" + statement.table +
                "' has no runtime storage managed by this SQL executor"
        );
        return false;
    }

    std::vector<std::pair<RID, std::vector<sql::Value>>> stored_rows;

    if (!table_it->second.record_table->Scan(&stored_rows, error)) {
        return false;
    }

    try {
        // Build the complete post-delete row set first. Referential checks
        // run against this candidate state before any record is removed.
        std::vector<RID> matching_rids;
        matching_rids.reserve(stored_rows.size());
        ValueRows resulting_rows;
        resulting_rows.reserve(stored_rows.size());

        for (const auto& stored : stored_rows) {
            const RID& rid = stored.first;
            const auto& row = stored.second;

            if (!statement.where.has_value() ||
                MatchesCondition(*statement.where, *schema, row)) {
                matching_rids.push_back(rid);
            } else {
                resulting_rows.push_back(row);
            }
        }

        if (!ValidateOutgoingForeignKeys(*schema, resulting_rows, catalog_,
                                         scan_table_rows, error) ||
            !ValidateIncomingForeignKeys(*schema, resulting_rows, catalog_,
                                         scan_table_rows, error)) {
            return false;
        }

        for (const RID& rid : matching_rids) {
            if (!table_it->second.record_table->Delete(rid)) {
                SetError(error, "failed to delete a matching row");
                return false;
            }
        }

        return true;
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
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