#include <iostream>
#include <string>
#include <vector>

#include "sql/tokenizer.h"

using namespace ocera::sql;

static int g_failures = 0;

static void Check(bool cond, const std::string& what) {
    if (!cond) {
        std::cout << "    FAILED: " << what << "\n";
        ++g_failures;
    }
}

// Compares the token stream to expected (type, text) pairs; END_OF_INPUT is
// checked automatically as the last token.
struct Expect {
    TokenType type;
    std::string text;
};

static void CheckTokens(const std::string& sql, const std::vector<Expect>& expected) {
    std::vector<Token> toks = Tokenize(sql);
    Check(toks.size() == expected.size() + 1,
          "token count for: " + sql + " (got " + std::to_string(toks.size() - 1) +
              ", expected " + std::to_string(expected.size()) + ")");
    if (toks.size() != expected.size() + 1) return;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        Check(toks[i].type == expected[i].type &&
                  toks[i].text == expected[i].text,
              "token " + std::to_string(i) + " of: " + sql + " (got " +
                  TokenTypeName(toks[i].type) + " '" + toks[i].text + "')");
    }
    Check(toks.back().type == TokenType::END_OF_INPUT, "ends with END_OF_INPUT");
}

static bool Throws(const std::string& sql, std::size_t* pos = nullptr) {
    try {
        Tokenize(sql);
    } catch (const TokenizerError& e) {
        if (pos) *pos = e.position();
        return true;
    }
    return false;
}

static void Report(const char* name, int failures_before) {
    std::cout << (g_failures == failures_before ? "    PASSED: " : "    (see failures above) ")
              << name << "\n";
}

