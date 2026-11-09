#pragma once

#include <string>
#include <vector>
#include <utility>

#include "catalog/catalog.h"
#include "common/rid.h"
#include "record/table_heap.h"
#include "sql/ast.h"

namespace ocera {

// Typed row interface over an existing TableHeap.
// The schema is copied so this object does not depend on the lifetime of a
// Catalog-owned TableSchema pointer.
class RecordTable {
public:
bool Scan(
    std::vector<std::pair<RID, std::vector<sql::Value>>>* rows,
    std::string* error = nullptr
);
    RecordTable(const TableSchema& schema, TableHeap& heap);

    // Values must match the schema's column order and types.
    bool Insert(
        const std::vector<sql::Value>& values,
        RID* rid,
        std::string* error = nullptr
    );

    // Decodes a stored row into schema-ordered SQL values.
    bool Get(
        const RID& rid,
        std::vector<sql::Value>* values,
        std::string* error = nullptr
    );

    // Replaces a row only when encoding and the underlying heap update succeed.
    bool Update(
        const RID& rid,
        const std::vector<sql::Value>& values,
        std::string* error = nullptr
    );

    bool Delete(const RID& rid);

    const TableSchema& GetSchema() const { return schema_; }

private:
    TableSchema schema_;
    TableHeap& heap_;
};

}  // namespace ocera
