#pragma once
#include <stdexcept>
#include <string>

#include "catalog/catalog.h"
#include "sql/ast.h"

namespace ocera {

// Thrown when a statement is grammatically fine but does not make sense for
// the current database (unknown table or column, wrong number of values, a
// string where a number is expected, ...). Kept separate from the parser's
// ParseError so callers can tell "bad SQL" from "bad against this schema".
class SemanticError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Checks `stmt` against `catalog`. Does NOT change the catalog (CREATE TABLE
// is only checked here; registering it is Catalog::CreateTable's job) and does
// not touch storage. Throws SemanticError on the first problem found.
//
// Rules (proposed):
//  CREATE TABLE : table must not already exist; column names must be unique.
//  INSERT       : table exists; one value per column, in order; each value's
//                 type matches its column (integer -> INT, string -> TEXT).
//  SELECT       : table exists; every named column exists.
//  UPDATE       : table exists; every assigned column exists and the value
//                 type matches.
//  DELETE       : table exists.
//  WHERE (any)  : every compared column exists and the literal's type matches
//                 the column's type.
void Validate(const sql::Statement& stmt, const Catalog& catalog);

}  // namespace ocera