int main() {
    std::cout << "=== OCERA SQL Tokenizer Test ===\n\n";
    using T = TokenType;
    int before;

    std::cout << "[1] Basic SELECT statement\n";
    before = g_failures;
    CheckTokens("SELECT name FROM students WHERE id = 5;",
                {{T::KEYWORD, "SELECT"}, {T::IDENTIFIER, "name"}, {T::KEYWORD, "FROM"},
                 {T::IDENTIFIER, "students"}, {T::KEYWORD, "WHERE"}, {T::IDENTIFIER, "id"},
                 {T::OPERATOR, "="}, {T::INTEGER, "5"}, {T::SYMBOL, ";"}});
    Report("basic SELECT", before);

    std::cout << "[2] Keywords are case-insensitive, identifiers keep their case\n";
    before = g_failures;
    CheckTokens("sElEcT Name fRoM T",
                {{T::KEYWORD, "SELECT"}, {T::IDENTIFIER, "Name"}, {T::KEYWORD, "FROM"},
                 {T::IDENTIFIER, "T"}});
    Report("case handling", before);

    std::cout << "[3] CREATE TABLE and INSERT\n";
    before = g_failures;
    CheckTokens("CREATE TABLE t (id INT, name TEXT);",
                {{T::KEYWORD, "CREATE"}, {T::KEYWORD, "TABLE"}, {T::IDENTIFIER, "t"},
                 {T::SYMBOL, "("}, {T::IDENTIFIER, "id"}, {T::KEYWORD, "INT"}, {T::SYMBOL, ","},
                 {T::IDENTIFIER, "name"}, {T::KEYWORD, "TEXT"}, {T::SYMBOL, ")"}, {T::SYMBOL, ";"}});
    CheckTokens("INSERT INTO t VALUES (1, 'Asha');",
                {{T::KEYWORD, "INSERT"}, {T::KEYWORD, "INTO"}, {T::IDENTIFIER, "t"},
                 {T::KEYWORD, "VALUES"}, {T::SYMBOL, "("}, {T::INTEGER, "1"}, {T::SYMBOL, ","},
                 {T::STRING, "Asha"}, {T::SYMBOL, ")"}, {T::SYMBOL, ";"}});
    Report("CREATE / INSERT", before);

    std::cout << "[4] UPDATE, DELETE, AND/OR, star\n";
    before = g_failures;
    CheckTokens("UPDATE t SET a = 1 WHERE b = 2 AND c = 3 OR d = 4",
                {{T::KEYWORD, "UPDATE"}, {T::IDENTIFIER, "t"}, {T::KEYWORD, "SET"},
                 {T::IDENTIFIER, "a"}, {T::OPERATOR, "="}, {T::INTEGER, "1"},
                 {T::KEYWORD, "WHERE"}, {T::IDENTIFIER, "b"}, {T::OPERATOR, "="}, {T::INTEGER, "2"},
                 {T::KEYWORD, "AND"}, {T::IDENTIFIER, "c"}, {T::OPERATOR, "="}, {T::INTEGER, "3"},
                 {T::KEYWORD, "OR"}, {T::IDENTIFIER, "d"}, {T::OPERATOR, "="}, {T::INTEGER, "4"}});
    CheckTokens("DELETE FROM t", {{T::KEYWORD, "DELETE"}, {T::KEYWORD, "FROM"}, {T::IDENTIFIER, "t"}});
    CheckTokens("SELECT * FROM t", {{T::KEYWORD, "SELECT"}, {T::SYMBOL, "*"},
                                    {T::KEYWORD, "FROM"}, {T::IDENTIFIER, "t"}});
    Report("UPDATE / DELETE / AND / OR / *", before);

    std::cout << "[5] All comparison operators (including two-character ones)\n";
    before = g_failures;
    CheckTokens("a=1 a!=1 a<1 a>1 a<=1 a>=1",
                {{T::IDENTIFIER, "a"}, {T::OPERATOR, "="}, {T::INTEGER, "1"},
                 {T::IDENTIFIER, "a"}, {T::OPERATOR, "!="}, {T::INTEGER, "1"},
                 {T::IDENTIFIER, "a"}, {T::OPERATOR, "<"}, {T::INTEGER, "1"},
                 {T::IDENTIFIER, "a"}, {T::OPERATOR, ">"}, {T::INTEGER, "1"},
                 {T::IDENTIFIER, "a"}, {T::OPERATOR, "<="}, {T::INTEGER, "1"},
                 {T::IDENTIFIER, "a"}, {T::OPERATOR, ">="}, {T::INTEGER, "1"}});
    Report("operators", before);

    std::cout << "[6] Integers: zero, large, negative\n";
    before = g_failures;
    CheckTokens("0 12345 -7", {{T::INTEGER, "0"}, {T::INTEGER, "12345"}, {T::INTEGER, "-7"}});
    Report("integers", before);

    std::cout << "[7] Strings: empty, spaces, escaped quote, keyword inside string\n";
    before = g_failures;
    CheckTokens("''", {{T::STRING, ""}});
    CheckTokens("'hello world'", {{T::STRING, "hello world"}});
    CheckTokens("'it''s'", {{T::STRING, "it's"}});
    CheckTokens("'SELECT FROM'", {{T::STRING, "SELECT FROM"}});
    Report("strings", before);

    std::cout << "[8] Whitespace variety and empty input\n";
    before = g_failures;
    CheckTokens("  SELECT\n\t*\r\nFROM   t  ", {{T::KEYWORD, "SELECT"}, {T::SYMBOL, "*"},
                                                {T::KEYWORD, "FROM"}, {T::IDENTIFIER, "t"}});
    CheckTokens("", {});
    CheckTokens("   ", {});
    Report("whitespace / empty", before);

    std::cout << "[9] Identifiers with underscores and digits; keyword prefix is not a keyword\n";
    before = g_failures;
    CheckTokens("_x student_1 selection",
                {{T::IDENTIFIER, "_x"}, {T::IDENTIFIER, "student_1"}, {T::IDENTIFIER, "selection"}});
    Report("identifiers", before);

    std::cout << "[10] Token positions\n";
    before = g_failures;
    {
        std::vector<Token> toks = Tokenize("ab  = 'x'");
        Check(toks[0].position == 0, "ab at 0");
        Check(toks[1].position == 4, "= at 4");
        Check(toks[2].position == 6, "string at 6");
    }
    Report("positions", before);

    std::cout << "[11] Errors are reported, not silently accepted\n";
    before = g_failures;
    std::size_t pos = 0;
    Check(Throws("SELECT 'abc", &pos) && pos == 7, "unterminated string, position 7");
    Check(Throws("SELECT @ FROM t", &pos) && pos == 7, "unknown character '@', position 7");
    Check(Throws("a ! b", &pos) && pos == 2, "lone '!' rejected");
    Check(Throws("12abc", &pos) && pos == 0, "digits followed by letters rejected");
    Check(Throws("SELECT #"), "unknown character '#'");
    Report("error cases", before);

    if (g_failures == 0) {
        std::cout << "\nAll tokenizer tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " tokenizer check(s) FAILED.\n";
    return 1;
}
