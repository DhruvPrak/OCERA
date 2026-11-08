#pragma once
#include <stdexcept>
#include <string>

#include "catalog/catalog.h"
#include "sql/ast.h"

namespace ocera {

// A grammatically valid statement that is incompatible with the current schema.
class SemanticError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Checks a statement against the Catalog without modifying the Catalog or
// touching storage. CREATE INDEX validation checks metadata constraints only;
// Catalog::CreateIndex performs the actual metadata registration.
void Validate(const sql::Statement& stmt, const Catalog& catalog);

}  // namespace ocera
