#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace ocera {
namespace sql {

// The tokenizer is the first step of the SQL front-end: it turns a SQL string
// into a flat list of labelled pieces ("tokens"). It does NOT check grammar;
// that is the parser's job. For example, "SELECT" followed by "FROM" is
// tokenized fine even though it is not a valid statement.

enum class TokenType {
    KEYWORD,      // SELECT, FROM, ... (text is stored UPPERCASE)
    IDENTIFIER,   // table / column names (original case kept)
    INTEGER,      // 42, -7
    STRING,       // 'hello' (text is the content WITHOUT the quotes)
    OPERATOR,     // =  !=  <  >  <=  >=
    SYMBOL,       // (  )  ,  ;  *
    END_OF_INPUT  // always the last token
};

struct Token {
    TokenType type;
    std::string text;
    std::size_t position;  // 0-based character offset in the SQL string
};

// Thrown for bad input (unterminated string, unknown character, ...).
// This is a *lexical* error; the parser will report grammar errors separately.
class TokenizerError : public std::runtime_error {
public:
    TokenizerError(const std::string& message, std::size_t position)
        : std::runtime_error(message + " (at position " +
                             std::to_string(position) + ")"),
          position_(position) {}
    std::size_t position() const { return position_; }

private:
    std::size_t position_;
};

// Splits `sql` into tokens. The returned vector always ends with an
// END_OF_INPUT token. Throws TokenizerError on invalid input.
//
// Rules:
//  - Keywords are case-insensitive: select == SELECT.
//  - String literals use single quotes; write '' inside to mean one quote.
//  - A '-' immediately followed by a digit is a negative integer.
std::vector<Token> Tokenize(const std::string& sql);

// Human-readable name of a token type, handy for tests and error messages.
const char* TokenTypeName(TokenType type);

}  // namespace sql
}  // namespace ocera
