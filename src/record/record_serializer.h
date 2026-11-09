#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "sql/ast.h"

namespace ocera {

// Converts schema-ordered SQL values to and from the opaque byte payload
// stored by TableHeap / RecordPage.
//
// Format:
//   INT  = 8-byte signed integer, little-endian
//   TEXT = 4-byte unsigned byte length, followed by that many bytes
//
// The schema supplies the field count and types. No schema metadata is
// duplicated in the record payload.
class RecordSerializer {
public:
    static bool Encode(
        const TableSchema& schema,
        const std::vector<sql::Value>& values,
        std::vector<std::uint8_t>* output,
        std::string* error = nullptr
    );

    static bool Decode(
        const TableSchema& schema,
        const std::vector<std::uint8_t>& input,
        std::vector<sql::Value>* output,
        std::string* error = nullptr
    );
};

}  // namespace ocera