#include "catalog/catalog.h"

#include <cctype>
#include <set>
#include <utility>
#include <limits>

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

void Catalog::DropTable(const std::string& name) {
    const std::string key = NormalizeIdentifier(name);

    auto it = tables_.find(key);
    if (it == tables_.end()) {
        throw CatalogError("table '" + name + "' does not exist");
    }

    for (auto index = indexes_.begin(); index != indexes_.end();) {
        if (NormalizeIdentifier(index->second.table_name) == key) {
            index = indexes_.erase(index);
        } else {
            ++index;
        }
    }

    tables_.erase(it);
}

void Catalog::CreateTable(
    const sql::CreateTableStatement& stmt,
    page_id_t first_page_id
) {
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
    schema.first_page_id = first_page_id;
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


namespace {
class CatalogWriter {
public:
    std::vector<std::uint8_t> data;
    void U32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) data.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xff));
    }
    void String(const std::string& value) {
        if (value.size() > UINT32_MAX) throw CatalogError("catalog string is too large");
        U32(static_cast<std::uint32_t>(value.size()));
        data.insert(data.end(), value.begin(), value.end());
    }
};
class CatalogReader {
public:
    explicit CatalogReader(const std::vector<std::uint8_t>& input) : data(input) {}
    std::uint32_t U32() {
        Require(4);
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[pos++]) << (i * 8);
        return v;
    }
    std::string String() {
        const std::uint32_t n = U32();
        Require(n);
        std::string value(data.begin() + static_cast<std::ptrdiff_t>(pos),
                          data.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        return value;
    }
    bool Done() const { return pos == data.size(); }
private:
    const std::vector<std::uint8_t>& data;
    std::size_t pos = 0;
    void Require(std::size_t n) {
        if (n > data.size() - pos) throw CatalogError("truncated catalog image");
    }
};
constexpr std::uint32_t kCatalogImageMagic = 0x31415443u; // "CTA1"
constexpr std::uint32_t kCatalogImageVersion = 2;
constexpr std::uint32_t kMaxCatalogItems = 100000;
}

std::vector<std::uint8_t> Catalog::Serialize() const {
    CatalogWriter w;
    w.U32(kCatalogImageMagic);
    w.U32(kCatalogImageVersion);
    w.U32(static_cast<std::uint32_t>(tables_.size()));
    for (const auto& entry : tables_) {
        const TableSchema& t = entry.second;
        w.String(t.name);
        w.U32(static_cast<std::uint32_t>(t.first_page_id));
        w.U32(static_cast<std::uint32_t>(t.columns.size()));
        for (const auto& c : t.columns) {
            w.String(c.name);
            w.U32(static_cast<std::uint32_t>(c.type));
        }
        w.U32(static_cast<std::uint32_t>(t.key_constraints.size()));
        for (const auto& k : t.key_constraints) {
            w.U32(static_cast<std::uint32_t>(k.type));
            w.U32(static_cast<std::uint32_t>(k.columns.size()));
            for (const auto& c : k.columns) w.String(c);
        }
        w.U32(static_cast<std::uint32_t>(t.foreign_key_constraints.size()));
        for (const auto& fk : t.foreign_key_constraints) {
            w.U32(static_cast<std::uint32_t>(fk.columns.size()));
            for (const auto& c : fk.columns) w.String(c);
            w.String(fk.referenced_table);
            w.U32(static_cast<std::uint32_t>(fk.referenced_columns.size()));
            for (const auto& c : fk.referenced_columns) w.String(c);
        }
    }
    w.U32(static_cast<std::uint32_t>(indexes_.size()));
    for (const auto& entry : indexes_) {
        const auto& i = entry.second;
        w.String(i.name); w.String(i.table_name); w.String(i.column_name);
    }
    return std::move(w.data);
}

