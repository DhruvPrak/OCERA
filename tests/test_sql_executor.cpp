#include "sql/executor.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "storage/disk_manager.h"
#include "storage/free_space_manager.h"

using namespace ocera;

namespace {

bool SameValue(const sql::Value& left, const sql::Value& right) {
    if (left.type != right.type) {
        return false;
    }

    if (left.type == sql::Value::Type::INTEGER) {
        return left.int_value == right.int_value;
    }

    return left.string_value == right.string_value;
}

void CheckRow(
    SqlExecutor& executor,
    const RID& rid,
    const std::vector<sql::Value>& expected
) {
    std::vector<sql::Value> actual;
    std::string error;

    assert(executor.GetRowByRID("students", rid, &actual, &error));
    assert(actual.size() == expected.size());

    for (std::size_t i = 0; i < expected.size(); ++i) {
        assert(SameValue(actual[i], expected[i]));
    }
}

sql::Value Int(std::int64_t value) {
    sql::Value result;
    result.type = sql::Value::Type::INTEGER;
    result.int_value = value;
    return result;
}

sql::Value Text(const std::string& value) {
    sql::Value result;
    result.type = sql::Value::Type::STRING;
    result.string_value = value;
    return result;
}

}  // namespace

int main() {
    const std::string db_file = "test_sql_executor.db";
    std::remove(db_file.c_str());

    {
        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        BufferPoolManager bpm(4, disk_manager, free_space_manager);

        Catalog catalog;
        SqlExecutor executor(catalog, bpm);
        std::string error;
        RID rid;

        std::cout << "[1] Execute CREATE TABLE through SQL text\n";
        assert(executor.Execute(
            "CREATE TABLE students (id INT, name TEXT, age INT);",
            nullptr,
            &error
        ));
        assert(catalog.HasTable("students"));
        assert(catalog.NumTables() == 1);

        std::cout << "[2] Execute INSERT and retrieve the stored row\n";
        assert(executor.Execute(
            "INSERT INTO students VALUES (7, 'Asha', 20);",
            &rid,
            &error
        ));
        assert(rid.page_id >= 2);
        CheckRow(executor, rid, {Int(7), Text("Asha"), Int(20)});

        std::cout << "[3] Identifier lookup is case-insensitive\n";
        RID second_rid;
        assert(executor.Execute(
            "insert into STUDENTS values (8, 'Riya', 21)",
            &second_rid,
            &error
        ));
        CheckRow(executor, second_rid, {Int(8), Text("Riya"), Int(21)});

        std::cout << "[4] Reject duplicate table and preserve Catalog\n";
        assert(!executor.Execute(
            "CREATE TABLE STUDENTS (id INT);", nullptr, &error
        ));
        assert(!error.empty());
        assert(catalog.NumTables() == 1);

        std::cout << "[5] Reject invalid INSERT values\n";
        RID rejected_rid;
        assert(!executor.Execute(
            "INSERT INTO students VALUES ('wrong', 'Asha', 20);",
            &rejected_rid,
            &error
        ));
        assert(rejected_rid.page_id == INVALID_PAGE_ID);
        assert(!error.empty());

        assert(!executor.Execute(
            "INSERT INTO students VALUES (1, 'too few');",
            &rejected_rid,
            &error
        ));
        assert(rejected_rid.page_id == INVALID_PAGE_ID);

        assert(!executor.Execute(
            "INSERT INTO missing VALUES (1, 'Nobody', 10);",
            &rejected_rid,
            &error
        ));

        std::cout << "[6] Reject malformed SQL\n";
        assert(!executor.Execute(
            "INSERT INTO students VALUES (1, 'unterminated, 10);",
            &rejected_rid,
            &error
        ));
        assert(!error.empty());

        std::cout << "[7] SELECT * returns all columns and rows\n";
        QueryResult result;

        assert(executor.Execute(
            "SELECT * FROM students;",
            nullptr,
            &error,
            &result
        ));

        assert(result.column_names.size() == 3);
        assert(result.column_names[0] == "id");
        assert(result.column_names[1] == "name");
        assert(result.column_names[2] == "age");
        assert(result.rows.size() == 2);

        std::cout << "[8] SELECT projection and WHERE filtering\n";
        assert(executor.Execute(
            "SELECT name, age FROM students WHERE age >= 21;",
            nullptr,
            &error,
            &result
        ));

        assert(result.column_names.size() == 2);
        assert(result.column_names[0] == "name");
        assert(result.column_names[1] == "age");
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Text("Riya")));
        assert(SameValue(result.rows[0][1], Int(21)));

        std::cout << "[9] AND is evaluated before OR\n";
        assert(executor.Execute(
            "SELECT id FROM students "
            "WHERE id = 7 OR id = 8 AND age = 21;",
            nullptr,
            &error,
            &result
        ));

        // Expected: id=7 OR (id=8 AND age=21), so both rows match.
        assert(result.rows.size() == 2);
        assert(SameValue(result.rows[0][0], Int(7)));
        assert(SameValue(result.rows[1][0], Int(8)));

        std::cout << "[9a] LIKE, IN, BETWEEN, NOT and NULL predicates\n";
        assert(executor.Execute(
            "SELECT id FROM students WHERE name LIKE 'A%';",
            nullptr, &error, &result));
        assert(result.rows.size() == 1 && SameValue(result.rows[0][0], Int(7)));

        assert(executor.Execute(
            "SELECT id FROM students WHERE name NOT LIKE 'A%';",
            nullptr, &error, &result));
        assert(result.rows.size() == 1 && SameValue(result.rows[0][0], Int(8)));

        assert(executor.Execute(
            "SELECT id FROM students WHERE id IN (7, 8, 99);",
            nullptr, &error, &result));
        assert(result.rows.size() == 2);

        assert(executor.Execute(
            "SELECT id FROM students WHERE age BETWEEN 20 AND 21;",
            nullptr, &error, &result));
        assert(result.rows.size() == 2);

        assert(executor.Execute(
            "SELECT id FROM students WHERE NOT age = 20;",
            nullptr, &error, &result));
        assert(result.rows.size() == 1 && SameValue(result.rows[0][0], Int(8)));

        assert(executor.Execute(
            "SELECT id FROM students WHERE age NOT BETWEEN 20 AND 20;",
            nullptr, &error, &result));
        assert(result.rows.size() == 1 && SameValue(result.rows[0][0], Int(8)));

        assert(executor.Execute(
            "SELECT id FROM students WHERE name IS NULL;",
            nullptr, &error, &result));
        assert(result.rows.empty());

        assert(executor.Execute(
            "SELECT id FROM students WHERE name IS NOT NULL;",
            nullptr, &error, &result));
        assert(result.rows.size() == 2);

        assert(executor.Execute(
            "SELECT id FROM students WHERE id = 7 OR id = 8 AND age IN (21);",
            nullptr, &error, &result));
        assert(result.rows.size() == 2);

        assert(!executor.Execute(
            "SELECT id FROM students WHERE id LIKE '7';",
            nullptr, &error, &result));
        assert(!error.empty());
        assert(!executor.Execute(
            "SELECT id FROM students WHERE name IN (7);",
            nullptr, &error, &result));
        assert(!error.empty());
        assert(!executor.Execute(
            "SELECT id FROM students WHERE age BETWEEN 1 AND 'x';",
            nullptr, &error, &result));
        assert(!error.empty());

        std::cout << "[10] SELECT with no matching rows\n";
        assert(executor.Execute(
            "SELECT * FROM students WHERE age > 100;",
            nullptr,
            &error,
            &result
        ));
        assert(result.column_names.size() == 3);
        assert(result.rows.empty());

        std::cout << "[11] Reject invalid SELECT and clear result\n";
        assert(!executor.Execute(
            "SELECT unknown_column FROM students;",
            nullptr,
            &error,
            &result
        ));
        assert(!error.empty());
        assert(result.column_names.empty());
        assert(result.rows.empty());

        std::cout << "[12] Previously inserted rows remain unchanged\n";
        CheckRow(executor, rid, {Int(7), Text("Asha"), Int(20)});
        CheckRow(executor, second_rid, {Int(8), Text("Riya"), Int(21)});
                std::cout << "[13] UPDATE one matching row\n";

        assert(executor.Execute(
            "UPDATE students SET age = 22 WHERE id = 7;",
            nullptr,
            &error
        ));

        CheckRow(executor, rid, {Int(7), Text("Asha"), Int(22)});
        CheckRow(executor, second_rid, {Int(8), Text("Riya"), Int(21)});

        std::cout << "[14] UPDATE multiple columns\n";

        assert(executor.Execute(
            "UPDATE students SET name = 'Riya Updated', age = 25 "
            "WHERE id = 8;",
            nullptr,
            &error
        ));

        CheckRow(
            executor,
            second_rid,
            {Int(8), Text("Riya Updated"), Int(25)}
        );

        std::cout << "[15] UPDATE respects AND-before-OR precedence\n";

        assert(executor.Execute(
            "UPDATE students SET age = 30 "
            "WHERE id = 7 OR id = 8 AND age = 25;",
            nullptr,
            &error
        ));

        CheckRow(executor, rid, {Int(7), Text("Asha"), Int(30)});
        CheckRow(
            executor,
            second_rid,
            {Int(8), Text("Riya Updated"), Int(30)}
        );

        std::cout << "[16] UPDATE without WHERE affects every row\n";

        assert(executor.Execute(
            "UPDATE students SET age = 40;",
            nullptr,
            &error
        ));

        CheckRow(executor, rid, {Int(7), Text("Asha"), Int(40)});
        CheckRow(
            executor,
            second_rid,
            {Int(8), Text("Riya Updated"), Int(40)}
        );

        std::cout << "[17] Reject invalid UPDATE statements\n";

        assert(!executor.Execute(
            "UPDATE students SET age = 'wrong';",
            nullptr,
            &error
        ));
        assert(!error.empty());

        assert(!executor.Execute(
            "UPDATE students SET unknown_column = 10;",
            nullptr,
            &error
        ));
        assert(!error.empty());

        assert(!executor.Execute(
            "UPDATE missing SET age = 10;",
            nullptr,
            &error
        ));
        assert(!error.empty());

        // Failed validation must not change existing records.
        CheckRow(executor, rid, {Int(7), Text("Asha"), Int(40)});
        CheckRow(
            executor,
            second_rid,
            {Int(8), Text("Riya Updated"), Int(40)}
        );

        std::cout << "[18] DELETE one matching row\n";

        assert(executor.Execute(
            "DELETE FROM students WHERE id = 7;",
            nullptr,
            &error
        ));

        std::vector<sql::Value> deleted_row;
        assert(!executor.GetRowByRID("students", rid, &deleted_row, &error));

        CheckRow(
            executor,
            second_rid,
            {Int(8), Text("Riya Updated"), Int(40)}
        );

        std::cout << "[19] DELETE with no matching rows\n";

        assert(executor.Execute(
            "DELETE FROM students WHERE id = 999;",
            nullptr,
            &error
        ));

        assert(executor.Execute(
            "SELECT * FROM students;",
            nullptr,
            &error,
            &result
        ));

        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Int(8)));

        std::cout << "[20] DELETE without WHERE removes every row\n";

        assert(executor.Execute(
            "DELETE FROM students;",
            nullptr,
            &error
        ));

        assert(executor.Execute(
            "SELECT * FROM students;",
            nullptr,
            &error,
            &result
        ));

        assert(result.rows.empty());
        assert(result.column_names.size() == 3);

        std::cout << "[21] Reject DELETE from an unknown table\n";

        assert(!executor.Execute(
            "DELETE FROM missing;",
            nullptr,
            &error
        ));

        assert(!error.empty());
        std::cout << "[24] Reject child INSERT without a parent and preserve rows\n";
        assert(executor.Execute(
            "CREATE TABLE fk_parent (id INT PRIMARY KEY, label TEXT);",
            nullptr, &error));
        assert(executor.Execute(
            "CREATE TABLE fk_child (id INT PRIMARY KEY, parent_id INT, "
            "FOREIGN KEY (parent_id) REFERENCES fk_parent(id));",
            nullptr, &error));
        assert(!executor.Execute(
            "INSERT INTO fk_child VALUES (10, 1);", nullptr, &error));
        assert(!error.empty());
        assert(executor.Execute("SELECT * FROM fk_child;", nullptr, &error, &result));
        assert(result.rows.empty());

        std::cout << "[25] Allow valid child INSERT; reject invalid child UPDATE\n";
        assert(executor.Execute(
            "INSERT INTO fk_parent VALUES (1, 'parent one');", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO fk_child VALUES (10, 1);", nullptr, &error));
        assert(!executor.Execute(
            "UPDATE fk_child SET parent_id = 999 WHERE id = 10;", nullptr, &error));
        assert(executor.Execute("SELECT * FROM fk_child;", nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Int(10)));
        assert(SameValue(result.rows[0][1], Int(1)));

        std::cout << "[26] RESTRICT parent DELETE and referenced-key UPDATE\n";
        assert(!executor.Execute(
            "DELETE FROM fk_parent WHERE id = 1;", nullptr, &error));
        assert(!executor.Execute(
            "UPDATE fk_parent SET id = 2 WHERE id = 1;", nullptr, &error));
        assert(executor.Execute("SELECT * FROM fk_parent;", nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Int(1)));
        assert(executor.Execute(
            "DELETE FROM fk_child WHERE id = 10;", nullptr, &error));
        assert(executor.Execute(
            "DELETE FROM fk_parent WHERE id = 1;", nullptr, &error));

        std::cout << "[27] Enforce composite foreign keys\n";
        assert(executor.Execute(
            "CREATE TABLE fk_parent_pair (a INT, b INT, "
            "PRIMARY KEY (a, b));", nullptr, &error));
        assert(executor.Execute(
            "CREATE TABLE fk_child_pair (id INT PRIMARY KEY, x INT, y INT, "
            "FOREIGN KEY (x, y) REFERENCES fk_parent_pair(a, b));",
            nullptr, &error));
        assert(!executor.Execute(
            "INSERT INTO fk_child_pair VALUES (1, 7, 8);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO fk_parent_pair VALUES (7, 8);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO fk_child_pair VALUES (1, 7, 8);", nullptr, &error));
        assert(!executor.Execute(
            "UPDATE fk_child_pair SET y = 9 WHERE id = 1;", nullptr, &error));
        assert(!executor.Execute(
            "DELETE FROM fk_parent_pair WHERE a = 7 AND b = 8;",
            nullptr, &error));
        assert(!executor.Execute(
            "UPDATE fk_parent_pair SET b = 9 WHERE a = 7 AND b = 8;",
            nullptr, &error));
        assert(executor.Execute(
            "SELECT * FROM fk_parent_pair;", nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Int(7)));
        assert(SameValue(result.rows[0][1], Int(8)));
        assert(executor.Execute(
            "DELETE FROM fk_child_pair WHERE id = 1;", nullptr, &error));
        assert(executor.Execute(
            "DELETE FROM fk_parent_pair WHERE a = 7 AND b = 8;",
            nullptr, &error));

        std::cout << "[28] Enforce self-referencing foreign keys\n";
        assert(executor.Execute(
            "CREATE TABLE fk_employee (id INT PRIMARY KEY, manager_id INT, "
            "FOREIGN KEY (manager_id) REFERENCES fk_employee(id));",
            nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO fk_employee VALUES (1, 1);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO fk_employee VALUES (2, 1);", nullptr, &error));
        assert(!executor.Execute(
            "INSERT INTO fk_employee VALUES (3, 99);", nullptr, &error));
        std::cout << "[29] CREATE INDEX on existing rows\n";

assert(executor.Execute(
    "CREATE TABLE index_students (id INT, age INT, name TEXT);",
    nullptr, &error));

assert(executor.Execute(
    "INSERT INTO index_students VALUES (1, 20, 'Asha');",
    nullptr, &error));

assert(executor.Execute(
    "INSERT INTO index_students VALUES (2, 20, 'Riya');",
    nullptr, &error));

assert(executor.Execute(
    "CREATE INDEX idx_students_age ON index_students(age);",
    nullptr, &error));

assert(catalog.HasIndex("idx_students_age"));
assert(catalog.GetIndexForColumn("index_students", "age") != nullptr);

std::cout << "[29a] Indexed SELECT returns duplicate-key rows\n";
assert(executor.Execute(
    "SELECT id FROM index_students WHERE age = 20;",
    nullptr, &error, &result));
assert(result.rows.size() == 2);

std::cout << "[29b] INSERT maintains an existing index\n";
assert(executor.Execute(
    "INSERT INTO index_students VALUES (3, 30, 'Mina');",
    nullptr, &error));
assert(executor.Execute(
    "SELECT id FROM index_students WHERE age = 30;",
    nullptr, &error, &result));
assert(result.rows.size() == 1);
assert(SameValue(result.rows[0][0], Int(3)));

std::cout << "[29c] UPDATE moves the index entry\n";
assert(executor.Execute(
    "UPDATE index_students SET age = 31 WHERE id = 3;",
    nullptr, &error));
assert(executor.Execute(
    "SELECT id FROM index_students WHERE age = 30;",
    nullptr, &error, &result));
assert(result.rows.empty());
assert(executor.Execute(
    "SELECT id FROM index_students WHERE age = 31;",
    nullptr, &error, &result));
assert(result.rows.size() == 1);
assert(SameValue(result.rows[0][0], Int(3)));

std::cout << "[29d-1] Negated comparison bypasses index candidate filtering\n";
assert(executor.Execute(
    "SELECT id FROM index_students WHERE NOT age = 20;",
    nullptr, &error, &result));
assert(result.rows.size() == 1);
assert(SameValue(result.rows[0][0], Int(3)));

std::cout << "[29d] OR predicate uses safe scan fallback\n";
assert(executor.Execute(
    "SELECT id FROM index_students WHERE age = 20 OR id = 3;",
    nullptr, &error, &result));
assert(result.rows.size() == 3);

std::cout << "[29e] Indexed range scan returns matching rows\n";
assert(executor.Execute(
    "SELECT id FROM index_students WHERE age >= 20 AND age < 21;",
    nullptr, &error, &result));
assert(result.rows.size() == 2);

std::cout << "[29f] DELETE removes the index entry\n";
assert(executor.Execute(
    "DELETE FROM index_students WHERE age = 31;",
    nullptr, &error));
assert(executor.Execute(
    "SELECT id FROM index_students WHERE age = 31;",
    nullptr, &error, &result));
assert(result.rows.empty());

std::cout << "[30] Reject duplicate index creation\n";

assert(!executor.Execute(
    "CREATE INDEX idx_students_age_2 ON index_students(age);",
    nullptr, &error));

assert(!error.empty());
assert(!catalog.HasIndex("idx_students_age_2"));

assert(!executor.Execute(
    "CREATE INDEX idx_students_age ON index_students(id);",
    nullptr, &error));

assert(!error.empty());

std::cout << "[31] Reject indexes on unsupported column types\n";

assert(!executor.Execute(
    "CREATE INDEX idx_students_name ON index_students(name);",
    nullptr, &error));

assert(!error.empty());
assert(!catalog.HasIndex("idx_students_name"));

std::cout << "[32] CREATE INDEX on an empty table\n";

assert(executor.Execute(
    "CREATE TABLE empty_index_table (id INT);",
    nullptr, &error));

assert(executor.Execute(
    "CREATE INDEX idx_empty_id ON empty_index_table(id);",
    nullptr, &error));

assert(catalog.HasIndex("idx_empty_id"));
        std::cout << "[30] SHOW TABLES\n";

        const bool show_ok =
            executor.Execute("SHOW TABLES;", nullptr, &error, &result);

        if (!show_ok) {
            std::cerr << "SHOW TABLES error: " << error << '\n';
        }
        assert(show_ok);

        assert(result.column_names.size() == 1);
        assert(result.column_names[0] == "Tables");
        assert(!result.rows.empty());

        std::cout << "[31] DESC students\n";

        assert(executor.Execute(
            "DESC students;",
            nullptr,
            &error,
            &result
        ));

        assert(result.column_names.size() == 2);
        assert(result.rows.size() == 3);
        assert(result.rows[0][0].string_value == "id");
        assert(result.rows[1][0].string_value == "name");
        assert(result.rows[2][0].string_value == "age");

        std::cout << "[32] DESC unknown table should fail\n";

        assert(!executor.Execute(
            "DESC unknown_table;",
            nullptr,
            &error,
            &result
        ));

        assert(!error.empty());


        std::cout << "[33] UPDATE and DELETE use extended predicates\n";
        assert(executor.Execute(
            "CREATE TABLE condition_rows (id INT, name TEXT);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO condition_rows VALUES (1, 'Alpha');", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO condition_rows VALUES (2, 'Beta');", nullptr, &error));
        assert(executor.Execute(
            "UPDATE condition_rows SET name = 'Changed' WHERE id IN (1);",
            nullptr, &error));
        assert(executor.Execute(
            "SELECT id FROM condition_rows WHERE name LIKE 'Ch%';",
            nullptr, &error, &result));
        assert(result.rows.size() == 1 && SameValue(result.rows[0][0], Int(1)));
        assert(executor.Execute(
            "DELETE FROM condition_rows WHERE id BETWEEN 1 AND 1;",
            nullptr, &error));
        assert(executor.Execute(
            "SELECT id FROM condition_rows;", nullptr, &error, &result));
        assert(result.rows.size() == 1 && SameValue(result.rows[0][0], Int(2)));

        std::cout << "[34] INNER JOIN returns only matching rows\n";
        assert(executor.Execute(
            "CREATE TABLE departments (dept_id INT, dept_name TEXT);", nullptr, &error));
        assert(executor.Execute(
            "CREATE TABLE employees (emp_id INT, emp_name TEXT, dept_id INT);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO departments VALUES (10, 'Engineering');", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO departments VALUES (20, 'Sales');", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO departments VALUES (30, 'HR');", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO employees VALUES (1, 'Alice', 10);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO employees VALUES (2, 'Bob', 20);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO employees VALUES (3, 'Cara', 99);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO employees VALUES (4, 'Dan', 10);", nullptr, &error));

        assert(executor.Execute(
            "SELECT employees.emp_name, departments.dept_name "
            "FROM employees INNER JOIN departments "
            "ON employees.dept_id = departments.dept_id;",
            nullptr, &error, &result));
        assert(result.column_names.size() == 2);
        assert(result.rows.size() == 3);
        assert(SameValue(result.rows[0][0], Text("Alice")));
        assert(SameValue(result.rows[0][1], Text("Engineering")));
        assert(SameValue(result.rows[1][0], Text("Bob")));
        assert(SameValue(result.rows[1][1], Text("Sales")));
        assert(SameValue(result.rows[2][0], Text("Dan")));

        std::cout << "[35] LEFT JOIN keeps unmatched left rows as NULL\n";
        assert(executor.Execute(
            "SELECT employees.emp_name, departments.dept_name "
            "FROM employees LEFT JOIN departments "
            "ON employees.dept_id = departments.dept_id;",
            nullptr, &error, &result));
        assert(result.rows.size() == 4);
        assert(SameValue(result.rows[2][0], Text("Cara")));
        assert(result.rows[2][1].type == sql::Value::Type::NULL_VALUE);

        std::cout << "[36] RIGHT JOIN keeps unmatched right rows as NULL\n";
        assert(executor.Execute(
            "SELECT employees.emp_name, departments.dept_name "
            "FROM employees RIGHT JOIN departments "
            "ON employees.dept_id = departments.dept_id;",
            nullptr, &error, &result));
        assert(result.rows.size() == 4);
        assert(result.rows.back()[0].type == sql::Value::Type::NULL_VALUE);
        assert(SameValue(result.rows.back()[1], Text("HR")));

        std::cout << "[37] FULL OUTER JOIN keeps unmatched rows on both sides\n";
        assert(executor.Execute(
            "SELECT employees.emp_name, departments.dept_name "
            "FROM employees FULL OUTER JOIN departments "
            "ON employees.dept_id = departments.dept_id;",
            nullptr, &error, &result));
        assert(result.rows.size() == 5);
        bool found_left_only = false;
        bool found_right_only = false;
        for (const auto& row : result.rows) {
            if (SameValue(row[0], Text("Cara")) &&
                row[1].type == sql::Value::Type::NULL_VALUE) found_left_only = true;
            if (row[0].type == sql::Value::Type::NULL_VALUE &&
                SameValue(row[1], Text("HR"))) found_right_only = true;
        }
        assert(found_left_only && found_right_only);

        std::cout << "[38] CROSS JOIN returns the Cartesian product\n";
        assert(executor.Execute(
            "SELECT employees.emp_id, departments.dept_id "
            "FROM employees CROSS JOIN departments;",
            nullptr, &error, &result));
        assert(result.rows.size() == 12);

        std::cout << "[39] JOIN accepts reversed ON column order\n";
        assert(executor.Execute(
            "SELECT employees.emp_name FROM employees JOIN departments "
            "ON departments.dept_id = employees.dept_id;",
            nullptr, &error, &result));
        assert(result.rows.size() == 3);

        std::cout << "[40] WHERE can select NULLs produced by an outer JOIN\n";
        assert(executor.Execute(
            "SELECT employees.emp_name FROM employees LEFT JOIN departments "
            "ON employees.dept_id = departments.dept_id "
            "WHERE departments.dept_name IS NULL;",
            nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Text("Cara")));

        std::cout << "[41] Ambiguous unqualified JOIN columns are rejected\n";
        assert(!executor.Execute(
            "SELECT dept_id FROM employees JOIN departments "
            "ON employees.dept_id = departments.dept_id;",
            nullptr, &error, &result));
        assert(!error.empty());

        std::cout << "[42] JOIN ON must connect the accumulated side to the new table\n";
        assert(!executor.Execute(
            "SELECT employees.emp_name FROM employees JOIN departments "
            "ON employees.emp_id = employees.dept_id;",
            nullptr, &error, &result));
        assert(!error.empty());

        std::cout << "[43] Chained JOIN can reference an earlier joined table\n";
        assert(executor.Execute(
            "CREATE TABLE dept_regions (dept_id INT, region TEXT);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO dept_regions VALUES (10, 'North');", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO dept_regions VALUES (20, 'South');", nullptr, &error));
        assert(executor.Execute(
            "SELECT employees.emp_name, dept_regions.region "
            "FROM employees "
            "JOIN departments ON employees.dept_id = departments.dept_id "
            "JOIN dept_regions ON departments.dept_id = dept_regions.dept_id;",
            nullptr, &error, &result));
        assert(result.rows.size() == 3);
        assert(SameValue(result.rows[0][1], Text("North")));
        assert(SameValue(result.rows[1][1], Text("South")));
        assert(SameValue(result.rows[2][1], Text("North")));

        std::cout << "[44] JOIN can resolve a key near the end of a wider right table\n";
        assert(executor.Execute(
            "CREATE TABLE one_column (id INT);", nullptr, &error));
        assert(executor.Execute(
            "CREATE TABLE wide_table (first_value INT, second_value INT, id INT);",
            nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO one_column VALUES (5);", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO wide_table VALUES (100, 200, 5);", nullptr, &error));
        assert(executor.Execute(
            "SELECT one_column.id FROM one_column JOIN wide_table "
            "ON one_column.id = wide_table.id;",
            nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Int(5)));

        std::cout << "[44a] Table aliases resolve across SELECT, JOIN, WHERE and ORDER BY\n";
        assert(executor.Execute(
            "SELECT e.emp_name, d.dept_name FROM employees AS e "
            "JOIN departments d ON e.dept_id = d.dept_id "
            "WHERE d.dept_name = 'Engineering' ORDER BY e.emp_name;",
            nullptr, &error, &result));
        assert(result.rows.size() == 2);
        assert(SameValue(result.rows[0][0], Text("Alice")));
        assert(SameValue(result.rows[1][0], Text("Dan")));

        std::cout << "[44a-1] Aliases also resolve in GROUP BY and HAVING\n";
        assert(executor.Execute(
            "SELECT d.dept_name, COUNT(*) FROM employees e "
            "JOIN departments d ON e.dept_id = d.dept_id "
            "GROUP BY d.dept_name HAVING COUNT(*) > 1 ORDER BY d.dept_name;",
            nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Text("Engineering")));
        assert(SameValue(result.rows[0][1], Int(2)));

        std::cout << "[44b] An alias hides the original table qualifier\n";
        assert(!executor.Execute(
            "SELECT employees.emp_name FROM employees e;",
            nullptr, &error, &result));
        assert(!error.empty());

        std::cout << "[44c] Duplicate aliases are rejected\n";
        assert(!executor.Execute(
            "SELECT e.emp_name FROM employees e JOIN departments e "
            "ON e.dept_id = e.dept_id;",
            nullptr, &error, &result));
        assert(!error.empty());

        std::cout << "[44d] Self-join works with distinct aliases\n";
        assert(executor.Execute(
            "SELECT worker.emp_name, manager.emp_name FROM employees worker "
            "JOIN employees manager ON worker.emp_id = manager.emp_id;",
            nullptr, &error, &result));
        assert(result.rows.size() == 4);
        assert(SameValue(result.rows[0][0], Text("Alice")));
        assert(SameValue(result.rows[0][1], Text("Alice")));

        std::cout << "[44e] Unknown alias qualifiers are rejected\n";
        assert(!executor.Execute(
            "SELECT x.emp_name FROM employees e;",
            nullptr, &error, &result));
        assert(!error.empty());

        std::cout << "[45] ORDER BY sorts integer and text columns" << std::endl;
        assert(executor.Execute(
            "CREATE TABLE task4_sort (id INT, name TEXT, score INT);", nullptr, &error));
        assert(executor.Execute("INSERT INTO task4_sort VALUES (1, 'Zoe', 20);", nullptr, &error));
        assert(executor.Execute("INSERT INTO task4_sort VALUES (2, 'Amy', 30);", nullptr, &error));
        assert(executor.Execute("INSERT INTO task4_sort VALUES (3, 'Bob', 20);", nullptr, &error));
        assert(executor.Execute("SELECT name FROM task4_sort ORDER BY score ASC, name DESC;",
                               nullptr, &error, &result));
        assert(result.rows.size() == 3);
        assert(SameValue(result.rows[0][0], Text("Zoe")));
        assert(SameValue(result.rows[1][0], Text("Bob")));
        assert(SameValue(result.rows[2][0], Text("Amy")));

        std::cout << "[46] ORDER BY supports qualified JOIN columns" << std::endl;
        assert(executor.Execute(
            "SELECT employees.emp_name FROM employees JOIN departments "
            "ON employees.dept_id = departments.dept_id "
            "ORDER BY departments.dept_name DESC;", nullptr, &error, &result));
        assert(result.rows.size() == 3);
        assert(SameValue(result.rows[0][0], Text("Bob")));

        std::cout << "[47] LIMIT and OFFSET paginate after sorting" << std::endl;
        assert(executor.Execute(
            "SELECT name FROM task4_sort ORDER BY score ASC, name ASC LIMIT 1 OFFSET 1;",
            nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Text("Zoe")));
        assert(executor.Execute(
            "SELECT name FROM task4_sort OFFSET 2;", nullptr, &error, &result));
        assert(result.rows.size() == 1);

        std::cout << "[48] DISTINCT removes duplicate projected rows" << std::endl;
        assert(executor.Execute(
            "SELECT DISTINCT score FROM task4_sort ORDER BY score DESC;",
            nullptr, &error, &result));
        assert(result.rows.size() == 2);
        assert(SameValue(result.rows[0][0], Int(30)));
        assert(SameValue(result.rows[1][0], Int(20)));
        assert(executor.Execute(
            "SELECT DISTINCT name, score FROM task4_sort;", nullptr, &error, &result));
        assert(result.rows.size() == 3);

        std::cout << "[49] GROUP BY produces one row per distinct key" << std::endl;
        assert(executor.Execute(
            "CREATE TABLE task5_groups (id INT, department TEXT, region TEXT);",
            nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO task5_groups VALUES (1, 'Engineering', 'North');", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO task5_groups VALUES (2, 'Engineering', 'North');", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO task5_groups VALUES (3, 'Sales', 'South');", nullptr, &error));
        assert(executor.Execute(
            "INSERT INTO task5_groups VALUES (4, 'Sales', 'West');", nullptr, &error));
        assert(executor.Execute(
            "SELECT department FROM task5_groups GROUP BY department ORDER BY department;",
            nullptr, &error, &result));
        assert(result.rows.size() == 2);
        assert(SameValue(result.rows[0][0], Text("Engineering")));
        assert(SameValue(result.rows[1][0], Text("Sales")));

        std::cout << "[50] GROUP BY supports composite grouping keys" << std::endl;
        assert(executor.Execute(
            "SELECT department, region FROM task5_groups "
            "GROUP BY department, region ORDER BY department, region;",
            nullptr, &error, &result));
        assert(result.rows.size() == 3);

        std::cout << "[51] HAVING filters grouped rows" << std::endl;
        assert(executor.Execute(
            "SELECT department FROM task5_groups GROUP BY department "
            "HAVING department = 'Sales';", nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Text("Sales")));

        std::cout << "[52] GROUP BY rejects non-grouped projected columns" << std::endl;
        assert(!executor.Execute(
            "SELECT id, department FROM task5_groups GROUP BY department;",
            nullptr, &error, &result));
        assert(!error.empty());

        std::cout << "[53] HAVING rejects non-grouped columns" << std::endl;
        assert(!executor.Execute(
            "SELECT department FROM task5_groups GROUP BY department "
            "HAVING id > 1;", nullptr, &error, &result));
        assert(!error.empty());

        std::cout << "[54] HAVING requires GROUP BY" << std::endl;
        assert(!executor.Execute(
            "SELECT department FROM task5_groups HAVING department = 'Sales';",
            nullptr, &error, &result));
        assert(!error.empty());

        std::cout << "[55] COUNT, SUM, AVG, MIN and MAX without GROUP BY" << std::endl;
        assert(executor.Execute(
            "SELECT COUNT(*), COUNT(id), SUM(id), AVG(id), MIN(id), MAX(id) "
            "FROM task5_groups;", nullptr, &error, &result));
        assert(result.rows.size() == 1 && result.rows[0].size() == 6);
        assert(SameValue(result.rows[0][0], Int(4)));
        assert(SameValue(result.rows[0][1], Int(4)));
        assert(SameValue(result.rows[0][2], Int(10)));
        assert(SameValue(result.rows[0][3], Int(2)));
        assert(SameValue(result.rows[0][4], Int(1)));
        assert(SameValue(result.rows[0][5], Int(4)));

        std::cout << "[56] Multiple aggregates work with GROUP BY" << std::endl;
        assert(executor.Execute(
            "SELECT department, COUNT(*), SUM(id), AVG(id) FROM task5_groups "
            "GROUP BY department ORDER BY department;", nullptr, &error, &result));
        assert(result.rows.size() == 2);
        assert(SameValue(result.rows[0][0], Text("Engineering")));
        assert(SameValue(result.rows[0][1], Int(2)));
        assert(SameValue(result.rows[0][2], Int(3)));
        assert(SameValue(result.rows[0][3], Int(1)));
        assert(SameValue(result.rows[1][1], Int(2)));
        assert(SameValue(result.rows[1][2], Int(7)));

        std::cout << "[57] Aggregate HAVING filters groups" << std::endl;
        assert(executor.Execute(
            "SELECT department, COUNT(*) FROM task5_groups GROUP BY department "
            "HAVING COUNT(*) > 1 ORDER BY department;", nullptr, &error, &result));
        assert(result.rows.size() == 2);
        assert(SameValue(result.rows[0][1], Int(2)));

        std::cout << "[57a] HAVING can filter with SUM as well as COUNT" << std::endl;
        assert(executor.Execute(
            "SELECT department, SUM(id) FROM task5_groups GROUP BY department "
            "HAVING SUM(id) > 5;", nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Text("Sales")));
        assert(SameValue(result.rows[0][1], Int(7)));

        std::cout << "[58] MIN and MAX support TEXT values" << std::endl;
        assert(executor.Execute(
            "SELECT MIN(department), MAX(department) FROM task5_groups;",
            nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Text("Engineering")));
        assert(SameValue(result.rows[0][1], Text("Sales")));

        std::cout << "[59] COUNT(column) ignores NULLs from an outer join" << std::endl;
        assert(executor.Execute(
            "SELECT COUNT(*), COUNT(departments.dept_name) FROM employees "
            "LEFT JOIN departments ON employees.dept_id = departments.dept_id;",
            nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Int(4)));
        assert(SameValue(result.rows[0][1], Int(3)));

        std::cout << "[60] Empty input returns COUNT 0 and NULL for other aggregates" << std::endl;
        assert(executor.Execute("CREATE TABLE task6_empty (id INT);", nullptr, &error));
        assert(executor.Execute(
            "SELECT COUNT(*), SUM(id), AVG(id), MIN(id), MAX(id) FROM task6_empty;",
            nullptr, &error, &result));
        assert(result.rows.size() == 1);
        assert(SameValue(result.rows[0][0], Int(0)));
        for (std::size_t i = 1; i < result.rows[0].size(); ++i)
            assert(result.rows[0][i].type == sql::Value::Type::NULL_VALUE);

        std::cout << "[61] Aggregate validation rejects invalid arguments and mixed projections" << std::endl;
        assert(!executor.Execute("SELECT SUM(emp_name) FROM employees;", nullptr, &error, &result));
        assert(!error.empty());
        assert(!executor.Execute("SELECT emp_name, COUNT(*) FROM employees;", nullptr, &error, &result));
        assert(!error.empty());
        assert(!executor.Execute("SELECT COUNT(*) FROM employees WHERE COUNT(*) > 1;", nullptr, &error, &result));
        assert(!error.empty());

        bpm.FlushAllPages();
    }

    std::remove(db_file.c_str());
    std::cout << "\nAll SQL executor tests passed.\n";
    return 0;
}
