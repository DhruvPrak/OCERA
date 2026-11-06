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

const char* ColumnTypeName(ColumnType t) { return t == ColumnType::INT ? "INT" : "TEXT"; }
const char* ValueTypeName(Value::Type t) {
    return t == Value::Type::INTEGER ? "an integer" : "a string";
}

std::string At(std::size_t position) {
    return " (at position " + std::to_string(position) + ")";
}

const TableSchema& RequireTable(const Catalog& catalog, const std::string& name) {
    const TableSchema* t = catalog.GetTable(name);
    if (!t) throw SemanticError("table '" + name + "' does not exist");
    return *t;
}

// Returns a pointer (never null) rather than a reference, because callers pass
// a temporary string for where_suffix and returning a reference there makes
// some compilers emit a (harmless) dangling-reference warning.
const ColumnSchema* RequireColumn(const TableSchema& table, const std::string& column,
                                  const std::string& where_suffix) {
    int idx = table.ColumnIndex(column);
    if (idx < 0) {
        throw SemanticError("table '" + table.name + "' has no column '" + column + "'" +
                            where_suffix);
    }
    return &table.columns[static_cast<std::size_t>(idx)];
}

void CheckValueType(const ColumnSchema& col, const Value& value, const std::string& suffix) {
    if (!TypesMatch(col.type, value.type)) {
        throw SemanticError(std::string("column '") + col.name + "' is " +
                            ColumnTypeName(col.type) + " but got " +
                            ValueTypeName(value.type) + suffix);
    }
}

void CheckCondition(const sql::Condition& cond, const TableSchema& table) {
    if (cond.comparisons.empty() ||
        cond.connectors.size() + 1 != cond.comparisons.size()) {
        throw SemanticError("malformed WHERE condition");
    }
    for (const auto& cmp : cond.comparisons) {
        const ColumnSchema* col = RequireColumn(table, cmp.column, At(cmp.position));
        CheckValueType(*col, cmp.value, At(cmp.value.position));
    }
}

void CheckOptionalCondition(const std::optional<sql::Condition>& cond,
                            const TableSchema& table) {
    if (cond) CheckCondition(*cond, table);
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
        const TableSchema& table = RequireTable(catalog, s->table);
        if (!s->select_all) {
            for (const auto& name : s->columns) RequireColumn(table, name, "");
        }
        CheckOptionalCondition(s->where, table);
    } else if (const auto* s = std::get_if<sql::UpdateStatement>(&stmt)) {
        const TableSchema& table = RequireTable(catalog, s->table);
        for (const auto& a : s->assignments) {
            const ColumnSchema* col = RequireColumn(table, a.column, At(a.position));
            CheckValueType(*col, a.value, At(a.value.position));
        }
        CheckOptionalCondition(s->where, table);
    } else if (const auto* s = std::get_if<sql::DeleteStatement>(&stmt)) {
        const TableSchema& table = RequireTable(catalog, s->table);
        CheckOptionalCondition(s->where, table);
    }
}

}  // namespace ocera
