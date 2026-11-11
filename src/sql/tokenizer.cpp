#include "sql/tokenizer.h"

#include <cctype>
#include <set>
#include <string>

namespace ocera {
namespace sql {
namespace {

const std::set<std::string>& Keywords() {
    // Keep this list lexical only: recognizing a keyword here does not imply
    // that the parser or executor supports the corresponding SQL feature yet.
    static const std::set<std::string> kKeywords = {
        // Existing DDL and DML
        "CREATE", "TABLE", "INDEX", "ON", "INSERT", "INTO", "VALUES",
        "SELECT", "FROM", "WHERE", "UPDATE", "SET", "DELETE",
        "SHOW", "TABLES", "DESC", "DROP",

        // Data types and constraints
        "INT", "INTEGER", "TEXT", "PRIMARY", "KEY", "UNIQUE",
        "FOREIGN", "REFERENCES", "NOT", "NULL",

        // Boolean and predicate operators
        "AND", "OR", "IS", "LIKE", "IN", "BETWEEN",

        // Joins
        "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "CROSS",
        "NATURAL", "USING",

        // Query clauses and ordering
        "AS", "ORDER", "BY", "ASC", "DESC", "LIMIT", "OFFSET",
        "DISTINCT", "GROUP", "HAVING",

        // Common aggregate and scalar functions
        "COUNT", "SUM", "AVG", "MIN", "MAX",

        // Date/time and conditional-expression vocabulary
        "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP",
        "CASE", "WHEN", "THEN", "ELSE", "END"
    };
    return kKeywords;
}

bool IsIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

std::string ToUpper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(
            std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// A minus sign belongs to an integer literal only where a value can begin.
// Otherwise it is the subtraction operator: `a-1` => IDENTIFIER, -, INTEGER.
bool CanStartSignedInteger(const std::vector<Token>& tokens) {
    if (tokens.empty()) {
        return true;
    }

    const Token& previous = tokens.back();
    if (previous.type == TokenType::OPERATOR) {
        return true;
    }

    if (previous.type == TokenType::SYMBOL) {
        return previous.text == "(" || previous.text == ",";
    }

    if (previous.type == TokenType::KEYWORD) {
        static const std::set<std::string> value_prefix_keywords = {
            "SELECT", "VALUES", "SET", "WHERE", "AND", "OR", "NOT",
            "IN", "BETWEEN", "THEN", "ELSE", "WHEN", "BY", "LIMIT",
            "OFFSET", "HAVING", "ON"
        };
        return value_prefix_keywords.count(previous.text) != 0;
    }

    return false;
}

}  // namespace

const char* TokenTypeName(TokenType type) {
    switch (type) {
        case TokenType::KEYWORD: return "KEYWORD";
        case TokenType::IDENTIFIER: return "IDENTIFIER";
        case TokenType::INTEGER: return "INTEGER";
        case TokenType::STRING: return "STRING";
        case TokenType::OPERATOR: return "OPERATOR";
        case TokenType::SYMBOL: return "SYMBOL";
        case TokenType::END_OF_INPUT: return "END_OF_INPUT";
    }
    return "UNKNOWN";
}

std::vector<Token> Tokenize(const std::string& sql) {
    std::vector<Token> tokens;
    const std::size_t n = sql.size();
    std::size_t i = 0;

    while (i < n) {
        const char c = sql[i];

        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
        }

        // SQL single-line comment. The newline is consumed by the whitespace
        // handling on the next iteration.
        if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
            i += 2;
            while (i < n && sql[i] != '\n' && sql[i] != '\r') {
                ++i;
            }
            continue;
        }

        const std::size_t start = i;

        if (IsIdentStart(c)) {
            while (i < n && IsIdentChar(sql[i])) {
                ++i;
            }

            const std::string word = sql.substr(start, i - start);
            const std::string upper = ToUpper(word);
            if (Keywords().count(upper) != 0) {
                tokens.push_back({TokenType::KEYWORD, upper, start});
            } else {
                tokens.push_back({TokenType::IDENTIFIER, word, start});
            }
            continue;
        }

        const bool signed_integer =
            c == '-' && i + 1 < n && IsDigit(sql[i + 1]) &&
            CanStartSignedInteger(tokens);

        if (IsDigit(c) || signed_integer) {
            if (c == '-') {
                ++i;
            }
            while (i < n && IsDigit(sql[i])) {
                ++i;
            }

            if (i < n && IsIdentStart(sql[i])) {
                throw TokenizerError(
                    "invalid number: digits followed by letters", start);
            }

            tokens.push_back(
                {TokenType::INTEGER, sql.substr(start, i - start), start});
            continue;
        }

        if (c == '\'') {
            ++i;
            std::string value;
            bool closed = false;

            while (i < n) {
                if (sql[i] == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') {
                        value.push_back('\'');
                        i += 2;
                    } else {
                        ++i;
                        closed = true;
                        break;
                    }
                } else {
                    value.push_back(sql[i]);
                    ++i;
                }
            }

            if (!closed) {
                throw TokenizerError("unterminated string literal", start);
            }

            tokens.push_back({TokenType::STRING, value, start});
            continue;
        }

        // Two-character comparison operators must be checked before their
        // one-character prefixes.
        if (i + 1 < n) {
            const std::string two = sql.substr(i, 2);
            if (two == "!=" || two == "<=" || two == ">=" || two == "<>") {
                tokens.push_back({TokenType::OPERATOR, two, start});
                i += 2;
                continue;
            }
        }

        if (c == '=' || c == '<' || c == '>' ||
            c == '+' || c == '-' || c == '/' || c == '%') {
            tokens.push_back(
                {TokenType::OPERATOR, std::string(1, c), start});
            ++i;
            continue;
        }

        // A dot is supported only between identifier components, e.g.
        // table.column or schema.table.column. This deliberately rejects
        // decimal literals and malformed qualification.
        if (c == '.') {
            const bool follows_identifier =
                !tokens.empty() && tokens.back().type == TokenType::IDENTIFIER;
            const bool precedes_identifier =
                i + 1 < n && IsIdentStart(sql[i + 1]);
            if (!follows_identifier || !precedes_identifier) {
                throw TokenizerError(
                    "dot must separate identifier components", start);
            }
            tokens.push_back({TokenType::SYMBOL, ".", start});
            ++i;
            continue;
        }

        // Distinguish multiplication from the SELECT * wildcard using the
        // preceding token. A completed value followed by '*' means
        // multiplication; in a value-prefix position it remains a wildcard.
        if (c == '*') {
            const bool has_left_operand =
                !tokens.empty() &&
                (tokens.back().type == TokenType::IDENTIFIER ||
                 tokens.back().type == TokenType::INTEGER ||
                 tokens.back().type == TokenType::STRING ||
                 (tokens.back().type == TokenType::SYMBOL &&
                  tokens.back().text == ")"));
            tokens.push_back({has_left_operand ? TokenType::OPERATOR
                                               : TokenType::SYMBOL,
                              "*", start});
            ++i;
            continue;
        }

        if (c == '(' || c == ')' || c == ',' || c == ';') {
            tokens.push_back(
                {TokenType::SYMBOL, std::string(1, c), start});
            ++i;
            continue;
        }

        throw TokenizerError(
            std::string("unexpected character '") + c + "'", start);
    }

    tokens.push_back({TokenType::END_OF_INPUT, "", n});
    return tokens;
}

}  // namespace sql
}  // namespace ocera
