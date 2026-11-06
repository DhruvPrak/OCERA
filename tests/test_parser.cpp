#include <iostream>
#include <string>
#include <variant>

#include "sql/parser.h"
#include "sql/tokenizer.h"

using namespace ocera::sql;

static int g_failures = 0;

static void Check(bool cond, const std::string& what) {
    if (!cond) {
        std::cout << "    FAILED: " << what << "\n";
        ++g_failures;
    }
}

static void Report(const char* name, int failures_before) {
    std::cout << (g_failures == failures_before ? "    PASSED: " : "    (see failures above) ")
              << name << "\n";
}

// Parses `sql` and returns the statement as type T. If parsing throws or the
// statement is a different type, records a failure and returns a default T.
template <class T>
static T ParseAs(const std::string& sql) {
    try {
        Statement s = Parse(sql);
        if (T* p = std::get_if<T>(&s)) return *p;
        Check(false, "wrong statement type for: " + sql);
    } catch (const std::exception& e) {
        Check(false, "unexpected exception for: " + sql + " -> " + e.what());
    }
    return T{};
}

// True if Parse throws ParseError (grammar error); reports its position.
static bool ParseFails(const std::string& sql, std::size_t* pos = nullptr) {
    try {
        Parse(sql);
    } catch (const ParseError& e) {
        if (pos) *pos = e.position();
        return true;
    } catch (const std::exception&) {
        return false;  // some other error type: not what we expect here
    }
    return false;
}

static bool IsInt(const Value& v, long long n) {
    return v.type == Value::Type::INTEGER && v.int_value == n;
}
static bool IsStr(const Value& v, const std::string& s) {
    return v.type == Value::Type::STRING && v.string_value == s;
}

