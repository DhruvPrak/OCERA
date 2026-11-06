#pragma once
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "sql/ast.h"  // only for sql::ColumnType and CreateTableStatement

namespace ocera {

// The Catalog is the database's "table of tables": it remembers which tables
// exist and what columns each one has. The SQL parser does not know any of
// this (it only checks grammar), so a statement like "SELECT age FROM
// students" parses fine even if there is no such column. The Catalog, plus
// the validator in validator.h, is what catches that.
//
// ---------------------------------------------------------------------------
// STATUS: IN-MEMORY VERSION.
// Table definitions live only in this object and are LOST when the program
// exits. Saving them into the database file (page DatabaseHeader::
// GetCatalogRootPageId() points at) needs a byte encoding and the Buffer Pool,
// and is not done yet.
// ---------------------------------------------------------------------------
//
// Proposed behaviour (not yet team-approved):
//  - Table and column names are CASE-INSENSITIVE (Students == students), but
//    the original spelling is kept for display.
//  - A table needs at least one column; column names must be unique within it.

// Thrown for catalog problems (duplicate table, duplicate column, ...).
class CatalogError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ColumnSchema {
    std::string name;
    sql::ColumnType type = sql::ColumnType::INT;
};

struct TableSchema {
    std::string name;
    std::vector<ColumnSchema> columns;

    // Position of the column (0-based, case-insensitive), or -1 if absent.
    int ColumnIndex(const std::string& column_name) const;
};

// Lower-cases ASCII letters; used to compare identifiers case-insensitively.
std::string NormalizeIdentifier(const std::string& name);

class Catalog {
public:
    // Registers a new table. Throws CatalogError (and changes nothing) if the
    // table already exists, has no columns, or has duplicate column names.
    void CreateTable(const sql::CreateTableStatement& stmt);

    bool HasTable(const std::string& name) const;

    // Returns nullptr if the table does not exist. The pointer stays valid for
    // the life of the Catalog (tables are never removed).
    const TableSchema* GetTable(const std::string& name) const;

    // Table names in alphabetical order, original spelling.
    std::vector<std::string> TableNames() const;

    std::size_t NumTables() const { return tables_.size(); }

private:
    std::map<std::string, TableSchema> tables_;  // key = NormalizeIdentifier(name)
};

}  // namespace ocera
