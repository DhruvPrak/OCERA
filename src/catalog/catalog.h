#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/page.h"
#include "sql/ast.h"  // SQL schema and statement definitions

namespace ocera {

// The Catalog stores in-memory table and index metadata. It is not persistent:
// definitions are lost when the Catalog is destroyed.
class CatalogError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ColumnSchema {
    std::string name;
    sql::ColumnType type = sql::ColumnType::INT;
};

struct KeySchema {
    sql::KeyConstraintType type = sql::KeyConstraintType::UNIQUE;
    std::vector<std::string> columns;
};

struct ForeignKeySchema {
    std::vector<std::string> columns;
    std::string referenced_table;
    std::vector<std::string> referenced_columns;
};

struct TableSchema {
    std::string name;
    std::vector<ColumnSchema> columns;
    std::vector<KeySchema> key_constraints;
    std::vector<ForeignKeySchema> foreign_key_constraints;
    // First RecordPage of this table on disk.
    // INVALID_PAGE_ID means no physical storage is registered.
    page_id_t first_page_id = INVALID_PAGE_ID;

    // Position of the column (0-based, case-insensitive), or -1 if absent.
    int ColumnIndex(const std::string& column_name) const;
};

struct IndexSchema {
    std::string name;
    std::string table_name;
    std::string column_name;
};

// Lower-cases ASCII letters; used to compare identifiers case-insensitively.
std::string NormalizeIdentifier(const std::string& name);

class Catalog {
public:
    // Registers a table. Throws CatalogError without changing the Catalog if
    // the table already exists, has no columns, or has duplicate columns.
    void CreateTable(
        const sql::CreateTableStatement& stmt,
        page_id_t first_page_id = INVALID_PAGE_ID
    );
    void DropTable(const std::string& name);

    bool HasTable(const std::string& name) const;
    // Returned pointers remain valid for the life of the Catalog.
    const TableSchema* GetTable(const std::string& name) const;
    std::vector<std::string> TableNames() const;
    std::size_t NumTables() const { return tables_.size(); }

    // Registers metadata for a single-column index. The column must exist and
    // have type INT. Index names are globally unique; only one index may cover
    // a given table/column pair. This does not build or own a B+Tree.
    void CreateIndex(const sql::CreateIndexStatement& stmt);
    bool HasIndex(const std::string& name) const;
    // Returns nullptr when no matching index exists. Pointers remain stable
    // because index metadata is never erased.
    const IndexSchema* GetIndex(const std::string& name) const;
    const IndexSchema* GetIndexForColumn(const std::string& table,
                                         const std::string& column) const;
    std::size_t NumIndexes() const { return indexes_.size(); }

    // Binary catalog image used by DatabaseInstance for clean-restart persistence.
    std::vector<std::uint8_t> Serialize() const;
    void Deserialize(const std::vector<std::uint8_t>& bytes);

private:
    std::map<std::string, TableSchema> tables_;   // normalized table name
    std::map<std::string, IndexSchema> indexes_; // normalized index name
};

}  // namespace ocera
