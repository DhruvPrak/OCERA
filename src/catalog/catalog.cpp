#include "catalog/catalog.h"

#include <cctype>
#include <set>
#include <utility>

namespace ocera {
namespace {

bool SameIdentifierTuple(const std::vector<std::string>& lhs,
                         const std::vector<std::string>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (NormalizeIdentifier(lhs[i]) != NormalizeIdentifier(rhs[i])) return false;
    }
    return true;
}

bool HasCandidateKey(const TableSchema& table,
                     const std::vector<std::string>& columns) {
    for (const auto& key : table.key_constraints) {
        if (SameIdentifierTuple(key.columns, columns)) return true;
    }
    return false;
}

}  // namespace

std::string NormalizeIdentifier(const std::string& name) {
    std::string out = name;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

int TableSchema::ColumnIndex(const std::string& column_name) const {
    const std::string key = NormalizeIdentifier(column_name);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (NormalizeIdentifier(columns[i].name) == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Catalog::CreateTable(const sql::CreateTableStatement& stmt) {
    if (stmt.table.empty()) throw CatalogError("table name is empty");
    if (stmt.columns.empty()) {
        throw CatalogError("table '" + stmt.table + "' must have at least one column");
    }

    const std::string key = NormalizeIdentifier(stmt.table);
    if (tables_.count(key)) {
        throw CatalogError("table '" + stmt.table + "' already exists");
    }

    // Construct first; publish only after every column and key has passed validation.
    TableSchema schema;
    schema.name = stmt.table;
    std::set<std::string> seen;
    for (const auto& col : stmt.columns) {
        if (col.name.empty()) throw CatalogError("column name is empty");
        if (!seen.insert(NormalizeIdentifier(col.name)).second) {
            throw CatalogError("duplicate column name '" + col.name + "' in table '" +
                               stmt.table + "'");
        }
        schema.columns.push_back({col.name, col.type});
    }

    bool has_primary_key = false;
    std::set<std::string> seen_constraints;
    for (const auto& constraint : stmt.key_constraints) {
        if (constraint.columns.empty())
            throw CatalogError("key constraint must contain at least one column");
        if (constraint.type == sql::KeyConstraintType::PRIMARY_KEY) {
            if (has_primary_key)
                throw CatalogError("table '" + stmt.table + "' has more than one PRIMARY KEY");
            has_primary_key = true;
        }
        std::set<std::string> constraint_columns;
        std::vector<std::string> canonical_columns;
        for (const auto& column_name : constraint.columns) {
            const int index = schema.ColumnIndex(column_name);
            if (index < 0)
                throw CatalogError("key constraint references unknown column '" + column_name + "'");
            const std::string canonical = NormalizeIdentifier(
                schema.columns[static_cast<std::size_t>(index)].name);
            if (!constraint_columns.insert(canonical).second)
                throw CatalogError("key constraint contains duplicate column '" + column_name + "'");
            canonical_columns.push_back(schema.columns[static_cast<std::size_t>(index)].name);
        }
        std::string signature = constraint.type == sql::KeyConstraintType::PRIMARY_KEY ? "P:" : "U:";
        for (const auto& column_name : canonical_columns) signature += NormalizeIdentifier(column_name) + ";";
        if (!seen_constraints.insert(signature).second)
            throw CatalogError("duplicate key constraint on table '" + stmt.table + "'");
        schema.key_constraints.push_back({constraint.type, std::move(canonical_columns)});
    }

    std::set<std::string> seen_foreign_keys;
    for (const auto& constraint : stmt.foreign_key_constraints) {
        if (constraint.columns.empty() || constraint.referenced_columns.empty()) {
            throw CatalogError("foreign-key constraint must contain at least one column");
        }
        if (constraint.columns.size() != constraint.referenced_columns.size()) {
            throw CatalogError("foreign-key column count does not match referenced column count");
        }

        std::vector<std::string> local_columns;
        std::vector<std::string> referenced_columns;
        std::set<std::string> local_seen;
        std::set<std::string> referenced_seen;
        for (const auto& name : constraint.columns) {
            const int index = schema.ColumnIndex(name);
            if (index < 0) {
                throw CatalogError("foreign key references unknown column '" + name + "'");
            }
            const std::string canonical = schema.columns[static_cast<std::size_t>(index)].name;
            if (!local_seen.insert(NormalizeIdentifier(canonical)).second) {
                throw CatalogError("foreign key contains duplicate column '" + name + "'");
            }
            local_columns.push_back(canonical);
        }

        const TableSchema* referenced_table = nullptr;
        const std::string referenced_key = NormalizeIdentifier(constraint.referenced_table);
        if (referenced_key == key) {
            referenced_table = &schema;  // Permit a self-referencing foreign key.
        } else {
            referenced_table = GetTable(constraint.referenced_table);
        }
        if (!referenced_table) {
            throw CatalogError("referenced table '" + constraint.referenced_table +
                               "' does not exist");
        }

        for (const auto& name : constraint.referenced_columns) {
            const int index = referenced_table->ColumnIndex(name);
            if (index < 0) {
                throw CatalogError("referenced table '" + referenced_table->name +
                                   "' has no column '" + name + "'");
            }
            const std::string canonical =
                referenced_table->columns[static_cast<std::size_t>(index)].name;
            if (!referenced_seen.insert(NormalizeIdentifier(canonical)).second) {
                throw CatalogError("foreign key contains duplicate referenced column '" +
                                   name + "'");
            }
            referenced_columns.push_back(canonical);
        }

        if (!HasCandidateKey(*referenced_table, referenced_columns)) {
            throw CatalogError("referenced columns on table '" + referenced_table->name +
                               "' must match a PRIMARY KEY or UNIQUE constraint");
        }

        for (std::size_t i = 0; i < local_columns.size(); ++i) {
            const int local_index = schema.ColumnIndex(local_columns[i]);
            const int parent_index = referenced_table->ColumnIndex(referenced_columns[i]);
            if (schema.columns[static_cast<std::size_t>(local_index)].type !=
                referenced_table->columns[static_cast<std::size_t>(parent_index)].type) {
                throw CatalogError("foreign-key column '" + local_columns[i] +
                                   "' has a different type from referenced column '" +
                                   referenced_columns[i] + "'");
            }
        }

        std::string signature = NormalizeIdentifier(referenced_table->name) + ":";
        for (const auto& name : local_columns) signature += NormalizeIdentifier(name) + ";";
        signature += "->";
        for (const auto& name : referenced_columns) signature += NormalizeIdentifier(name) + ";";
        if (!seen_foreign_keys.insert(signature).second) {
            throw CatalogError("duplicate foreign-key constraint on table '" + stmt.table + "'");
        }

        schema.foreign_key_constraints.push_back(
            {std::move(local_columns), referenced_table->name,
             std::move(referenced_columns)});
    }
    tables_.emplace(key, std::move(schema));
}

bool Catalog::SetTableFirstPageId(const std::string& name, page_id_t page_id) {
    auto it = tables_.find(NormalizeIdentifier(name));
    if (it == tables_.end() || page_id < 2) return false;
    it->second.first_page_id = page_id;
    return true;
}

bool Catalog::RemoveTable(const std::string& name) {
    return tables_.erase(NormalizeIdentifier(name)) != 0;
}

bool Catalog::HasTable(const std::string& name) const {
    return tables_.count(NormalizeIdentifier(name)) != 0;
}

const TableSchema* Catalog::GetTable(const std::string& name) const {
    auto it = tables_.find(NormalizeIdentifier(name));
    return it == tables_.end() ? nullptr : &it->second;
}

std::vector<std::string> Catalog::TableNames() const {
    std::vector<std::string> names;
    for (const auto& kv : tables_) names.push_back(kv.second.name);
    return names;
}

void Catalog::CreateIndex(const sql::CreateIndexStatement& stmt) {
    if (stmt.index_name.empty()) throw CatalogError("index name is empty");

    const std::string index_key = NormalizeIdentifier(stmt.index_name);
    if (indexes_.count(index_key)) {
        throw CatalogError("index '" + stmt.index_name + "' already exists");
    }

    const TableSchema* table = GetTable(stmt.table);
    if (!table) {
        throw CatalogError("table '" + stmt.table + "' does not exist");
    }

    const int column_index = table->ColumnIndex(stmt.column);
    if (column_index < 0) {
        throw CatalogError("table '" + table->name + "' has no column '" +
                           stmt.column + "'");
    }

    const ColumnSchema& column =
        table->columns[static_cast<std::size_t>(column_index)];
    if (column.type != sql::ColumnType::INT) {
        throw CatalogError("index '" + stmt.index_name +
                           "' requires an INT column; '" + column.name + "' is TEXT");
    }

    if (GetIndexForColumn(table->name, column.name) != nullptr) {
        throw CatalogError("an index already exists for column '" + column.name +
                           "' on table '" + table->name + "'");
    }

    // Publish only after all checks succeed.
    IndexSchema schema{stmt.index_name, table->name, column.name};
    indexes_.emplace(index_key, std::move(schema));
}

bool Catalog::RemoveIndex(const std::string& name) {
    return indexes_.erase(NormalizeIdentifier(name)) != 0;
}

bool Catalog::HasIndex(const std::string& name) const {
    return indexes_.count(NormalizeIdentifier(name)) != 0;
}

const IndexSchema* Catalog::GetIndex(const std::string& name) const {
    auto it = indexes_.find(NormalizeIdentifier(name));
    return it == indexes_.end() ? nullptr : &it->second;
}

const IndexSchema* Catalog::GetIndexForColumn(const std::string& table,
                                               const std::string& column) const {
    const std::string table_key = NormalizeIdentifier(table);
    const std::string column_key = NormalizeIdentifier(column);
    for (const auto& kv : indexes_) {
        const IndexSchema& index = kv.second;
        if (NormalizeIdentifier(index.table_name) == table_key &&
            NormalizeIdentifier(index.column_name) == column_key) {
            return &index;
        }
    }
    return nullptr;
}

std::vector<IndexSchema> Catalog::Indexes() const {
    std::vector<IndexSchema> result;
    result.reserve(indexes_.size());

    for (const auto& entry : indexes_) {
        result.push_back(entry.second);
    }

    return result;
}

}  // namespace ocera
