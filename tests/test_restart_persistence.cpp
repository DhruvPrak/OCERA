
#include <cstdio>
#include <iostream>

#include "catalog/catalog.h"
#include "sql/executor.h"
#include "storage/buffer_pool_manager.h"
#include "storage/database_header.h"
#include "storage/disk_manager.h"
#include "storage/free_space_manager.h"

using namespace ocera;

#define CHECK(condition)                                      \
    do {                                                      \
        if (!(condition)) {                                   \
            std::cerr << "CHECK failed at line " << __LINE__  \
                      << ": " #condition << '\n';             \
            return 1;                                         \
        }                                                     \
    } while (false)

int main() {
    const std::string file = "test_restart_persistence.db";
    std::remove(file.c_str());

    std::string error;
    QueryResult result;

    // Session 1: Create tables and insert records.
    {
        DiskManager disk(file);
        FreeSpaceManager space(disk);
        DatabaseHeader header(disk);
        BufferPoolManager bpm(8, disk, space);
        Catalog catalog;
        SqlExecutor sql(catalog, bpm, &header);

        CHECK(sql.Execute(
            "CREATE TABLE students (id INT PRIMARY KEY, name TEXT);",
            nullptr, &error));

        CHECK(sql.Execute(
            "INSERT INTO students VALUES (1, 'Dhruv');",
            nullptr, &error));

        CHECK(sql.Execute(
            "INSERT INTO students VALUES (2, 'Ishika');",
            nullptr, &error));

        CHECK(sql.Execute(
            "CREATE TABLE courses (id INT PRIMARY KEY, student_id INT, "
            "FOREIGN KEY (student_id) REFERENCES students(id));",
            nullptr, &error));

        CHECK(sql.Execute(
            "INSERT INTO courses VALUES (10, 1);",
            nullptr, &error));

        CHECK(sql.Execute(
            "CREATE INDEX idx_courses_student_id ON courses(student_id);",
            nullptr, &error));

        CHECK(catalog.HasIndex("idx_courses_student_id"));

        bpm.FlushAllPages();
    }

    // Session 2: Reopen, read records, check constraints, and update.
    {
        DiskManager disk(file);
        FreeSpaceManager space(disk);
        DatabaseHeader header(disk);
        BufferPoolManager bpm(8, disk, space);
        Catalog catalog;
        SqlExecutor sql(catalog, bpm, &header);

        CHECK(catalog.HasTable("students"));
        CHECK(catalog.HasTable("courses"));

        // Index metadata must survive restart.
        CHECK(catalog.HasIndex("idx_courses_student_id"));

        const IndexSchema* index =
            catalog.GetIndex("idx_courses_student_id");

        CHECK(index != nullptr);
        CHECK(index->table_name == "courses");
        CHECK(index->column_name == "student_id");

        CHECK(sql.Execute(
            "SELECT * FROM students;",
            nullptr, &error, &result));

        CHECK(result.rows.size() == 2);

        CHECK(!sql.Execute(
            "INSERT INTO courses VALUES (11, 999);",
            nullptr, &error));

        CHECK(sql.Execute(
            "UPDATE students SET name = 'Dhruv Updated' WHERE id = 1;",
            nullptr, &error));

        bpm.FlushAllPages();
    }

    // Session 3: Reopen again and verify the update was saved.
    {
        DiskManager disk(file);
        FreeSpaceManager space(disk);
        DatabaseHeader header(disk);
        BufferPoolManager bpm(8, disk, space);
        Catalog catalog;
        SqlExecutor sql(catalog, bpm, &header);

        CHECK(catalog.HasIndex("idx_courses_student_id"));

        CHECK(sql.Execute(
            "SELECT * FROM students WHERE id = 1;",
            nullptr, &error, &result));

        CHECK(result.rows.size() == 1);
        CHECK(result.rows[0][1].string_value == "Dhruv Updated");

        CHECK(sql.Execute(
            "SELECT * FROM students WHERE id = 2;",
            nullptr, &error, &result));

        CHECK(result.rows.size() == 1);
        CHECK(result.rows[0][1].string_value == "Ishika");

        bpm.FlushAllPages();
    }

    std::remove(file.c_str());

    std::cout << "Restart persistence test passed.\n";
    return 0;
}
