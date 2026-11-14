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
        auto keyed = ParseAs<CreateTableStatement>(
            "CREATE TABLE enrollments (student_id INT, course_id INT, "
            "PRIMARY KEY (student_id, course_id), email TEXT UNIQUE)");
        Check(keyed.columns.size() == 3, "keyed table columns");
        Check(keyed.key_constraints.size() == 2, "table and column constraints");
        if (keyed.key_constraints.size() == 2) {
            Check(keyed.key_constraints[0].type == KeyConstraintType::PRIMARY_KEY &&
                  keyed.key_constraints[0].columns.size() == 2,
                  "composite primary key parsed");
            Check(keyed.key_constraints[1].type == KeyConstraintType::UNIQUE &&
                  keyed.key_constraints[1].columns[0] == "email",
                  "column UNIQUE parsed");
        }
        auto single = ParseAs<CreateTableStatement>(
            "CREATE TABLE users (id INT PRIMARY KEY, email TEXT UNIQUE)");
        Check(single.key_constraints.size() == 2, "column-level key constraints parsed");
        Check(ParseFails("CREATE TABLE bad (a INT, PRIMARY KEY ())"),
              "empty primary-key column list rejected");
        Check(ParseFails("CREATE TABLE bad (a INT, UNIQUE (a,))"),
              "trailing comma in unique key rejected");
    }
    Report("CREATE TABLE and key constraints", before);

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
    Check(ParseFails("SELECT * FROM t extra junk"), "extra tokens after a valid bare table alias");
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
    {
        auto s = ParseAs<DropTableStatement>("DROP TABLE t");
        Check(s.table == "t", "DROP TABLE parsed");
    }
    Check(ParseFails("SELECT * FROM a, b"), "comma-separated FROM tables remain unsupported");
    {
        auto inner = ParseAs<SelectStatement>(
            "SELECT a.id, b.name FROM a INNER JOIN b ON a.id = b.a_id WHERE b.name LIKE 'A%'");
        Check(inner.table == "a" && inner.joins.size() == 1,
              "INNER JOIN source and clause parsed");
        if (!inner.joins.empty()) {
            Check(inner.joins[0].type == JoinType::INNER && inner.joins[0].table == "b",
                  "INNER JOIN type and table retained");
            Check(inner.joins[0].on && inner.joins[0].on->left_column == "a.id" &&
                  inner.joins[0].on->right_column == "b.a_id",
                  "qualified JOIN equality retained");
        }
        Check(inner.where.has_value(), "WHERE after JOIN parsed");

        auto left = ParseAs<SelectStatement>(
            "SELECT * FROM a LEFT OUTER JOIN b ON a.id = b.a_id");
        Check(left.joins.size() == 1 && left.joins[0].type == JoinType::LEFT,
              "LEFT OUTER JOIN parsed");

        auto right = ParseAs<SelectStatement>(
            "SELECT * FROM a RIGHT JOIN b ON a.id = b.a_id");
        Check(right.joins.size() == 1 && right.joins[0].type == JoinType::RIGHT,
              "RIGHT JOIN parsed");

        auto full = ParseAs<SelectStatement>(
            "SELECT * FROM a FULL OUTER JOIN b ON a.id = b.a_id");
        Check(full.joins.size() == 1 && full.joins[0].type == JoinType::FULL_OUTER,
              "FULL OUTER JOIN parsed");

        auto cross = ParseAs<SelectStatement>("SELECT * FROM a CROSS JOIN b");
        Check(cross.joins.size() == 1 && cross.joins[0].type == JoinType::CROSS &&
              !cross.joins[0].on, "CROSS JOIN parsed without ON");

        auto chained = ParseAs<SelectStatement>(
            "SELECT * FROM a JOIN b ON a.id = b.a_id JOIN c ON b.id = c.b_id");
        Check(chained.joins.size() == 2, "chained JOIN clauses parsed");

        Check(ParseFails("SELECT * FROM a LEFT JOIN b"),
              "non-CROSS JOIN requires ON");
        Check(ParseFails("SELECT * FROM a CROSS JOIN b ON a.id = b.id"),
              "CROSS JOIN rejects ON");
        Check(ParseFails("SELECT * FROM a JOIN b ON a.id > b.id"),
              "JOIN ON rejects non-equality operator");
    }
    {
        auto s = ParseAs<InsertStatement>("INSERT INTO t VALUES (  'x'  )");
        Check(s.values.size() == 1 && s.values[0].position == 24, "value position recorded");
    }
    Report("edge cases", before);

    std::cout << "[23] DISTINCT, ORDER BY, LIMIT and OFFSET" << std::endl;
    before = g_failures;
    {
        auto ordered = ParseAs<SelectStatement>(
            "SELECT DISTINCT name FROM students WHERE id > 0 "
            "ORDER BY age DESC, name ASC LIMIT 5 OFFSET 2");
        Check(ordered.distinct, "DISTINCT flag retained");
        Check(ordered.order_by.size() == 2, "multiple ORDER BY items retained");
        if (ordered.order_by.size() == 2) {
            Check(ordered.order_by[0].column == "age" && ordered.order_by[0].descending,
                  "first ORDER BY item DESC");
            Check(ordered.order_by[1].column == "name" && !ordered.order_by[1].descending,
                  "second ORDER BY item ASC");
        }
        Check(ordered.limit && *ordered.limit == 5 && ordered.offset == 2,
              "LIMIT and OFFSET retained");
        auto offset_only = ParseAs<SelectStatement>("SELECT * FROM students OFFSET 3");
        Check(!offset_only.limit && offset_only.offset == 3, "OFFSET without LIMIT");
        Check(ParseFails("SELECT * FROM t LIMIT -1"), "negative LIMIT rejected");
        Check(ParseFails("SELECT * FROM t OFFSET -2"), "negative OFFSET rejected");
        Check(ParseFails("SELECT * FROM t LIMIT"), "missing LIMIT value rejected");
        Check(ParseFails("SELECT * FROM t ORDER BY"), "missing ORDER BY column rejected");
        Check(ParseFails("SELECT * FROM t LIMIT 1 OFFSET 2 OFFSET 3"),
              "duplicate OFFSET rejected");
    }
    Report("SELECT modifiers", before);

    std::cout << "[24] GROUP BY and HAVING" << std::endl;
    before = g_failures;
    {
        auto grouped = ParseAs<SelectStatement>(
            "SELECT department, region FROM staff "
            "WHERE id > 0 GROUP BY department, region "
            "HAVING department = 'Engineering' ORDER BY region DESC LIMIT 4");
        Check(grouped.group_by.size() == 2, "multiple GROUP BY columns retained");
        if (grouped.group_by.size() == 2) {
            Check(grouped.group_by[0] == "department", "first grouping column retained");
            Check(grouped.group_by[1] == "region", "second grouping column retained");
        }
        Check(grouped.having.has_value() && grouped.having->predicates.size() == 1,
              "HAVING condition retained");
        Check(ParseFails("SELECT department FROM staff GROUP BY"),
              "missing GROUP BY column rejected");
        Check(ParseFails("SELECT department FROM staff GROUP BY department HAVING"),
              "missing HAVING predicate rejected");
    }
    Report("GROUP BY and HAVING", before);

    std::cout << "[22] FOREIGN KEY declarations" << std::endl;
    before = g_failures;
    {
        auto single = ParseAs<CreateTableStatement>(
            "CREATE TABLE enrollments (student_id INT, "
            "FOREIGN KEY (student_id) REFERENCES students (id))");
        Check(single.foreign_key_constraints.size() == 1,
              "single-column foreign key parsed");
        if (!single.foreign_key_constraints.empty()) {
            const auto& fk = single.foreign_key_constraints[0];
            Check(fk.columns.size() == 1 && fk.columns[0] == "student_id",
                  "local foreign-key column retained");
            Check(fk.referenced_table == "students" &&
                  fk.referenced_columns.size() == 1 &&
                  fk.referenced_columns[0] == "id",
                  "referenced table and column retained");
        }

        auto composite = ParseAs<CreateTableStatement>(
            "CREATE TABLE enrollment (student_id INT, course_id INT, "
            "FOREIGN KEY (student_id, course_id) "
            "REFERENCES student_courses (student_id, course_id))");
        Check(composite.foreign_key_constraints.size() == 1,
              "composite foreign key parsed");
        if (!composite.foreign_key_constraints.empty()) {
            const auto& fk = composite.foreign_key_constraints[0];
            Check(fk.columns.size() == 2 && fk.referenced_columns.size() == 2,
                  "both composite column lists retained");
            Check(fk.columns[1] == "course_id" &&
                  fk.referenced_columns[1] == "course_id",
                  "composite column order retained");
        }
        Check(ParseFails("CREATE TABLE bad (a INT, FOREIGN KEY () REFERENCES p (id))"),
              "empty local foreign-key list rejected");
        Check(ParseFails("CREATE TABLE bad (a INT, FOREIGN KEY (a) REFERENCES p ())"),
              "empty referenced-column list rejected");
        Check(ParseFails("CREATE TABLE bad (a INT, FOREIGN KEY (a,) REFERENCES p (id))"),
              "trailing comma in local foreign-key list rejected");
        Check(ParseFails("CREATE TABLE bad (a INT, FOREIGN KEY (a) REFERENCES p (id,))"),
              "trailing comma in referenced list rejected");
        Check(ParseFails("CREATE TABLE bad (a INT, FOREIGN (a) REFERENCES p (id))"),
              "FOREIGN requires KEY");
        Check(ParseFails("CREATE TABLE bad (a INT, FOREIGN KEY (a) p (id))"),
              "REFERENCES keyword required");
    }
    Report("FOREIGN KEY declarations", before);

    std::cout << "[25] Aggregate expressions and aggregate HAVING" << std::endl;
    before = g_failures;
    {
        auto aggregate = ParseAs<SelectStatement>(
            "SELECT department, COUNT(*), COUNT(id), SUM(id), AVG(id), MIN(id), MAX(id) "
            "FROM staff GROUP BY department HAVING COUNT(*) > 1");
        Check(aggregate.select_items.size() == 7, "aggregate and plain select items retained");
        Check(aggregate.select_items[0].column == "department" &&
              !aggregate.select_items[0].aggregate, "plain select item retained");
        Check(aggregate.select_items[1].aggregate &&
              aggregate.select_items[1].aggregate->function == AggregateFunction::COUNT &&
              aggregate.select_items[1].aggregate->star, "COUNT(*) parsed");
        Check(aggregate.select_items[3].aggregate &&
              aggregate.select_items[3].aggregate->function == AggregateFunction::SUM &&
              aggregate.select_items[3].aggregate->column == "id", "SUM(column) parsed");
        Check(aggregate.having && aggregate.having->predicates[0].aggregate &&
              aggregate.having->predicates[0].aggregate->function == AggregateFunction::COUNT,
              "aggregate HAVING parsed");
        Check(ParseFails("SELECT SUM(*) FROM staff"), "SUM(*) rejected");
        Check(ParseFails("SELECT COUNT( FROM staff"), "malformed aggregate call rejected");
    }
    Report("aggregate expressions", before);

    std::cout << "[26] Table aliases" << std::endl;
    before = g_failures;
    {
        auto base_as = ParseAs<SelectStatement>(
            "SELECT e.id FROM employees AS e");
        Check(base_as.table == "employees" && base_as.table_alias == "e",
              "base table alias with AS retained");

        auto base_bare = ParseAs<SelectStatement>(
            "SELECT e.id FROM employees e");
        Check(base_bare.table == "employees" && base_bare.table_alias == "e",
              "base table alias without AS retained");

        auto joined = ParseAs<SelectStatement>(
            "SELECT e.id FROM employees e JOIN departments AS d "
            "ON e.dept_id = d.dept_id");
        Check(joined.table_alias == "e" && joined.joins.size() == 1 &&
              joined.joins[0].table == "departments" &&
              joined.joins[0].alias == "d",
              "joined table alias retained");

        auto cross = ParseAs<SelectStatement>(
            "SELECT e.id FROM employees e CROSS JOIN departments d");
        Check(cross.joins.size() == 1 && cross.joins[0].alias == "d",
              "CROSS JOIN alias retained");
        Check(ParseFails("SELECT * FROM employees AS"),
              "AS without an alias rejected");
    }
    Report("table aliases", before);

    if (g_failures == 0) {
        std::cout << "\nAll parser tests passed.\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " parser check(s) FAILED.\n";
    return 1;
}