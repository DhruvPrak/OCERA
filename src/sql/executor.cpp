#include "sql/executor.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "catalog/validator.h"
#include "catalog/catalog_storage.h"
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

    for (const auto& key : statement.key_constraints)
        schema.key_constraints.push_back({key.type, key.columns});
    for (const auto& fk : statement.foreign_key_constraints)
        schema.foreign_key_constraints.push_back(
            {fk.columns, fk.referenced_table, fk.referenced_columns});
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

bool LikeMatches(const std::string& value, const std::string& pattern) {
    // Dynamic programming keeps wildcard matching linear in the pattern/value
    // grid and avoids recursive backtracking on repeated '%' characters.
    std::vector<bool> previous(value.size() + 1, false);
    std::vector<bool> current(value.size() + 1, false);
    previous[0] = true;

    for (char token : pattern) {
        std::fill(current.begin(), current.end(), false);
        if (token == '%') {
            current[0] = previous[0];
            for (std::size_t j = 1; j <= value.size(); ++j)
                current[j] = previous[j] || current[j - 1];
        } else {
            for (std::size_t j = 1; j <= value.size(); ++j) {
                if (token == '_' || token == value[j - 1])
                    current[j] = previous[j - 1];
            }
        }
        previous.swap(current);
    }
    return previous[value.size()];
}

bool MatchesPredicate(const sql::ConditionPredicate& predicate,
                      const TableSchema& schema,
                      const std::vector<sql::Value>& row) {
    const int column_index = schema.ColumnIndex(predicate.column);
    if (column_index < 0 ||
        static_cast<std::size_t>(column_index) >= row.size()) return false;

    const sql::Value& value = row[static_cast<std::size_t>(column_index)];
    bool matches = false;
    switch (predicate.type) {
        case sql::PredicateType::COMPARISON:
            matches = predicate.values.size() == 1 &&
                CompareValues(value, predicate.op, predicate.values[0]);
            break;
        case sql::PredicateType::IS_NULL:
            // Stored rows currently cannot contain NULL. Therefore IS NULL is
            // false and IS NOT NULL is true for every existing row.
            matches = false;
            break;
        case sql::PredicateType::LIKE:
            matches = predicate.values.size() == 1 &&
                value.type == sql::Value::Type::STRING &&
                predicate.values[0].type == sql::Value::Type::STRING &&
                LikeMatches(value.string_value,
                            predicate.values[0].string_value);
            break;
        case sql::PredicateType::IN:
            for (const auto& candidate : predicate.values) {
                if (ValuesEqual(value, candidate)) {
                    matches = true;
                    break;
                }
            }
            break;
        case sql::PredicateType::BETWEEN:
            if (predicate.values.size() == 2)
                matches = CompareValues(value, sql::CompareOp::GE,
                                         predicate.values[0]) &&
                          CompareValues(value, sql::CompareOp::LE,
                                         predicate.values[1]);
            break;
    }
    return predicate.negated ? !matches : matches;
}

bool MatchesCondition(
    const sql::Condition& condition,
    const TableSchema& schema,
    const std::vector<sql::Value>& row
) {
    if (condition.predicates.empty() ||
        condition.connectors.size() + 1 != condition.predicates.size())
        return false;

    // SQL precedence: AND binds more tightly than OR.
    bool current_and_group = true;
    bool overall_result = false;
    for (std::size_t i = 0; i < condition.predicates.size(); ++i) {
        current_and_group = current_and_group &&
            MatchesPredicate(condition.predicates[i], schema, row);
        const bool end_of_group = i + 1 == condition.predicates.size() ||
            condition.connectors[i] == sql::LogicalOp::OR;
        if (end_of_group) {
            overall_result = overall_result || current_and_group;
            current_and_group = true;
        }
    }
    return overall_result;
}

}  // namespace

