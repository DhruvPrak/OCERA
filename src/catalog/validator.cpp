#include "catalog/validator.h"

#include <algorithm>
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
        struct SelectRelation {
            const TableSchema* schema;
            std::string qualifier;
        };
        std::vector<SelectRelation> relations;
        std::set<std::string> qualifiers;
        auto add_relation = [&](const std::string& table_name,
                                const std::string& alias) {
            const TableSchema& schema = RequireTable(catalog, table_name);
            const std::string qualifier = alias.empty() ? schema.name : alias;
            const std::string normalized = NormalizeIdentifier(qualifier);
            if (!qualifiers.insert(normalized).second)
                throw SemanticError("duplicate table qualifier '" + qualifier + "'");
            relations.push_back({&schema, qualifier});
        };
        add_relation(s->table, s->table_alias);
        for (const auto& join : s->joins)
            add_relation(join.table, join.alias);

        auto resolve = [&](const std::string& reference) -> const ColumnSchema* {
            const auto dot = reference.find('.');
            if (dot != std::string::npos) {
                const std::string table_name = reference.substr(0, dot);
                const std::string column_name = reference.substr(dot + 1);
                for (const auto& relation : relations) {
                    if (NormalizeIdentifier(relation.qualifier) == NormalizeIdentifier(table_name)) {
                        const int index = relation.schema->ColumnIndex(column_name);
                        if (index >= 0) return &relation.schema->columns[static_cast<std::size_t>(index)];
                        throw SemanticError("table qualifier '" + table_name + "' has no column '" + column_name + "'");
                    }
                }
                throw SemanticError("unknown table qualifier '" + table_name + "'");
            }
            const ColumnSchema* found = nullptr;
            for (const auto& relation : relations) {
                const int index = relation.schema->ColumnIndex(reference);
                if (index >= 0) {
                    if (found != nullptr)
                        throw SemanticError("ambiguous column '" + reference + "'");
                    found = &relation.schema->columns[static_cast<std::size_t>(index)];
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
        std::vector<const ColumnSchema*> group_columns;
        for (const auto& name : s->group_by)
            group_columns.push_back(resolve(name));

        auto require_grouped = [&](const std::string& name, const char* clause) {
            const auto* column = resolve(name);
            if (std::find(group_columns.begin(), group_columns.end(), column) ==
                group_columns.end())
                throw SemanticError(std::string(clause) + " column '" + name +
                                    "' must appear in GROUP BY");
        };

        auto validate_aggregate = [&](const sql::AggregateExpression& aggregate) {
            if (aggregate.star) {
                if (aggregate.function != sql::AggregateFunction::COUNT)
                    throw SemanticError("only COUNT accepts '*'");
                return;
            }
            const auto* column = resolve(aggregate.column);
            if ((aggregate.function == sql::AggregateFunction::SUM ||
                 aggregate.function == sql::AggregateFunction::AVG) &&
                column->type != ColumnType::INT)
                throw SemanticError("SUM and AVG require an INT column" + At(aggregate.position));
        };
        bool has_aggregate = false;
        if (!s->select_items.empty()) {
            for (const auto& item : s->select_items) {
                if (item.aggregate) {
                    has_aggregate = true;
                    validate_aggregate(*item.aggregate);
                }
            }
        }
        if (s->having) {
            for (const auto& predicate : s->having->predicates) {
                if (predicate.aggregate) {
                    has_aggregate = true;
                    validate_aggregate(*predicate.aggregate);
                }
            }
        }

        if (!s->group_by.empty()) {
            if (s->select_all) {
                for (const auto& relation : relations)
                    for (const auto& column : relation.schema->columns)
                        require_grouped(relation.qualifier + "." + column.name, "SELECT");
            } else if (!s->select_items.empty()) {
                for (const auto& item : s->select_items)
                    if (!item.aggregate) require_grouped(item.column, "SELECT");
            } else {
                for (const auto& name : s->columns) require_grouped(name, "SELECT");
            }
            for (const auto& item : s->order_by)
                require_grouped(item.column, "ORDER BY");
        } else if (s->having && !has_aggregate) {
            throw SemanticError("HAVING requires GROUP BY or an aggregate function");
        }
        if (has_aggregate && s->select_all)
            throw SemanticError("SELECT * cannot be combined with aggregate functions");
        if (has_aggregate && s->group_by.empty()) {
            const bool has_plain_projection = !s->select_items.empty() &&
                std::any_of(s->select_items.begin(), s->select_items.end(),
                    [](const auto& item) { return !item.aggregate.has_value(); });
            if (has_plain_projection)
                throw SemanticError("non-aggregate SELECT columns require GROUP BY");
        }

        if (has_aggregate && s->group_by.empty() && !s->order_by.empty())
            throw SemanticError("ORDER BY columns in an aggregate query require GROUP BY");
        for (const auto& item : s->order_by) (void)resolve(item.column);
        if (s->where) {
            if (s->where->predicates.empty() ||
                s->where->connectors.size() + 1 != s->where->predicates.size())
                throw SemanticError("malformed WHERE condition");
            for (const auto& predicate : s->where->predicates) {
                if (predicate.aggregate)
                    throw SemanticError("aggregate functions are not allowed in WHERE" + At(predicate.position));
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
        if (s->having) {
            if (s->having->predicates.empty() ||
                s->having->connectors.size() + 1 != s->having->predicates.size())
                throw SemanticError("malformed HAVING condition");
            for (const auto& predicate : s->having->predicates) {
                if (predicate.aggregate) {
                    if (predicate.type != sql::PredicateType::COMPARISON ||
                        predicate.values.size() != 1)
                        throw SemanticError("aggregate HAVING supports one comparison value" + At(predicate.position));
                    const auto& aggregate = *predicate.aggregate;
                    if (aggregate.function == sql::AggregateFunction::COUNT ||
                        aggregate.function == sql::AggregateFunction::SUM ||
                        aggregate.function == sql::AggregateFunction::AVG) {
                        if (predicate.values[0].type != Value::Type::INTEGER)
                            throw SemanticError("aggregate comparison requires an integer value" + At(predicate.position));
                    } else if (!aggregate.star &&
                               !TypesMatch(resolve(aggregate.column)->type, predicate.values[0].type)) {
                        throw SemanticError("aggregate comparison value has the wrong type" + At(predicate.position));
                    }
                    continue;
                }
                const auto* column = resolve(predicate.column);
                if (!s->group_by.empty()) require_grouped(predicate.column, "HAVING");
                else if (has_aggregate)
                    throw SemanticError("non-aggregate HAVING columns require GROUP BY" + At(predicate.position));
                if (predicate.type == sql::PredicateType::COMPARISON ||
                    predicate.type == sql::PredicateType::LIKE) {
                    if (predicate.values.size() != 1)
                        throw SemanticError("malformed HAVING predicate");
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