void Catalog::Deserialize(const std::vector<std::uint8_t>& bytes) {
    CatalogReader r(bytes);
    if (r.U32() != kCatalogImageMagic) throw CatalogError("invalid catalog image magic");
    const std::uint32_t version = r.U32();

    if (version != 1 && version != 2) {
        throw CatalogError("unsupported catalog image version");
    }
    std::map<std::string, TableSchema> new_tables;
    std::map<std::string, IndexSchema> new_indexes;
    const std::uint32_t table_count = r.U32();
    if (table_count > kMaxCatalogItems) throw CatalogError("catalog has too many tables");
    for (std::uint32_t ti = 0; ti < table_count; ++ti) {
        TableSchema t;
        t.name = r.String();

        if (version >= 2) {
            const std::uint32_t raw_page_id = r.U32();

            if (raw_page_id ==
            static_cast<std::uint32_t>(INVALID_PAGE_ID)) {
                t.first_page_id = INVALID_PAGE_ID;
            } else {
                if (raw_page_id < 2 ||
                raw_page_id >
                static_cast<std::uint32_t>(
                std::numeric_limits<page_id_t>::max())) {
                    throw CatalogError("invalid table first-page ID");
                }

             t.first_page_id = static_cast<page_id_t>(raw_page_id);
             }
}

const auto columns = r.U32();
        if (columns == 0 || columns > kMaxCatalogItems) throw CatalogError("invalid catalog column count");
        for (std::uint32_t ci = 0; ci < columns; ++ci) {
            ColumnSchema c; c.name = r.String();
            const auto type = r.U32();
            if (type > static_cast<std::uint32_t>(sql::ColumnType::TEXT)) throw CatalogError("invalid catalog column type");
            c.type = static_cast<sql::ColumnType>(type); t.columns.push_back(std::move(c));
        }
        const auto keys = r.U32();
        if (keys > kMaxCatalogItems) throw CatalogError("too many catalog key constraints");
        for (std::uint32_t ki = 0; ki < keys; ++ki) {
            KeySchema k; const auto type = r.U32();
            if (type > static_cast<std::uint32_t>(sql::KeyConstraintType::UNIQUE)) throw CatalogError("invalid catalog key type");
            k.type = static_cast<sql::KeyConstraintType>(type);
            const auto count = r.U32(); if (count == 0 || count > columns) throw CatalogError("invalid key column count");
            for (std::uint32_t j = 0; j < count; ++j) k.columns.push_back(r.String());
            t.key_constraints.push_back(std::move(k));
        }
        const auto fks = r.U32();
        if (fks > kMaxCatalogItems) throw CatalogError("too many foreign keys");
        for (std::uint32_t fi = 0; fi < fks; ++fi) {
            ForeignKeySchema fk; const auto count = r.U32();
            if (count == 0 || count > columns) throw CatalogError("invalid foreign-key column count");
            for (std::uint32_t j = 0; j < count; ++j) fk.columns.push_back(r.String());
            fk.referenced_table = r.String(); const auto refs = r.U32();
            if (refs != count) throw CatalogError("invalid referenced foreign-key column count");
            for (std::uint32_t j = 0; j < refs; ++j) fk.referenced_columns.push_back(r.String());
            t.foreign_key_constraints.push_back(std::move(fk));
        }
        const std::string key = NormalizeIdentifier(t.name);
        if (key.empty() || !new_tables.emplace(key, std::move(t)).second) throw CatalogError("duplicate or empty table in catalog image");
    }
    const auto index_count = r.U32();
    if (index_count > kMaxCatalogItems) throw CatalogError("too many indexes");
    for (std::uint32_t ii = 0; ii < index_count; ++ii) {
        IndexSchema i; i.name = r.String(); i.table_name = r.String(); i.column_name = r.String();
        if (!new_indexes.emplace(NormalizeIdentifier(i.name), std::move(i)).second) throw CatalogError("duplicate index in catalog image");
    }
    if (!r.Done()) throw CatalogError("trailing bytes in catalog image");
    // Validate references after every table has been loaded (supports forward/self references).
    for (const auto& entry : new_tables) {
        const TableSchema& t = entry.second;
        for (const auto& fk : t.foreign_key_constraints) {
            auto parent = new_tables.find(NormalizeIdentifier(fk.referenced_table));
            if (parent == new_tables.end()) throw CatalogError("catalog foreign key references missing table");
            if (fk.columns.size() != fk.referenced_columns.size()) throw CatalogError("invalid catalog foreign key");
            for (std::size_t j = 0; j < fk.columns.size(); ++j) {
                if (t.ColumnIndex(fk.columns[j]) < 0 || parent->second.ColumnIndex(fk.referenced_columns[j]) < 0)
                    throw CatalogError("catalog foreign key references missing column");
            }
        }
    }
    for (const auto& entry : new_indexes) {
        auto table = new_tables.find(NormalizeIdentifier(entry.second.table_name));
        if (table == new_tables.end() || table->second.ColumnIndex(entry.second.column_name) < 0)
            throw CatalogError("catalog index references missing table or column");
    }
    tables_.swap(new_tables);
    indexes_.swap(new_indexes);
}

}  // namespace ocera
