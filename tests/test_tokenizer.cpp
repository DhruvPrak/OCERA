#include <iostream>
#include <string>
#include <vector>

#include "sql/tokenizer.h"

using namespace ocera::sql;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cout << "    FAILED: " << message << '\n';
        ++g_failures;
    }
}

struct ExpectedToken {
    TokenType type;
    std::string text;
};

void CheckTokens(const std::string& sql,
                 const std::vector<ExpectedToken>& expected,
                 const std::string& test_name) {
    try {
        const std::vector<Token> actual = Tokenize(sql);
        Check(actual.size() == expected.size() + 1,
              test_name + ": token count (expected " +
                  std::to_string(expected.size()) + ", got " +
                  std::to_string(actual.empty() ? 0 : actual.size() - 1) + ")");

        const std::size_t comparable =
            actual.size() < expected.size() ? actual.size() : expected.size();
        for (std::size_t i = 0; i < comparable; ++i) {
            Check(actual[i].type == expected[i].type &&
                      actual[i].text == expected[i].text,
                  test_name + ": token " + std::to_string(i) +
                      " expected " + TokenTypeName(expected[i].type) +
                      " '" + expected[i].text + "', got " +
                      TokenTypeName(actual[i].type) + " '" + actual[i].text + "'");
        }

        if (!actual.empty()) {
            Check(actual.back().type == TokenType::END_OF_INPUT,
                  test_name + ": final token is END_OF_INPUT");
            Check(actual.back().position == sql.size(),
                  test_name + ": END_OF_INPUT position equals SQL length");
        }
    } catch (const std::exception& error) {
        Check(false, test_name + ": unexpected exception: " + error.what());
    }
}

