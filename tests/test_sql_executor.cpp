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

        std::cout << "[6] Reject malformed SQL and unsupported statements\n";
        assert(!executor.Execute(
            "INSERT INTO students VALUES (1, 'unterminated, 10);",
            &rejected_rid,
            &error
        ));
        assert(!error.empty());

        assert(!executor.Execute(
            "SELECT * FROM students;",
            nullptr,
            &error
        ));
        assert(error.find("only CREATE TABLE and INSERT") != std::string::npos);

        std::cout << "[7] Previously inserted rows remain unchanged\n";
        CheckRow(executor, rid, {Int(7), Text("Asha"), Int(20)});
        CheckRow(executor, second_rid, {Int(8), Text("Riya"), Int(21)});

        bpm.FlushAllPages();
    }

    std::remove(db_file.c_str());
    std::cout << "\nAll SQL executor tests passed.\n";
    return 0;
}
