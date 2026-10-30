#include "catalog/catalog.h"

#include <cctype>
#include <set>

namespace ocera {

std::string NormalizeIdentifier(const std::string& name) {
    std::string out = name;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

int TableSchema::ColumnIndex(const std::string& column_name) const {
    const std::string key = NormalizeIdentifier(column_name);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (NormalizeIdentifier(columns[i].name) == key) return static_cast<int>(i);
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

    // Build the whole schema first; only insert it if everything is valid, so
    // a failed CreateTable never leaves a half-registered table behind.
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
    tables_.emplace(key, std::move(schema));
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

}  // namespace ocera