bool ThrowsTokenizerError(const std::string& sql,
                          std::size_t* error_position = nullptr) {
    try {
        (void)Tokenize(sql);
    } catch (const TokenizerError& error) {
        if (error_position != nullptr) {
            *error_position = error.position();
        }
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

void Report(const char* name, int failures_before) {
    std::cout << (g_failures == failures_before ? "    PASSED: " : "    FAILED: ")
              << name << '\n';
}

}  // namespace

int main() {
    using T = TokenType;
    int before = g_failures;

    std::cout << "=== OCERA SQL Tokenizer Tests ===\n\n";

    std::cout << "[1] Existing SQL statements remain tokenized correctly\n";
    before = g_failures;
    CheckTokens(
        "SELECT name FROM students WHERE id = 5;",
        {{T::KEYWORD, "SELECT"}, {T::IDENTIFIER, "name"},
         {T::KEYWORD, "FROM"}, {T::IDENTIFIER, "students"},
         {T::KEYWORD, "WHERE"}, {T::IDENTIFIER, "id"},
         {T::OPERATOR, "="}, {T::INTEGER, "5"}, {T::SYMBOL, ";"}},
        "basic SELECT");
    CheckTokens(
        "CREATE TABLE t (id INT, name TEXT);",
        {{T::KEYWORD, "CREATE"}, {T::KEYWORD, "TABLE"},
         {T::IDENTIFIER, "t"}, {T::SYMBOL, "("},
         {T::IDENTIFIER, "id"}, {T::KEYWORD, "INT"},
         {T::SYMBOL, ","}, {T::IDENTIFIER, "name"},
         {T::KEYWORD, "TEXT"}, {T::SYMBOL, ")"}, {T::SYMBOL, ";"}},
        "CREATE TABLE");
    CheckTokens(
        "INSERT INTO t VALUES (1, 'Asha');",
        {{T::KEYWORD, "INSERT"}, {T::KEYWORD, "INTO"},
         {T::IDENTIFIER, "t"}, {T::KEYWORD, "VALUES"},
         {T::SYMBOL, "("}, {T::INTEGER, "1"}, {T::SYMBOL, ","},
         {T::STRING, "Asha"}, {T::SYMBOL, ")"}, {T::SYMBOL, ";"}},
        "INSERT");
    CheckTokens(
        "CREATE INDEX idx_age ON students (age);",
        {{T::KEYWORD, "CREATE"}, {T::KEYWORD, "INDEX"},
         {T::IDENTIFIER, "idx_age"}, {T::KEYWORD, "ON"},
         {T::IDENTIFIER, "students"}, {T::SYMBOL, "("},
         {T::IDENTIFIER, "age"}, {T::SYMBOL, ")"}, {T::SYMBOL, ";"}},
        "CREATE INDEX");
    CheckTokens(
        "SELECT students.name, school.students.id FROM school.students",
        {{T::KEYWORD, "SELECT"}, {T::IDENTIFIER, "students"},
         {T::SYMBOL, "."}, {T::IDENTIFIER, "name"}, {T::SYMBOL, ","},
         {T::IDENTIFIER, "school"}, {T::SYMBOL, "."},
         {T::IDENTIFIER, "students"}, {T::SYMBOL, "."},
         {T::IDENTIFIER, "id"}, {T::KEYWORD, "FROM"},
         {T::IDENTIFIER, "school"}, {T::SYMBOL, "."},
         {T::IDENTIFIER, "students"}},
        "qualified identifiers");
    Report("existing SQL statements", before);

    std::cout << "[2] Phase 3 arithmetic and comparison operators\n";
    before = g_failures;
    CheckTokens(
        "a = 1 a != 2 a <> 3 a < 4 a > 5 a <= 6 a >= 7",
        {{T::IDENTIFIER, "a"}, {T::OPERATOR, "="}, {T::INTEGER, "1"},
         {T::IDENTIFIER, "a"}, {T::OPERATOR, "!="}, {T::INTEGER, "2"},
         {T::IDENTIFIER, "a"}, {T::OPERATOR, "<>"}, {T::INTEGER, "3"},
         {T::IDENTIFIER, "a"}, {T::OPERATOR, "<"}, {T::INTEGER, "4"},
         {T::IDENTIFIER, "a"}, {T::OPERATOR, ">"}, {T::INTEGER, "5"},
         {T::IDENTIFIER, "a"}, {T::OPERATOR, "<="}, {T::INTEGER, "6"},
         {T::IDENTIFIER, "a"}, {T::OPERATOR, ">="}, {T::INTEGER, "7"}},
        "comparison operators");
    CheckTokens(
        "price + tax - discount * quantity / factor % 2",
        {{T::IDENTIFIER, "price"}, {T::OPERATOR, "+"},
         {T::IDENTIFIER, "tax"}, {T::OPERATOR, "-"},
         {T::IDENTIFIER, "discount"}, {T::OPERATOR, "*"},
         {T::IDENTIFIER, "quantity"}, {T::OPERATOR, "/"},
         {T::IDENTIFIER, "factor"}, {T::OPERATOR, "%"},
         {T::INTEGER, "2"}},
        "arithmetic operators");
    CheckTokens(
        "SELECT * FROM products WHERE price = -7 AND quantity = 1-2",
        {{T::KEYWORD, "SELECT"}, {T::SYMBOL, "*"},
         {T::KEYWORD, "FROM"}, {T::IDENTIFIER, "products"},
         {T::KEYWORD, "WHERE"}, {T::IDENTIFIER, "price"},
         {T::OPERATOR, "="}, {T::INTEGER, "-7"},
         {T::KEYWORD, "AND"}, {T::IDENTIFIER, "quantity"},
         {T::OPERATOR, "="}, {T::INTEGER, "1"},
         {T::OPERATOR, "-"}, {T::INTEGER, "2"}},
        "unary minus, subtraction and SELECT star");
    Report("arithmetic and comparison operators", before);

    std::cout << "[3] Phase 3 condition, join, clause and function keywords\n";
    before = g_failures;
    CheckTokens(
        "NOT IS NULL LIKE IN BETWEEN JOIN INNER LEFT RIGHT FULL OUTER CROSS NATURAL USING",
        {{T::KEYWORD, "NOT"}, {T::KEYWORD, "IS"}, {T::KEYWORD, "NULL"},
         {T::KEYWORD, "LIKE"}, {T::KEYWORD, "IN"}, {T::KEYWORD, "BETWEEN"},
         {T::KEYWORD, "JOIN"}, {T::KEYWORD, "INNER"}, {T::KEYWORD, "LEFT"},
         {T::KEYWORD, "RIGHT"}, {T::KEYWORD, "FULL"}, {T::KEYWORD, "OUTER"},
         {T::KEYWORD, "CROSS"}, {T::KEYWORD, "NATURAL"}, {T::KEYWORD, "USING"}},
        "conditions and joins");
    CheckTokens(
        "AS ORDER BY ASC DESC LIMIT OFFSET DISTINCT GROUP HAVING",
        {{T::KEYWORD, "AS"}, {T::KEYWORD, "ORDER"}, {T::KEYWORD, "BY"},
         {T::KEYWORD, "ASC"}, {T::KEYWORD, "DESC"}, {T::KEYWORD, "LIMIT"},
         {T::KEYWORD, "OFFSET"}, {T::KEYWORD, "DISTINCT"},
         {T::KEYWORD, "GROUP"}, {T::KEYWORD, "HAVING"}},
        "query clauses");
    CheckTokens(
        "COUNT SUM AVG MIN MAX CURRENT_DATE CURRENT_TIME CURRENT_TIMESTAMP",
        {{T::KEYWORD, "COUNT"}, {T::KEYWORD, "SUM"}, {T::KEYWORD, "AVG"},
         {T::KEYWORD, "MIN"}, {T::KEYWORD, "MAX"},
         {T::KEYWORD, "CURRENT_DATE"}, {T::KEYWORD, "CURRENT_TIME"},
         {T::KEYWORD, "CURRENT_TIMESTAMP"}},
        "aggregate and date-time keywords");
    CheckTokens(
        "CASE WHEN THEN ELSE END",
        {{T::KEYWORD, "CASE"}, {T::KEYWORD, "WHEN"},
         {T::KEYWORD, "THEN"}, {T::KEYWORD, "ELSE"}, {T::KEYWORD, "END"}},
        "conditional expression keywords");
    Report("Phase 3 keyword vocabulary", before);

    std::cout << "[4] Keywords are case-insensitive; identifiers preserve case\n";
    before = g_failures;
    CheckTokens(
        "sElEcT Name fRoM Students wHeRe value bEtWeEn 1 aNd 9",
        {{T::KEYWORD, "SELECT"}, {T::IDENTIFIER, "Name"},
         {T::KEYWORD, "FROM"}, {T::IDENTIFIER, "Students"},
         {T::KEYWORD, "WHERE"}, {T::IDENTIFIER, "value"},
         {T::KEYWORD, "BETWEEN"}, {T::INTEGER, "1"},
         {T::KEYWORD, "AND"}, {T::INTEGER, "9"}},
        "case handling");
    CheckTokens(
        "selection order_by student_1 _private",
        {{T::IDENTIFIER, "selection"}, {T::IDENTIFIER, "order_by"},
         {T::IDENTIFIER, "student_1"}, {T::IDENTIFIER, "_private"}},
        "keyword prefixes and identifiers");
    Report("case handling and identifiers", before);

    std::cout << "[5] Strings, escaped quotes and punctuation\n";
    before = g_failures;
    CheckTokens("'' 'hello world' 'it''s' 'SELECT FROM'",
                {{T::STRING, ""}, {T::STRING, "hello world"},
                 {T::STRING, "it's"}, {T::STRING, "SELECT FROM"}},
                "string literals");
    CheckTokens("(a,b); *",
                {{T::SYMBOL, "("}, {T::IDENTIFIER, "a"}, {T::SYMBOL, ","},
                 {T::IDENTIFIER, "b"}, {T::SYMBOL, ")"}, {T::SYMBOL, ";"},
                 {T::SYMBOL, "*"}},
                "punctuation and star");
    Report("strings and punctuation", before);

    std::cout << "[6] SQL single-line comments\n";
    before = g_failures;
    CheckTokens("SELECT -- ignore this text\nname FROM students",
                {{T::KEYWORD, "SELECT"}, {T::IDENTIFIER, "name"},
                 {T::KEYWORD, "FROM"}, {T::IDENTIFIER, "students"}},
                "comment before newline");
    CheckTokens("SELECT 1; -- trailing comment",
                {{T::KEYWORD, "SELECT"}, {T::INTEGER, "1"}, {T::SYMBOL, ";"}},
                "trailing comment");
    CheckTokens("-- comment only\r\nSELECT 2",
                {{T::KEYWORD, "SELECT"}, {T::INTEGER, "2"}},
                "CRLF comment");
    CheckTokens("SELECT '--not a comment'",
                {{T::KEYWORD, "SELECT"}, {T::STRING, "--not a comment"}},
                "comment marker inside string");
    Report("single-line comments", before);

    std::cout << "[7] Signed integers and subtraction boundaries\n";
    before = g_failures;
    // A leading minus is unary only when no left-hand operand exists.
    // After an integer, '-' is necessarily the subtraction operator.
    CheckTokens("-7", {{T::INTEGER, "-7"}},
                "negative integer at expression start");
    CheckTokens("0 12345", {{T::INTEGER, "0"}, {T::INTEGER, "12345"}},
                "positive integers separated by whitespace");
    CheckTokens("0 - 7", {{T::INTEGER, "0"}, {T::OPERATOR, "-"},
                          {T::INTEGER, "7"}},
                "whitespace-separated subtraction");
    CheckTokens("a-7 7-2 a = -12 ( -3, 4 )",
                {{T::IDENTIFIER, "a"}, {T::OPERATOR, "-"}, {T::INTEGER, "7"},
                 {T::INTEGER, "7"}, {T::OPERATOR, "-"}, {T::INTEGER, "2"},
                 {T::IDENTIFIER, "a"}, {T::OPERATOR, "="}, {T::INTEGER, "-12"},
                 {T::SYMBOL, "("}, {T::INTEGER, "-3"}, {T::SYMBOL, ","},
                 {T::INTEGER, "4"}, {T::SYMBOL, ")"}},
                "negative literals versus subtraction");
    Report("signed integers", before);

    std::cout << "[8] Whitespace, empty input and token positions\n";
    before = g_failures;
    CheckTokens("  SELECT\n\t*\r\nFROM   t  ",
                {{T::KEYWORD, "SELECT"}, {T::SYMBOL, "*"},
                 {T::KEYWORD, "FROM"}, {T::IDENTIFIER, "t"}},
                "mixed whitespace");
    CheckTokens("", {}, "empty input");
    CheckTokens("   \n\t", {}, "whitespace-only input");
    {
        const std::vector<Token> tokens = Tokenize("ab  = 'x'");
        Check(tokens.size() == 4, "positions: three tokens plus EOF");
        if (tokens.size() == 4) {
            Check(tokens[0].position == 0, "positions: identifier starts at 0");
            Check(tokens[1].position == 4, "positions: operator starts at 4");
            Check(tokens[2].position == 6, "positions: string starts at 6");
            Check(tokens[3].position == 9, "positions: EOF is at input length");
        }
    }
    Report("whitespace, empty input and positions", before);

    std::cout << "[9] Invalid input reports lexical errors at the right position\n";
    before = g_failures;
    std::size_t position = 0;
    Check(ThrowsTokenizerError("SELECT 'abc", &position) && position == 7,
          "unterminated string at opening quote");
    Check(ThrowsTokenizerError("SELECT @ FROM t", &position) && position == 7,
          "unknown character position");
    Check(ThrowsTokenizerError("a ! b", &position) && position == 2,
          "lone exclamation mark rejected");
    Check(ThrowsTokenizerError("12abc", &position) && position == 0,
          "digits followed by identifier rejected");
    Check(ThrowsTokenizerError("SELECT #"), "unknown hash character rejected");
    Check(ThrowsTokenizerError("SELECT 1.5", &position) && position == 8,
          "decimal point rejected until decimal literals are implemented");
    Check(ThrowsTokenizerError(".name", &position) && position == 0,
          "leading dot rejected");
    Check(ThrowsTokenizerError("table.", &position) && position == 5,
          "trailing dot rejected");
    Check(ThrowsTokenizerError("table..column", &position) && position == 5,
          "repeated dot rejected");
    Report("lexical error handling", before);

    if (g_failures == 0) {
        std::cout << "\nAll tokenizer tests passed.\n";
        return 0;
    }

    std::cout << "\n" << g_failures << " tokenizer check(s) failed.\n";
    return 1;
}