SqlExecutor::SqlExecutor(Catalog& catalog, BufferPoolManager& bpm,
                         DatabaseHeader* database_header)
    : catalog_(catalog), bpm_(bpm), database_header_(database_header) {
    if (database_header_ == nullptr) return;
    std::string error;
    if (!CatalogStorage::Load(catalog_, bpm_, *database_header_, &error))
        throw std::runtime_error("could not load saved table list: " + error);
    for (const auto& name : catalog_.TableNames()) {
        const TableSchema* schema = catalog_.GetTable(name);
        if (schema == nullptr || schema->first_page_id < 2)
            throw std::runtime_error("saved table has an invalid starting page: " + name);
        auto heap = std::make_unique<TableHeap>(bpm_, schema->first_page_id);
        auto record_table = std::make_unique<RecordTable>(*schema, *heap);
        TableRuntime runtime;
        runtime.heap = std::move(heap);
        runtime.record_table = std::move(record_table);
        tables_.emplace(NormalizeIdentifier(name), std::move(runtime));
    }

    // CatalogStorage persists index metadata, while B+Trees are in-memory
    // structures. Rebuild each runtime tree from its table rows on restart.
    for (const auto& index : catalog_.Indexes()) {
        std::unique_ptr<BPlusTree> tree;
        std::string index_error;
        if (!BuildIndexTree(index.table_name, index.column_name, &tree,
                            &index_error)) {
            throw std::runtime_error(
                "could not rebuild index '" + index.name + "': " + index_error);
        }

        const auto insertion = indexes_.emplace(
            NormalizeIdentifier(index.name), std::move(tree));
        if (!insertion.second) {
            throw std::runtime_error(
                "duplicate runtime index registration for '" + index.name + "'");
        }
    }
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

    if (const auto* create_index =
            std::get_if<sql::CreateIndexStatement>(&statement)) {
        return ExecuteCreateIndex(*create_index, error);
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

    if (std::get_if<sql::ShowTablesStatement>(&statement) != nullptr) {
    if (query_result == nullptr) {
        SetError(error, "SHOW TABLES requires a query result output");
        return false;
    }

    QueryResult result;
    result.column_names = {"Tables"};

    for (const auto& table_name : catalog_.TableNames()) {
        sql::Value value;
        value.type = sql::Value::Type::STRING;
        value.string_value = table_name;
        result.rows.push_back({std::move(value)});
    }

    *query_result = std::move(result);
    return true;
}

if (const auto* desc =
        std::get_if<sql::DescTableStatement>(&statement)) {
    const TableSchema* schema = catalog_.GetTable(desc->table);

    if (schema == nullptr) {
        SetError(error, "table '" + desc->table + "' does not exist");
        return false;
    }

    if (query_result == nullptr) {
        SetError(error, "DESC requires a query result output");
        return false;
    }

    QueryResult result;
    result.column_names = {"Column", "Type"};

    for (const auto& column : schema->columns) {
        sql::Value name;
        name.type = sql::Value::Type::STRING;
        name.string_value = column.name;

        sql::Value type;
        type.type = sql::Value::Type::STRING;
        type.string_value =
            column.type == sql::ColumnType::INT ? "INT" : "TEXT";

        result.rows.push_back({std::move(name), std::move(type)});
    }

    *query_result = std::move(result);
    return true;
}

    SetError(error, "unsupported SQL statement");
    return false;
}

bool SqlExecutor::BuildIndexTree(
    const std::string& table_name,
    const std::string& column_name,
    std::unique_ptr<BPlusTree>* tree,
    std::string* error
) {
    if (tree == nullptr) {
        SetError(error, "index tree output pointer is null");
        return false;
    }

    tree->reset();

    const TableSchema* schema = catalog_.GetTable(table_name);
    if (schema == nullptr) {
        SetError(error, "table '" + table_name + "' does not exist");
        return false;
    }

    const int column_index = schema->ColumnIndex(column_name);
    if (column_index < 0 ||
        static_cast<std::size_t>(column_index) >= schema->columns.size()) {
        SetError(error, "unknown index column '" + column_name +
                        "' in table '" + table_name + "'");
        return false;
    }

    if (schema->columns[static_cast<std::size_t>(column_index)].type !=
        sql::ColumnType::INT) {
        SetError(error, "B+Tree indexes currently support INT columns only");
        return false;
    }

    const auto table_it = tables_.find(NormalizeIdentifier(table_name));
    if (table_it == tables_.end() || table_it->second.record_table == nullptr) {
        SetError(error, "table '" + table_name +
                        "' has no runtime storage managed by this SQL executor");
        return false;
    }

    StoredRows rows;
    if (!table_it->second.record_table->Scan(&rows, error)) {
        return false;
    }

    auto built_tree = std::make_unique<BPlusTree>();
    for (const auto& stored : rows) {
        const RID& rid = stored.first;
        const auto& row = stored.second;

        if (static_cast<std::size_t>(column_index) >= row.size()) {
            SetError(error, "stored row is missing indexed column '" +
                            column_name + "'");
            return false;
        }

        const sql::Value& value = row[static_cast<std::size_t>(column_index)];
        if (value.type != sql::Value::Type::INTEGER) {
            SetError(error, "stored value has an incompatible type for index column '" +
                            column_name + "'");
            return false;
        }

        // Duplicate keys are supported; only an identical key/RID pair fails.
        if (!built_tree->Insert(value.int_value, rid)) {
            SetError(error, "duplicate key/RID pair encountered while building index '" +
                            column_name + "'");
            return false;
        }
    }

    *tree = std::move(built_tree);
    return true;
}

bool SqlExecutor::ExecuteCreateIndex(
    const sql::CreateIndexStatement& statement,
    std::string* error
) {
    try {
        Validate(sql::Statement{statement}, catalog_);

        std::unique_ptr<BPlusTree> tree;
        if (!BuildIndexTree(statement.table, statement.column, &tree, error)) {
            return false;
        }

        const std::string index_key = NormalizeIdentifier(statement.index_name);
        bool runtime_registered = false;
        bool catalog_registered = false;

        try {
            const auto runtime_insertion =
                indexes_.emplace(index_key, std::move(tree));
            if (!runtime_insertion.second) {
                SetError(error, "runtime index '" + statement.index_name +
                                "' is already registered");
                return false;
            }
            runtime_registered = true;

            catalog_.CreateIndex(statement);
            catalog_registered = true;

            if (database_header_ != nullptr) {
                std::string save_error;
                if (!CatalogStorage::Save(catalog_, bpm_, *database_header_,
                                          &save_error)) {
                    catalog_.RemoveIndex(statement.index_name);
                    catalog_registered = false;
                    indexes_.erase(index_key);
                    runtime_registered = false;
                    SetError(error, "could not save index metadata: " + save_error);
                    return false;
                }
            }

            return true;
        } catch (...) {
            if (catalog_registered) {
                catalog_.RemoveIndex(statement.index_name);
            }
            if (runtime_registered) {
                indexes_.erase(index_key);
            }
            throw;
        }
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return false;
    }
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
            if (!catalog_.SetTableFirstPageId(statement.table, first_page_id))
                throw std::runtime_error("could not save table starting page");
            if (database_header_ != nullptr) {
                std::string save_error;
                if (!CatalogStorage::Save(catalog_, bpm_, *database_header_, &save_error))
                    throw std::runtime_error("could not save table list: " + save_error);
            }
        } catch (...) {
            catalog_.RemoveTable(statement.table);
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

    // Keep every index on this table synchronized with the inserted row.
    for (const auto& index : catalog_.Indexes()) {
        if (NormalizeIdentifier(index.table_name) != key) continue;
        const int column = schema->ColumnIndex(index.column_name);
        const auto tree = indexes_.find(NormalizeIdentifier(index.name));
        if (column < 0 || tree == indexes_.end() ||
            !tree->second->Insert(
                statement.values[static_cast<std::size_t>(column)].int_value,
                new_rid)) {
            table_it->second.record_table->Delete(new_rid);
            SetError(error, "failed to update index '" + index.name + "' after INSERT");
            return false;
        }
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
    try { Validate(sql::Statement{statement}, catalog_); }
    catch (const std::exception& exception) { SetError(error, exception.what()); return false; }

    struct Relation {
        const TableSchema* schema;
        std::size_t offset;
        std::string qualifier;
    };
    std::vector<Relation> relations;
    std::vector<std::vector<sql::Value>> rows;
    std::size_t width = 0;

    auto scan = [&](const std::string& name,
                    std::vector<std::vector<sql::Value>>* output) -> bool {
        const auto it = tables_.find(NormalizeIdentifier(name));
        if (it == tables_.end() || !it->second.record_table) {
            SetError(error, "table '" + name + "' has no runtime storage managed by this SQL executor");
            return false;
        }
        std::vector<std::pair<RID, std::vector<sql::Value>>> stored;
        if (!it->second.record_table->Scan(&stored, error)) return false;
        output->clear(); output->reserve(stored.size());
        for (auto& item : stored) output->push_back(std::move(item.second));
        return true;
    };
    auto null_row = [](std::size_t count) {
        std::vector<sql::Value> result(count);
        for (auto& value : result) value.type = sql::Value::Type::NULL_VALUE;
        return result;
    };
    auto resolve = [&](const std::string& ref, std::size_t* index) -> bool {
        const auto dot = ref.find('.');
        const std::string table_name = dot == std::string::npos ? "" : ref.substr(0, dot);
        const std::string column_name = dot == std::string::npos ? ref : ref.substr(dot + 1);
        bool found = false;
        for (const auto& relation : relations) {
            if (dot != std::string::npos && NormalizeIdentifier(relation.qualifier) != NormalizeIdentifier(table_name)) continue;
            const int column = relation.schema->ColumnIndex(column_name);
            if (column < 0) continue;
            if (found) { SetError(error, "ambiguous column '" + ref + "'"); return false; }
            *index = relation.offset + static_cast<std::size_t>(column); found = true;
        }
        if (!found) SetError(error, "unknown column '" + ref + "'");
        return found;
    };

    const TableSchema* base_schema = catalog_.GetTable(statement.table);
    if (!base_schema) { SetError(error, "table '" + statement.table + "' does not exist"); return false; }
    relations.push_back({base_schema, 0,
        statement.table_alias.empty() ? base_schema->name : statement.table_alias});
    width = base_schema->columns.size();
    if (!scan(statement.table, &rows)) return false;

    for (const auto& join : statement.joins) {
        const TableSchema* right_schema = catalog_.GetTable(join.table);
        if (!right_schema) { SetError(error, "table '" + join.table + "' does not exist"); return false; }
        std::vector<std::vector<sql::Value>> right_rows;
        if (!scan(join.table, &right_rows)) return false;
        const std::size_t right_width = right_schema->columns.size();
        relations.push_back({right_schema, width,
            join.alias.empty() ? right_schema->name : join.alias});
        std::size_t left_key = 0, right_key = 0;
        if (join.on) {
            if (!resolve(join.on->left_column, &left_key) ||
                !resolve(join.on->right_column, &right_key)) return false;

            // One ON reference must come from the accumulated left side and
            // the other from the table currently being joined. Accept either
            // written order, but reject comparisons within only one side.
            const bool left_to_right = left_key < width &&
                right_key >= width && right_key < width + right_width;
            const bool right_to_left = right_key < width &&
                left_key >= width && left_key < width + right_width;
            if (!left_to_right && !right_to_left) {
                SetError(error, "JOIN ON must compare a left-side column with a column from '" +
                                join.table + "'");
                return false;
            }
        }
        std::vector<bool> right_matched(right_rows.size(), false);
        std::vector<std::vector<sql::Value>> joined;
        for (const auto& left_row : rows) {
            bool left_matched = false;
            for (std::size_t r = 0; r < right_rows.size(); ++r) {
                bool matches = join.type == sql::JoinType::CROSS;
                if (join.on && left_key < width + right_width &&
                    right_key < width + right_width) {
                    const auto get_value = [&](std::size_t idx) -> const sql::Value* {
                        if (idx < width) return idx < left_row.size() ? &left_row[idx] : nullptr;
                        const std::size_t ri = idx - width;
                        return ri < right_rows[r].size() ? &right_rows[r][ri] : nullptr;
                    };
                    const sql::Value* a = get_value(left_key);
                    const sql::Value* b = get_value(right_key);
                    // ON references are required to compare one existing-side
                    // column with one column from the newly joined table.
                    if (a && b && a->type != sql::Value::Type::NULL_VALUE &&
                        b->type != sql::Value::Type::NULL_VALUE && a->type == b->type)
                        matches = a->type == sql::Value::Type::INTEGER
                            ? a->int_value == b->int_value
                            : a->string_value == b->string_value;
                }
                if (matches) {
                    std::vector<sql::Value> combined = left_row;
                    combined.insert(combined.end(), right_rows[r].begin(), right_rows[r].end());
                    joined.push_back(std::move(combined));
                    left_matched = true; right_matched[r] = true;
                }
            }
            if (!left_matched && (join.type == sql::JoinType::LEFT || join.type == sql::JoinType::FULL_OUTER)) {
                std::vector<sql::Value> combined = left_row;
                auto missing = null_row(right_width);
                combined.insert(combined.end(), missing.begin(), missing.end());
                joined.push_back(std::move(combined));
            }
        }
        if (join.type == sql::JoinType::RIGHT || join.type == sql::JoinType::FULL_OUTER) {
            for (std::size_t r = 0; r < right_rows.size(); ++r) {
                if (right_matched[r]) continue;
                auto combined = null_row(width);
                combined.insert(combined.end(), right_rows[r].begin(), right_rows[r].end());
                joined.push_back(std::move(combined));
            }
        }
        rows = std::move(joined);
        width += right_width;
    }

    auto value_compare = [](const sql::Value& a, const sql::Value& b, sql::CompareOp op) {
        if (a.type == sql::Value::Type::NULL_VALUE || b.type == sql::Value::Type::NULL_VALUE || a.type != b.type) return false;
        int c = a.type == sql::Value::Type::INTEGER
            ? (a.int_value < b.int_value ? -1 : a.int_value > b.int_value ? 1 : 0)
            : (a.string_value < b.string_value ? -1 : a.string_value > b.string_value ? 1 : 0);
        switch (op) { case sql::CompareOp::EQ: return c==0; case sql::CompareOp::NE: return c!=0;
            case sql::CompareOp::LT: return c<0; case sql::CompareOp::GT: return c>0;
            case sql::CompareOp::LE: return c<=0; case sql::CompareOp::GE: return c>=0; }
        return false;
    };
    std::vector<std::vector<sql::Value>> result_rows;
    auto condition_matches = [&](const sql::Condition& condition, const std::vector<sql::Value>& row) {
        if (condition.predicates.empty() || condition.connectors.size() + 1 != condition.predicates.size()) return false;
        auto one = [&](const sql::ConditionPredicate& predicate) {
            if (predicate.aggregate) return false;
            std::size_t index = 0;
            if (!resolve(predicate.column, &index) || index >= row.size()) return false;
            const auto& value = row[index];
            if (value.type == sql::Value::Type::NULL_VALUE && predicate.type != sql::PredicateType::IS_NULL) return false;
            bool matches = false;
            switch (predicate.type) {
                case sql::PredicateType::COMPARISON:
                    matches = predicate.values.size() == 1 && value_compare(value, predicate.values[0], predicate.op); break;
                case sql::PredicateType::IS_NULL: matches = value.type == sql::Value::Type::NULL_VALUE; break;
                case sql::PredicateType::LIKE:
                    matches = value.type == sql::Value::Type::STRING && predicate.values.size() == 1 &&
                        predicate.values[0].type == sql::Value::Type::STRING &&
                        LikeMatches(value.string_value, predicate.values[0].string_value); break;
                case sql::PredicateType::IN:
                    for (const auto& candidate : predicate.values)
                        if (value.type != sql::Value::Type::NULL_VALUE && ValuesEqual(value, candidate)) { matches = true; break; }
                    break;
                case sql::PredicateType::BETWEEN:
                    matches = predicate.values.size() == 2 && value_compare(value, predicate.values[0], sql::CompareOp::GE) &&
                        value_compare(value, predicate.values[1], sql::CompareOp::LE); break;
            }
            return predicate.negated ? !matches : matches;
        };
        bool and_group = true, overall = false;
        for (std::size_t i = 0; i < condition.predicates.size(); ++i) {
            and_group = and_group && one(condition.predicates[i]);
            if (i + 1 == condition.predicates.size() || condition.connectors[i] == sql::LogicalOp::OR) {
                overall = overall || and_group; and_group = true;
            }
        }
        return overall;
    };

    auto evaluate_aggregate = [&](const sql::AggregateExpression& expression,
                                  const std::vector<std::size_t>& members,
                                  sql::Value* output) -> bool {
        output->type = sql::Value::Type::NULL_VALUE;
        std::size_t column_index = 0;
        if (!expression.star && !resolve(expression.column, &column_index)) return false;
        if (expression.function == sql::AggregateFunction::COUNT) {
            std::int64_t count = 0;
            for (const auto member : members) {
                if (expression.star ||
                    (column_index < result_rows[member].size() &&
                     result_rows[member][column_index].type != sql::Value::Type::NULL_VALUE))
                    ++count;
            }
            output->type = sql::Value::Type::INTEGER;
            output->int_value = count;
            return true;
        }

        bool seen = false;
        std::int64_t total = 0;
        sql::Value best;
        for (const auto member : members) {
            if (column_index >= result_rows[member].size()) continue;
            const auto& value = result_rows[member][column_index];
            if (value.type == sql::Value::Type::NULL_VALUE) continue;
            if (expression.function == sql::AggregateFunction::SUM ||
                expression.function == sql::AggregateFunction::AVG) {
                if (value.type != sql::Value::Type::INTEGER) {
                    SetError(error, "SUM and AVG require integer values");
                    return false;
                }
                if ((value.int_value > 0 && total > std::numeric_limits<std::int64_t>::max() - value.int_value) ||
                    (value.int_value < 0 && total < std::numeric_limits<std::int64_t>::min() - value.int_value)) {
                    SetError(error, "aggregate result is out of range");
                    return false;
                }
                total += value.int_value;
            } else if (!seen) {
                best = value;
            } else {
                const int comparison = value.type == sql::Value::Type::INTEGER
                    ? (value.int_value < best.int_value ? -1 : value.int_value > best.int_value ? 1 : 0)
                    : (value.string_value < best.string_value ? -1 : value.string_value > best.string_value ? 1 : 0);
                if ((expression.function == sql::AggregateFunction::MIN && comparison < 0) ||
                    (expression.function == sql::AggregateFunction::MAX && comparison > 0))
                    best = value;
            }
            seen = true;
        }
        if (!seen) return true; // SQL empty/all-NULL aggregate result is NULL (except COUNT).
        if (expression.function == sql::AggregateFunction::SUM) {
            output->type = sql::Value::Type::INTEGER;
            output->int_value = total;
        } else if (expression.function == sql::AggregateFunction::AVG) {
            output->type = sql::Value::Type::INTEGER;
            std::size_t count = 0;
            for (const auto member : members)
                if (column_index < result_rows[member].size() &&
                    result_rows[member][column_index].type != sql::Value::Type::NULL_VALUE) ++count;
            output->int_value = count == 0 ? 0 : total / static_cast<std::int64_t>(count);
        } else {
            *output = std::move(best);
        }
        return true;
    };

    result_rows.reserve(rows.size());
    for (const auto& row : rows) {
        if (!statement.where || condition_matches(*statement.where, row))
            result_rows.push_back(row);
    }

    std::vector<sql::SelectItem> select_items = statement.select_items;
    if (select_items.empty() && !statement.select_all) {
        for (const auto& column : statement.columns)
            select_items.push_back(sql::SelectItem{column, std::nullopt});
    }
    bool has_aggregate = false;
    for (const auto& item : select_items) has_aggregate = has_aggregate || item.aggregate.has_value();
    if (statement.having)
        for (const auto& predicate : statement.having->predicates)
            has_aggregate = has_aggregate || predicate.aggregate.has_value();

    std::vector<std::vector<std::size_t>> groups;
    if (!statement.group_by.empty()) {
        std::vector<std::size_t> group_indices;
        for (const auto& name : statement.group_by) {
            std::size_t index = 0;
            if (!resolve(name, &index)) return false;
            group_indices.push_back(index);
        }
        auto same_group = [&](const std::vector<sql::Value>& left,
                              const std::vector<sql::Value>& right) {
            for (const auto index : group_indices) {
                if (index >= left.size() || index >= right.size()) return false;
                const auto& a = left[index]; const auto& b = right[index];
                if (a.type != b.type) return false;
                if (a.type == sql::Value::Type::NULL_VALUE) continue;
                if (a.type == sql::Value::Type::INTEGER) {
                    if (a.int_value != b.int_value) return false;
                } else if (a.string_value != b.string_value) return false;
            }
            return true;
        };
        for (std::size_t i = 0; i < result_rows.size(); ++i) {
            auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
                return !group.empty() && same_group(result_rows[i], result_rows[group.front()]);
            });
            if (found == groups.end()) groups.push_back({i});
            else found->push_back(i);
        }
    } else if (has_aggregate) {
        std::vector<std::size_t> all;
        for (std::size_t i = 0; i < result_rows.size(); ++i) all.push_back(i);
        groups.push_back(std::move(all)); // aggregate over empty input still yields one row
    } else {
        for (std::size_t i = 0; i < result_rows.size(); ++i) groups.push_back({i});
    }

    auto condition_matches_group = [&](const sql::Condition& condition,
                                       const std::vector<std::size_t>& members) {
        if (condition.predicates.empty() || condition.connectors.size() + 1 != condition.predicates.size()) return false;
        const std::vector<sql::Value> empty_row;
        const auto& row = members.empty() ? empty_row : result_rows[members.front()];
        auto one = [&](const sql::ConditionPredicate& predicate) {
            sql::Value value;
            if (predicate.aggregate) {
                if (!evaluate_aggregate(*predicate.aggregate, members, &value)) return false;
            } else {
                sql::Condition single;
                single.predicates.push_back(predicate);
                return condition_matches(single, row);
            }
            if (predicate.type != sql::PredicateType::COMPARISON || predicate.values.size() != 1)
                return false;
            return value_compare(value, predicate.values[0], predicate.op);
        };
        bool and_group = true, overall = false;
        for (std::size_t i = 0; i < condition.predicates.size(); ++i) {
            and_group = and_group && one(condition.predicates[i]);
            if (i + 1 == condition.predicates.size() || condition.connectors[i] == sql::LogicalOp::OR) {
                overall = overall || and_group;
                and_group = true;
            }
        }
        return overall;
    };

    if (statement.having) {
        groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const auto& group) {
            return !condition_matches_group(*statement.having, group);
        }), groups.end());
        if (error != nullptr && !error->empty()) return false;
    }

    QueryResult result;
    std::vector<std::size_t> projection;
    if (statement.select_all) {
        for (std::size_t i = 0; i < width; ++i) projection.push_back(i);
        for (std::size_t idx : projection) {
            std::string name;
            for (const auto& relation : relations)
                if (idx >= relation.offset && idx < relation.offset + relation.schema->columns.size())
                    name = relation.schema->columns[idx - relation.offset].name;
            result.column_names.push_back(name);
        }
    } else {
        for (const auto& item : select_items) {
            if (item.aggregate) {
                const auto& aggregate = *item.aggregate;
                const char* fn = aggregate.function == sql::AggregateFunction::COUNT ? "COUNT" :
                    aggregate.function == sql::AggregateFunction::SUM ? "SUM" :
                    aggregate.function == sql::AggregateFunction::AVG ? "AVG" :
                    aggregate.function == sql::AggregateFunction::MIN ? "MIN" : "MAX";
                result.column_names.push_back(std::string(fn) + "(" + (aggregate.star ? "*" : aggregate.column) + ")");
            } else {
                std::size_t index = 0;
                if (!resolve(item.column, &index)) return false;
                projection.push_back(index);
                const auto dot = item.column.find('.');
                result.column_names.push_back(dot == std::string::npos ? item.column : item.column.substr(dot + 1));
            }
        }
    }

    struct OutputRow { std::vector<sql::Value> projected; std::vector<sql::Value> sort_values; };
    std::vector<std::size_t> order_indices;
    for (const auto& item : statement.order_by) {
        std::size_t index = 0;
        if (!resolve(item.column, &index)) return false;
        order_indices.push_back(index);
    }
    std::vector<OutputRow> output_rows;
    for (const auto& group : groups) {
        OutputRow output;
        const std::vector<sql::Value> empty_row;
        const auto& representative = group.empty() ? empty_row : result_rows[group.front()];
        if (statement.select_all) {
            output.projected = representative;
        } else {
            std::size_t plain_index = 0;
            for (const auto& item : select_items) {
                if (item.aggregate) {
                    sql::Value value;
                    if (!evaluate_aggregate(*item.aggregate, group, &value)) return false;
                    output.projected.push_back(std::move(value));
                } else {
                    if (plain_index >= projection.size() || projection[plain_index] >= representative.size()) {
                        SetError(error, "cannot project a non-aggregate column from an empty aggregate group");
                        return false;
                    }
                    output.projected.push_back(representative[projection[plain_index++]]);
                }
            }
        }
        for (const auto index : order_indices) {
            if (index >= representative.size()) { SetError(error, "ORDER BY column unavailable"); return false; }
            output.sort_values.push_back(representative[index]);
        }
        output_rows.push_back(std::move(output));
    }

    auto value_equal = [](const sql::Value& a, const sql::Value& b) {
        if (a.type != b.type) return false;
        if (a.type == sql::Value::Type::NULL_VALUE) return true;
        return a.type == sql::Value::Type::INTEGER
            ? a.int_value == b.int_value : a.string_value == b.string_value;
    };
    if (statement.distinct) {
        std::vector<OutputRow> unique_rows;
        unique_rows.reserve(output_rows.size());
        for (auto& candidate : output_rows) {
            bool duplicate = false;
            for (const auto& existing : unique_rows) {
                if (candidate.projected.size() != existing.projected.size()) continue;
                bool same = true;
                for (std::size_t i = 0; i < candidate.projected.size(); ++i) {
                    if (!value_equal(candidate.projected[i], existing.projected[i])) {
                        same = false;
                        break;
                    }
                }
                if (same) { duplicate = true; break; }
            }
            if (!duplicate) unique_rows.push_back(std::move(candidate));
        }
        output_rows = std::move(unique_rows);
    }

    if (!statement.order_by.empty()) {
        std::stable_sort(output_rows.begin(), output_rows.end(),
            [&](const OutputRow& a, const OutputRow& b) {
                for (std::size_t i = 0; i < statement.order_by.size(); ++i) {
                    const auto& left = a.sort_values[i];
                    const auto& right = b.sort_values[i];
                    int comparison = 0;
                    if (left.type != right.type) {
                        comparison = static_cast<int>(left.type) < static_cast<int>(right.type) ? -1 : 1;
                    } else if (left.type == sql::Value::Type::NULL_VALUE) {
                        comparison = 0;
                    } else if (left.type == sql::Value::Type::INTEGER) {
                        comparison = left.int_value < right.int_value ? -1 :
                                     left.int_value > right.int_value ? 1 : 0;
                    } else {
                        comparison = left.string_value < right.string_value ? -1 :
                                     left.string_value > right.string_value ? 1 : 0;
                    }
                    if (comparison != 0)
                        return statement.order_by[i].descending ? comparison > 0 : comparison < 0;
                }
                return false;
            });
    }

    const std::size_t begin = std::min(statement.offset, output_rows.size());
    const std::size_t available = output_rows.size() - begin;
    const std::size_t count = statement.limit ? std::min(*statement.limit, available) : available;
    result.rows.reserve(count);
    for (std::size_t i = begin; i < begin + count; ++i)
        result.rows.push_back(std::move(output_rows[i].projected));

    *query_result = std::move(result);
    return true;
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

            const auto original = std::find_if(
                stored_rows.begin(), stored_rows.end(),
                [&](const auto& stored) { return stored.first == update.first; });
            if (original == stored_rows.end()) continue;

            for (const auto& index : catalog_.Indexes()) {
                if (NormalizeIdentifier(index.table_name) !=
                    NormalizeIdentifier(statement.table)) continue;
                const int column = schema->ColumnIndex(index.column_name);
                const auto tree = indexes_.find(NormalizeIdentifier(index.name));
                if (column < 0 || tree == indexes_.end()) continue;
                const auto position = static_cast<std::size_t>(column);
                const auto old_key = original->second[position].int_value;
                const auto new_key = update.second[position].int_value;
                if (old_key != new_key) {
                    tree->second->Remove(old_key, update.first);
                    if (!tree->second->Insert(new_key, update.first)) {
                        SetError(error, "failed to update index '" + index.name +
                                        "' after UPDATE");
                        return false;
                    }
                }
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
            const auto stored = std::find_if(
                stored_rows.begin(), stored_rows.end(),
                [&](const auto& row) { return row.first == rid; });
            if (!table_it->second.record_table->Delete(rid)) {
                SetError(error, "failed to delete a matching row");
                return false;
            }
            if (stored != stored_rows.end()) {
                for (const auto& index : catalog_.Indexes()) {
                    if (NormalizeIdentifier(index.table_name) !=
                        NormalizeIdentifier(statement.table)) continue;
                    const int column = schema->ColumnIndex(index.column_name);
                    const auto tree = indexes_.find(NormalizeIdentifier(index.name));
                    if (column >= 0 && tree != indexes_.end()) {
                        tree->second->Remove(
                            stored->second[static_cast<std::size_t>(column)].int_value,
                            rid);
                    }
                }
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