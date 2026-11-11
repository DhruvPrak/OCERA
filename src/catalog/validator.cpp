#include "catalog/validator.h"

#include <set>

namespace ocera {

namespace {

using sql::ColumnType;
using sql::Value;

bool TypesMatch(ColumnType column, Value::Type value) {
    return (column == ColumnType::INT && value == Value::Type::INTEGER) ||
           (column == ColumnType::TEXT && value == Value::Type::STRING);
}

const char* ColumnTypeName(ColumnType t) {
    return t == ColumnType::INT ? "INT" : "TEXT";
}

const char* ValueTypeName(Value::Type t) {
    return t == Value::Type::INTEGER ? "an integer" : "a string";
}

std::string At(std::size_t position) {
    return " (at position " + std::to_string(position) + ")";
}

const TableSchema& RequireTable(const Catalog& catalog, const std::string& name) {
    const TableSchema* table = catalog.GetTable(name);
    if (!table) throw SemanticError("table '" + name + "' does not exist");
    return *table;
}

const ColumnSchema* RequireColumn(const TableSchema& table,
                                  const std::string& column,
                                  const std::string& where_suffix) {
    const int index = table.ColumnIndex(column);
    if (index < 0) {
        throw SemanticError("table '" + table.name + "' has no column '" +
                            column + "'" + where_suffix);
    }
    return &table.columns[static_cast<std::size_t>(index)];
}

void CheckValueType(const ColumnSchema& col, const Value& value,
                    const std::string& suffix) {
    if (!TypesMatch(col.type, value.type)) {
        throw SemanticError(std::string("column '") + col.name + "' is " +
                            ColumnTypeName(col.type) + " but got " +
                            ValueTypeName(value.type) + suffix);
    }
}

void CheckCondition(const sql::Condition& cond, const TableSchema& table) {
    if (cond.predicates.empty() ||
        cond.connectors.size() + 1 != cond.predicates.size()) {
        throw SemanticError("malformed WHERE condition");
    }

    for (const auto& predicate : cond.predicates) {
        const ColumnSchema* col =
            RequireColumn(table, predicate.column, At(predicate.position));
        switch (predicate.type) {
            case sql::PredicateType::COMPARISON:
                if (predicate.values.size() != 1)
                    throw SemanticError("comparison requires exactly one value" +
                                        At(predicate.position));
                CheckValueType(*col, predicate.values[0],
                               At(predicate.values[0].position));
                break;
            case sql::PredicateType::IS_NULL:
                if (!predicate.values.empty())
                    throw SemanticError("IS NULL does not take a value" +
                                        At(predicate.position));
                // The current record format has no NULL representation.
                break;
            case sql::PredicateType::LIKE:
                if (col->type != ColumnType::TEXT)
                    throw SemanticError("LIKE requires a TEXT column" +
                                        At(predicate.position));
                if (predicate.values.size() != 1 ||
                    predicate.values[0].type != Value::Type::STRING)
                    throw SemanticError("LIKE requires one string pattern" +
                                        At(predicate.position));
                break;
            case sql::PredicateType::IN:
                if (predicate.values.empty())
                    throw SemanticError("IN requires at least one value" +
                                        At(predicate.position));
                for (const auto& value : predicate.values)
                    CheckValueType(*col, value, At(value.position));
                break;
            case sql::PredicateType::BETWEEN:
                if (predicate.values.size() != 2)
                    throw SemanticError("BETWEEN requires two bounds" +
                                        At(predicate.position));
                CheckValueType(*col, predicate.values[0],
                               At(predicate.values[0].position));
                CheckValueType(*col, predicate.values[1],
                               At(predicate.values[1].position));
                break;
        }
    }
}

void CheckOptionalCondition(const std::optional<sql::Condition>& cond,
                            const TableSchema& table) {
    if (cond) CheckCondition(*cond, table);
}

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

bool HasCandidateKey(const sql::CreateTableStatement& stmt,
                     const std::vector<std::string>& columns) {
    for (const auto& key : stmt.key_constraints) {
        if (SameIdentifierTuple(key.columns, columns)) return true;
    }
    return false;
}

const sql::ColumnDefinition* FindColumnDefinition(
    const sql::CreateTableStatement& stmt, const std::string& name) {
    for (const auto& column : stmt.columns) {
        if (NormalizeIdentifier(column.name) == NormalizeIdentifier(name))
            return &column;
    }
    return nullptr;
}

int FindColumnIndex(const sql::CreateTableStatement& stmt,
                    const std::string& name) {
    for (std::size_t i = 0; i < stmt.columns.size(); ++i) {
        if (NormalizeIdentifier(stmt.columns[i].name) == NormalizeIdentifier(name))
            return static_cast<int>(i);
    }
    return -1;
}

void ValidateCreateIndex(const sql::CreateIndexStatement& stmt,
                         const Catalog& catalog) {
    if (stmt.index_name.empty()) throw SemanticError("index name is empty");
    if (catalog.HasIndex(stmt.index_name)) {
        throw SemanticError("index '" + stmt.index_name + "' already exists");
    }

    const TableSchema& table = RequireTable(catalog, stmt.table);
    const ColumnSchema* column = RequireColumn(table, stmt.column, "");
    if (column->type != ColumnType::INT) {
        throw SemanticError("index '" + stmt.index_name +
                            "' requires an INT column; '" + column->name + "' is TEXT");
    }
    if (catalog.GetIndexForColumn(table.name, column->name) != nullptr) {
        throw SemanticError("an index already exists for column '" + column->name +
                            "' on table '" + table.name + "'");
    }
}

}  // namespace

void Validate(const sql::Statement& stmt, const Catalog& catalog) {
    if (const auto* s = std::get_if<sql::CreateTableStatement>(&stmt)) {
        if (catalog.HasTable(s->table)) {
            throw SemanticError("table '" + s->table + "' already exists");
        }
        std::set<std::string> seen;
        for (const auto& col : s->columns) {
            if (!seen.insert(NormalizeIdentifier(col.name)).second) {
                throw SemanticError("duplicate column name '" + col.name + "'" +
                                    At(col.position));
            }
        }
        bool has_primary_key = false;
        std::set<std::string> seen_constraints;
        for (const auto& constraint : s->key_constraints) {
            if (constraint.columns.empty())
                throw SemanticError("key constraint must contain at least one column" +
                                    At(constraint.position));
            if (constraint.type == sql::KeyConstraintType::PRIMARY_KEY) {
                if (has_primary_key)
                    throw SemanticError("table '" + s->table +
                                        "' has more than one PRIMARY KEY" +
                                        At(constraint.position));
                has_primary_key = true;
            }
            std::set<std::string> constraint_columns;
            std::string signature =
                constraint.type == sql::KeyConstraintType::PRIMARY_KEY ? "P:" : "U:";
            for (const auto& name : constraint.columns) {
                const std::string normalized = NormalizeIdentifier(name);
                if (!seen.count(normalized))
                    throw SemanticError("key constraint references unknown column '" +
                                        name + "'" + At(constraint.position));
                if (!constraint_columns.insert(normalized).second)
                    throw SemanticError("key constraint contains duplicate column '" +
                                        name + "'" + At(constraint.position));
                signature += normalized + ";";
            }
            if (!seen_constraints.insert(signature).second)
                throw SemanticError("duplicate key constraint on table '" + s->table +
                                    "'" + At(constraint.position));
        }

        std::set<std::string> seen_foreign_keys;
        for (const auto& constraint : s->foreign_key_constraints) {
            if (constraint.columns.empty() || constraint.referenced_columns.empty())
                throw SemanticError("foreign-key constraint must contain at least one column" +
                                    At(constraint.position));
            if (constraint.columns.size() != constraint.referenced_columns.size())
                throw SemanticError("foreign-key column count does not match referenced column count" +
                                    At(constraint.position));

            std::set<std::string> local_seen;
            std::set<std::string> referenced_seen;
            for (const auto& name : constraint.columns) {
                if (!seen.count(NormalizeIdentifier(name)))
                    throw SemanticError("foreign key references unknown column '" + name + "'" +
                                        At(constraint.position));
                if (!local_seen.insert(NormalizeIdentifier(name)).second)
                    throw SemanticError("foreign key contains duplicate column '" + name + "'" +
                                        At(constraint.position));
            }

            const bool self_reference =
                NormalizeIdentifier(constraint.referenced_table) ==
                NormalizeIdentifier(s->table);
            const TableSchema* parent = self_reference
                ? nullptr : catalog.GetTable(constraint.referenced_table);
            if (!self_reference && !parent)
                throw SemanticError("referenced table '" + constraint.referenced_table +
                                    "' does not exist" + At(constraint.position));

            for (const auto& name : constraint.referenced_columns) {
                const int index = self_reference
                    ? FindColumnIndex(*s, name)
                    : parent->ColumnIndex(name);
                if (index < 0)
                    throw SemanticError("referenced table '" + constraint.referenced_table +
                                        "' has no column '" + name + "'" +
                                        At(constraint.position));
                if (!referenced_seen.insert(NormalizeIdentifier(name)).second)
                    throw SemanticError("foreign key contains duplicate referenced column '" +
                                        name + "'" + At(constraint.position));
            }

            const bool candidate_key = self_reference
                ? HasCandidateKey(*s, constraint.referenced_columns)
                : HasCandidateKey(*parent, constraint.referenced_columns);
            if (!candidate_key)
                throw SemanticError("referenced columns on table '" +
                                    constraint.referenced_table +
                                    "' must match a PRIMARY KEY or UNIQUE constraint" +
                                    At(constraint.position));

            for (std::size_t i = 0; i < constraint.columns.size(); ++i) {
                const auto* local = FindColumnDefinition(*s, constraint.columns[i]);
                const sql::ColumnType parent_type = self_reference
                    ? FindColumnDefinition(*s, constraint.referenced_columns[i])->type
                    : parent->columns[static_cast<std::size_t>(
                          parent->ColumnIndex(constraint.referenced_columns[i]))].type;
                if (local->type != parent_type)
                    throw SemanticError("foreign-key column '" + constraint.columns[i] +
                                        "' has a different type from referenced column '" +
                                        constraint.referenced_columns[i] + "'" +
                                        At(constraint.position));
            }

            const std::string parent_name =
                self_reference ? NormalizeIdentifier(s->table)
                               : NormalizeIdentifier(parent->name);
            std::string signature = parent_name + ":";
            for (const auto& name : constraint.columns)
                signature += NormalizeIdentifier(name) + ";";
            signature += "->";
            for (const auto& name : constraint.referenced_columns)
                signature += NormalizeIdentifier(name) + ";";
            if (!seen_foreign_keys.insert(signature).second)
                throw SemanticError("duplicate foreign-key constraint on table '" +
                                    s->table + "'" + At(constraint.position));
        }
    } else if (const auto* s = std::get_if<sql::CreateIndexStatement>(&stmt)) {
        ValidateCreateIndex(*s, catalog);
    } else if (const auto* s = std::get_if<sql::InsertStatement>(&stmt)) {
        const TableSchema& table = RequireTable(catalog, s->table);
        if (s->values.size() != table.columns.size()) {
            throw SemanticError("table '" + table.name + "' has " +
                                std::to_string(table.columns.size()) + " column(s) but " +
                                std::to_string(s->values.size()) + " value(s) were given");
        }
        for (std::size_t i = 0; i < s->values.size(); ++i) {
            CheckValueType(table.columns[i], s->values[i], At(s->values[i].position));
        }
    } else if (const auto* s = std::get_if<sql::SelectStatement>(&stmt)) {
        std::vector<const TableSchema*> tables;
        tables.push_back(&RequireTable(catalog, s->table));
        for (const auto& join : s->joins)
            tables.push_back(&RequireTable(catalog, join.table));

        auto resolve = [&](const std::string& reference) -> const ColumnSchema* {
            const auto dot = reference.find('.');
            if (dot != std::string::npos) {
                const std::string table_name = reference.substr(0, dot);
                const std::string column_name = reference.substr(dot + 1);
                for (const auto* table : tables) {
                    if (NormalizeIdentifier(table->name) == NormalizeIdentifier(table_name)) {
                        const int index = table->ColumnIndex(column_name);
                        if (index >= 0) return &table->columns[static_cast<std::size_t>(index)];
                        throw SemanticError("table '" + table_name + "' has no column '" + column_name + "'");
                    }
                }
                throw SemanticError("unknown table qualifier '" + table_name + "'");
            }
            const ColumnSchema* found = nullptr;
            for (const auto* table : tables) {
                const int index = table->ColumnIndex(reference);
                if (index >= 0) {
                    if (found != nullptr)
                        throw SemanticError("ambiguous column '" + reference + "'");
                    found = &table->columns[static_cast<std::size_t>(index)];
                }
            }
            if (!found) throw SemanticError("unknown column '" + reference + "'");
            return found;
        };

        for (const auto& join : s->joins) {
            if (join.type == sql::JoinType::CROSS) {
                if (join.on) throw SemanticError("CROSS JOIN must not have an ON clause");
            } else if (!join.on) {
                throw SemanticError("JOIN requires an ON clause");
            } else {
                const auto* left = resolve(join.on->left_column);
                const auto* right = resolve(join.on->right_column);
                if (left->type != right->type)
                    throw SemanticError("JOIN columns must have matching data types");
            }
        }
        if (!s->select_all)
            for (const auto& name : s->columns) (void)resolve(name);
        if (s->where) {
            if (s->where->predicates.empty() ||
                s->where->connectors.size() + 1 != s->where->predicates.size())
                throw SemanticError("malformed WHERE condition");
            for (const auto& predicate : s->where->predicates) {
                const auto* column = resolve(predicate.column);
                if (predicate.type == sql::PredicateType::COMPARISON ||
                    predicate.type == sql::PredicateType::LIKE) {
                    if (predicate.values.size() != 1)
                        throw SemanticError("malformed WHERE predicate");
                    if (predicate.type == sql::PredicateType::COMPARISON)
                        CheckValueType(*column, predicate.values[0], At(predicate.position));
                    else if (column->type != sql::ColumnType::TEXT ||
                             predicate.values[0].type != sql::Value::Type::STRING)
                        throw SemanticError("LIKE requires TEXT values" + At(predicate.position));
                } else if (predicate.type == sql::PredicateType::IN) {
                    for (const auto& value : predicate.values)
                        CheckValueType(*column, value, At(predicate.position));
                } else if (predicate.type == sql::PredicateType::BETWEEN) {
                    if (predicate.values.size() != 2)
                        throw SemanticError("BETWEEN requires two values" + At(predicate.position));
                    for (const auto& value : predicate.values)
                        CheckValueType(*column, value, At(predicate.position));
                }
            }
        }
    } else if (const auto* s = std::get_if<sql::UpdateStatement>(&stmt)) {
        const TableSchema& table = RequireTable(catalog, s->table);
        for (const auto& assignment : s->assignments) {
            const ColumnSchema* col =
                RequireColumn(table, assignment.column, At(assignment.position));
            CheckValueType(*col, assignment.value, At(assignment.value.position));
        }
        CheckOptionalCondition(s->where, table);
    } else if (const auto* s = std::get_if<sql::DeleteStatement>(&stmt)) {
        const TableSchema& table = RequireTable(catalog, s->table);
        CheckOptionalCondition(s->where, table);
    }
}

}  // namespace ocera