int main() {
    std::cout << "=== OCERA SQL Parser Test ===\n\n";
    int before;
    std::size_t pos = 0;

    std::cout << "[1] CREATE TABLE\n";
    before = g_failures;
    {
        auto s = ParseAs<CreateTableStatement>("CREATE TABLE students (id INT, name TEXT);");
        Check(s.table == "students", "table name");
        Check(s.columns.size() == 2, "two columns");
        if (s.columns.size() == 2) {
            Check(s.columns[0].name == "id" && s.columns[0].type == ColumnType::INT, "id INT");
            Check(s.columns[1].name == "name" && s.columns[1].type == ColumnType::TEXT, "name TEXT");
            Check(s.columns[1].position == 31, "column position recorded");
        }
        auto one = ParseAs<CreateTableStatement>("CREATE TABLE t (a INT)");
        Check(one.columns.size() == 1, "single column, no semicolon");
    }
    Report("CREATE TABLE", before);

    std::cout << "[2] INSERT with integers\n";
    before = g_failures;
    {
        auto s = ParseAs<InsertStatement>("INSERT INTO t VALUES (1, -7, 0);");
        Check(s.table == "t", "table name");
        Check(s.values.size() == 3, "three values");
        if (s.values.size() == 3) {
            Check(IsInt(s.values[0], 1) && IsInt(s.values[1], -7) && IsInt(s.values[2], 0),
                  "integer values in order");
        }
    }
    Report("INSERT integers", before);

    std::cout << "[3] INSERT with strings and escaped quotes\n";
    before = g_failures;
    {
        auto s = ParseAs<InsertStatement>("INSERT INTO students VALUES (1, 'Asha');");
        Check(s.values.size() == 2 && IsInt(s.values[0], 1) && IsStr(s.values[1], "Asha"),
              "int and string");
        auto q = ParseAs<InsertStatement>("INSERT INTO t VALUES ('it''s', '')");
        Check(q.values.size() == 2 && IsStr(q.values[0], "it's") && IsStr(q.values[1], ""),
              "escaped quote and empty string");
    }
    Report("INSERT strings", before);

    std::cout << "[4] SELECT *\n";
    before = g_failures;
    {
        auto s = ParseAs<SelectStatement>("SELECT * FROM students;");
        Check(s.table == "students", "table name");
        Check(s.select_all && s.columns.empty(), "select_all, no column list");
        Check(!s.where.has_value(), "no WHERE");
    }
    Report("SELECT *", before);

    std::cout << "[5] SELECT with column list\n";
    before = g_failures;
    {
        auto s = ParseAs<SelectStatement>("SELECT id, name FROM students");
        Check(!s.select_all, "not select_all");
        Check(s.columns.size() == 2 && s.columns[0] == "id" && s.columns[1] == "name",
              "columns in order");
    }
    Report("SELECT columns", before);

    std::cout << "[6] SELECT with WHERE\n";
    before = g_failures;
    {
        auto s = ParseAs<SelectStatement>("SELECT name FROM students WHERE id = 7;");
        Check(s.where.has_value(), "WHERE present");
        if (s.where) {
            Check(s.where->comparisons.size() == 1 && s.where->connectors.empty(),
                  "one comparison, no connectors");
            const Comparison& c = s.where->comparisons[0];
            Check(c.column == "id" && c.op == CompareOp::EQ && IsInt(c.value, 7),
                  "id = 7");
        }
    }
    Report("SELECT WHERE", before);

    std::cout << "[7] All comparison operators\n";
    before = g_failures;
    {
        struct Case { const char* text; CompareOp op; };
        const Case cases[] = {{"=", CompareOp::EQ}, {"!=", CompareOp::NE},
                              {"<", CompareOp::LT}, {">", CompareOp::GT},
                              {"<=", CompareOp::LE}, {">=", CompareOp::GE}};
        for (const Case& k : cases) {
            auto s = ParseAs<DeleteStatement>(std::string("DELETE FROM t WHERE a ") + k.text + " 5");
            Check(s.where && s.where->comparisons.size() == 1 &&
                      s.where->comparisons[0].op == k.op,
                  std::string("operator ") + k.text);
        }
        auto str = ParseAs<SelectStatement>("SELECT * FROM t WHERE name = 'Asha'");
        Check(str.where && IsStr(str.where->comparisons[0].value, "Asha"), "string comparison value");
    }
    Report("operators", before);

    std::cout << "[8] UPDATE\n";
    before = g_failures;
    {
        auto s = ParseAs<UpdateStatement>("UPDATE students SET name = 'Asha' WHERE id = 1;");
        Check(s.table == "students", "table name");
        Check(s.assignments.size() == 1 && s.assignments[0].column == "name" &&
                  IsStr(s.assignments[0].value, "Asha"),
              "one assignment");
        Check(s.where && s.where->comparisons.size() == 1 &&
                  s.where->comparisons[0].column == "id",
              "WHERE id = 1");
        auto m = ParseAs<UpdateStatement>("UPDATE t SET a = 1, b = 'x'");
        Check(m.assignments.size() == 2 && m.assignments[0].column == "a" &&
                  IsInt(m.assignments[0].value, 1) && m.assignments[1].column == "b" &&
                  IsStr(m.assignments[1].value, "x") && !m.where.has_value(),
              "two assignments, no WHERE");
    }
    Report("UPDATE", before);

    std::cout << "[9] DELETE\n";
    before = g_failures;
    {
        auto s = ParseAs<DeleteStatement>("DELETE FROM students WHERE id = 1;");
        Check(s.table == "students" && s.where && s.where->comparisons.size() == 1, "with WHERE");
        auto all = ParseAs<DeleteStatement>("DELETE FROM students");
        Check(all.table == "students" && !all.where.has_value(), "without WHERE");
    }
    Report("DELETE", before);

    std::cout << "[10] AND / OR kept as a flat ordered list\n";
    before = g_failures;
    {
        auto s = ParseAs<SelectStatement>("SELECT * FROM t WHERE a = 1 AND b > 2 OR c <= 3");
        Check(s.where.has_value(), "WHERE present");
        if (s.where) {
            Check(s.where->comparisons.size() == 3, "three comparisons");
            Check(s.where->connectors.size() == 2 && s.where->connectors[0] == LogicalOp::AND &&
                      s.where->connectors[1] == LogicalOp::OR,
                  "connectors AND, OR in order");
            if (s.where->comparisons.size() == 3) {
                Check(s.where->comparisons[0].column == "a" && s.where->comparisons[1].column == "b" &&
                          s.where->comparisons[2].column == "c",
                      "columns in order");
            }
        }
    }
    Report("AND / OR", before);

    std::cout << "[11] Keywords are case-insensitive\n";
    before = g_failures;
    {
        auto s = ParseAs<SelectStatement>("sElEcT Name fRoM Students wHeRe Id = 1 aNd x = 2");
        Check(s.table == "Students" && s.columns.size() == 1 && s.columns[0] == "Name",
              "identifiers keep their case");
        Check(s.where && s.where->connectors.size() == 1 && s.where->connectors[0] == LogicalOp::AND,
              "lowercase AND recognised");
        ParseAs<CreateTableStatement>("create table t (a int, b text)");
    }
    Report("case-insensitive keywords", before);

    std::cout << "[12] Empty and whitespace-only input\n";
    before = g_failures;
    Check(ParseFails(""), "empty input rejected");
    Check(ParseFails("   \n\t "), "whitespace-only input rejected");
    Check(ParseFails(";"), "lone semicolon rejected");
    Check(ParseFails("students"), "text that is not a statement rejected");
    Report("empty input", before);

    std::cout << "[13] Missing table name\n";
    before = g_failures;
    Check(ParseFails("SELECT * FROM", &pos) && pos == 13, "SELECT: error at end of input");
    Check(ParseFails("SELECT * FROM WHERE id = 1"), "SELECT: keyword is not a table name");
    Check(ParseFails("CREATE TABLE (id INT)"), "CREATE TABLE without a name");
    Check(ParseFails("INSERT INTO VALUES (1)"), "INSERT without a table");
    Check(ParseFails("UPDATE SET a = 1"), "UPDATE without a table");
    Check(ParseFails("DELETE FROM"), "DELETE without a table");
    Report("missing table name", before);

    std::cout << "[14] Missing parentheses\n";
    before = g_failures;
    Check(ParseFails("CREATE TABLE t id INT"), "CREATE: no '('");
    Check(ParseFails("CREATE TABLE t (id INT"), "CREATE: no ')'");
    Check(ParseFails("INSERT INTO t VALUES 1, 2"), "INSERT: no '('");
    Check(ParseFails("INSERT INTO t VALUES (1, 2"), "INSERT: no ')'");
    Report("missing parentheses", before);

    std::cout << "[15] Missing commas or values\n";
    before = g_failures;
    Check(ParseFails("CREATE TABLE t (id INT name TEXT)"), "CREATE: missing comma");
    Check(ParseFails("CREATE TABLE t (id INT,)"), "CREATE: trailing comma");
    Check(ParseFails("CREATE TABLE t ()"), "CREATE: no columns");
    Check(ParseFails("INSERT INTO t VALUES (1 2)"), "INSERT: missing comma");
    Check(ParseFails("INSERT INTO t VALUES (1,)"), "INSERT: trailing comma");
    Check(ParseFails("INSERT INTO t VALUES ()"), "INSERT: no values");
    Check(ParseFails("SELECT id name FROM t"), "SELECT: missing comma");
    Check(ParseFails("SELECT FROM t"), "SELECT: no columns");
    Check(ParseFails("SELECT * , id FROM t"), "SELECT: '*' mixed with a column");
    Check(ParseFails("UPDATE t SET"), "UPDATE: no assignment");
    Check(ParseFails("UPDATE t SET a = 1 b = 2"), "UPDATE: missing comma");
    Check(ParseFails("UPDATE t SET a 1"), "UPDATE: missing '='");
    Report("missing commas / values", before);

    std::cout << "[16] Invalid column type\n";
    before = g_failures;
    Check(ParseFails("CREATE TABLE t (id FLOAT)"), "unknown type name");
    Check(ParseFails("CREATE TABLE t (id)"), "missing type");
    Check(ParseFails("CREATE TABLE t (id 5)"), "number instead of a type");
    Report("invalid column type", before);

    std::cout << "[17] Invalid operator\n";
    before = g_failures;
    Check(ParseFails("SELECT * FROM t WHERE id 5"), "no operator");
    Check(ParseFails("SELECT * FROM t WHERE id == 5"), "'==' is two tokens, rejected");
    Check(ParseFails("SELECT * FROM t WHERE id AND 5"), "keyword where operator belongs");
    Check(ParseFails("SELECT * FROM t WHERE = 5"), "missing column");
    Report("invalid operator", before);

    std::cout << "[18] Missing WHERE comparison value\n";
    before = g_failures;
    Check(ParseFails("SELECT * FROM t WHERE id ="), "value missing at end");
    Check(ParseFails("SELECT * FROM t WHERE id = AND x = 1"), "value replaced by keyword");
    Check(ParseFails("SELECT * FROM t WHERE id = name"), "identifier is not a value");
    Check(ParseFails("SELECT * FROM t WHERE a = 1 AND"), "dangling AND");
    Check(ParseFails("SELECT * FROM t WHERE"), "WHERE with nothing after it");
    Report("missing comparison value", before);

    std::cout << "[19] Unexpected trailing tokens\n";
    before = g_failures;
    Check(ParseFails("SELECT * FROM t extra"), "extra word");
    Check(ParseFails("SELECT * FROM t; SELECT * FROM u"), "second statement after ';'");
    Check(ParseFails("SELECT * FROM t;;"), "double semicolon");
    Check(ParseFails("DELETE FROM t WHERE a = 1 b = 2"), "junk after WHERE");
    Check(ParseFails("SELECT * FROM t; x", &pos) && pos == 17, "position of first unexpected token");
    ParseAs<SelectStatement>("SELECT * FROM t;");
    ParseAs<SelectStatement>("SELECT * FROM t");
    Report("trailing tokens / semicolon rule", before);

    std::cout << "[20] Lexical errors propagate as TokenizerError\n";
    before = g_failures;
    {
        bool lexical = false, grammar = false;
        try {
            Parse("SELECT 'abc");
        } catch (const TokenizerError& e) {
            lexical = (e.position() == 7);
        } catch (const ParseError&) {
            grammar = true;
        }
        Check(lexical && !grammar, "unterminated string -> TokenizerError at position 7");
        lexical = false;
        try {
            Parse("SELECT @ FROM t");
        } catch (const TokenizerError&) {
            lexical = true;
        } catch (...) {
        }
        Check(lexical, "unknown character -> TokenizerError");
    }
    Report("lexical errors", before);

    std::cout << "[21] Other edge cases\n";
    before = g_failures;
    Check(ParseFails("INSERT INTO t VALUES (99999999999999999999)"), "integer out of range");
    Check(ParseFails("DROP TABLE t"), "unsupported statement");
    Check(ParseFails("SELECT * FROM a, b"), "multiple tables (no joins) rejected");
    {
        auto s = ParseAs<InsertStatement>("INSERT INTO t VALUES (  'x'  )");
        Check(s.values.size() == 1 && s.values[0].position == 24, "value position recorded");
    }
    Report("edge cases", before);

    if (g_failures == 0) {
        std::cout << "\nAll parser tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " parser check(s) FAILED.\n";
    return 1;
}
