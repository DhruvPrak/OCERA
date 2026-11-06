#include "sql/tokenizer.h"

#include <cctype>
#include <set>

namespace ocera {
namespace sql {

namespace {

const std::set<std::string>& Keywords() {
    static const std::set<std::string> kKeywords = {
        "CREATE", "TABLE", "INSERT", "INTO", "VALUES", "SELECT", "FROM",
        "WHERE",  "UPDATE", "SET",   "DELETE", "INT",  "TEXT",   "AND", "OR"};
    return kKeywords;
}

bool IsIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool IsIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
bool IsDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

std::string ToUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
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

        // Whitespace is skipped.
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        const std::size_t start = i;

        // Keyword or identifier.
        if (IsIdentStart(c)) {
            while (i < n && IsIdentChar(sql[i])) ++i;
            const std::string word = sql.substr(start, i - start);
            const std::string upper = ToUpper(word);
            if (Keywords().count(upper)) {
                tokens.push_back({TokenType::KEYWORD, upper, start});
            } else {
                tokens.push_back({TokenType::IDENTIFIER, word, start});
            }
            continue;
        }

        // Integer (optionally negative).
        if (IsDigit(c) || (c == '-' && i + 1 < n && IsDigit(sql[i + 1]))) {
            ++i;  // consume first digit or '-'
            while (i < n && IsDigit(sql[i])) ++i;
            // "12abc" is almost certainly a typo; reject it clearly.
            if (i < n && IsIdentStart(sql[i])) {
                throw TokenizerError("invalid number: digits followed by letters", start);
            }
            tokens.push_back({TokenType::INTEGER, sql.substr(start, i - start), start});
            continue;
        }

        // String literal: 'text', with '' meaning a literal single quote.
        if (c == '\'') {
            ++i;  // skip opening quote
            std::string value;
            bool closed = false;
            while (i < n) {
                if (sql[i] == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') {
                        value.push_back('\'');
                        i += 2;
                    } else {
                        ++i;  // closing quote
                        closed = true;
                        break;
                    }
                } else {
                    value.push_back(sql[i]);
                    ++i;
                }
            }
            if (!closed) throw TokenizerError("unterminated string literal", start);
            tokens.push_back({TokenType::STRING, value, start});
            continue;
        }

        // Operators: = != < > <= >=
        if (c == '=') {
            tokens.push_back({TokenType::OPERATOR, "=", start});
            ++i;
            continue;
        }
        if (c == '!') {
            if (i + 1 < n && sql[i + 1] == '=') {
                tokens.push_back({TokenType::OPERATOR, "!=", start});
                i += 2;
                continue;
            }
            throw TokenizerError("unexpected '!' (did you mean '!='?)", start);
        }
        if (c == '<' || c == '>') {
            if (i + 1 < n && sql[i + 1] == '=') {
                tokens.push_back({TokenType::OPERATOR, std::string(1, c) + "=", start});
                i += 2;
            } else {
                tokens.push_back({TokenType::OPERATOR, std::string(1, c), start});
                ++i;
            }
            continue;
        }

        // Single-character symbols.
        if (c == '(' || c == ')' || c == ',' || c == ';' || c == '*') {
            tokens.push_back({TokenType::SYMBOL, std::string(1, c), start});
            ++i;
            continue;
        }

        throw TokenizerError(std::string("unexpected character '") + c + "'", start);
    }

    tokens.push_back({TokenType::END_OF_INPUT, "", n});
    return tokens;
}

}  // namespace sql
}  // namespace ocera
