#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>

#include "sql/ast.h"

namespace ocera {
namespace sql {

// The parser is the second step of the SQL front-end. The tokenizer turned
// characters into labelled tokens; the parser checks that the tokens form a
// valid statement (grammar) and builds an AST from them.
//
// It does NOT check that tables or columns exist, and never touches storage.

// Thrown when the tokens are well-formed but the grammar is wrong, e.g.
// "SELECT FROM t" (missing column list). This is a *grammar* error; bad
// characters are reported by TokenizerError instead.
class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& message, std::size_t position)
        : std::runtime_error(message + " (at position " +
                             std::to_string(position) + ")"),
          position_(position) {}
    std::size_t position() const { return position_; }

private:
    std::size_t position_;
};

// Parses exactly ONE statement. Throws TokenizerError for lexical problems
// and ParseError for grammar problems.
//
// PROPOSED grammar (subset, not yet team-approved):
//   CREATE TABLE name ( col type [, col type]... )        type: INT | TEXT
//   INSERT INTO name VALUES ( value [, value]... )
//   SELECT ( * | col [, col]... ) FROM name [WHERE cond]
//   UPDATE name SET col = value [, col = value]... [WHERE cond]
//   DELETE FROM name [WHERE cond]
//   cond  := col op value [(AND | OR) col op value]...    (flat, no parentheses)
//   value := integer | 'string'
// One optional ';' may end the statement; anything after it is an error.
Statement Parse(const std::string& sql);

}  // namespace sql
}  // namespace ocera
