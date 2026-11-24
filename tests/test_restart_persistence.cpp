
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/catalog_storage.h"
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


static void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
    }
}

static void AppendString(std::vector<std::uint8_t>& bytes, const std::string& value) {
    AppendU32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

// Format written by the original Catalog::Serialize() implementation:
// CTA1 signature, image version 2, then 32-bit enum values.
static std::vector<std::uint8_t> MakeLegacyCatalog(std::uint32_t version) {
    std::vector<std::uint8_t> bytes;
    AppendU32(bytes, 0x31415443U);  // "CTA1"
    AppendU32(bytes, version);     // legacy image version
    AppendU32(bytes, 1);            // table count
    AppendString(bytes, "legacy_students");
    if (version >= 2) {
        AppendU32(bytes, static_cast<std::uint32_t>(INVALID_PAGE_ID));
    }
    AppendU32(bytes, 2);            // column count
    AppendString(bytes, "id");
    AppendU32(bytes, static_cast<std::uint32_t>(sql::ColumnType::INT));
    AppendString(bytes, "name");
    AppendU32(bytes, static_cast<std::uint32_t>(sql::ColumnType::TEXT));
    AppendU32(bytes, 0);            // key constraints
    AppendU32(bytes, 0);            // foreign keys
    AppendU32(bytes, 0);            // indexes
    return bytes;
}

static void WriteU32(char* destination, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        destination[i] = static_cast<char>((value >> (8U * i)) & 0xffU);
    }
}

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

        // The runtime B+Tree must be rebuilt and usable after reopening.
        CHECK(sql.Execute(
            "SELECT id FROM courses WHERE student_id = 1;",
            nullptr, &error, &result));
        CHECK(result.rows.size() == 1);

        // DML after restart must keep the rebuilt index synchronized.
        CHECK(sql.Execute(
            "INSERT INTO courses VALUES (11, 2);",
            nullptr, &error));
        CHECK(sql.Execute(
            "UPDATE courses SET student_id = 1 WHERE id = 11;",
            nullptr, &error));
        CHECK(sql.Execute(
            "SELECT id FROM courses WHERE student_id = 1;",
            nullptr, &error, &result));
        CHECK(result.rows.size() == 2);
        CHECK(sql.Execute(
            "DELETE FROM courses WHERE id = 10;",
            nullptr, &error));

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
            "SELECT id FROM courses WHERE student_id = 1;",
            nullptr, &error, &result));
        CHECK(result.rows.size() == 1);
        CHECK(result.rows[0][0].int_value == 11);

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

    // Legacy catalog compatibility: the original implementation used an
    // 8-byte page header and a CTA1/v2 serialized image. This fixture also
    // represents a schema-only legacy table with no persisted first page.
    const std::string legacy_file = "test_legacy_catalog_persistence.db";
    std::remove(legacy_file.c_str());
    page_id_t legacy_root = INVALID_PAGE_ID;
    {
        DiskManager disk(legacy_file);
        FreeSpaceManager space(disk);
        DatabaseHeader header(disk);
        BufferPoolManager bpm(8, disk, space);

        Page* page = bpm.NewPage(&legacy_root);
        CHECK(page != nullptr);
        const auto image = MakeLegacyCatalog(2);
        CHECK(image.size() <= PAGE_SIZE - 8);

        std::memset(page->data, 0, PAGE_SIZE);
        WriteU32(page->data, static_cast<std::uint32_t>(INVALID_PAGE_ID));
        WriteU32(page->data + 4, static_cast<std::uint32_t>(image.size()));
        std::memcpy(page->data + 8, image.data(), image.size());
        CHECK(bpm.UnpinPage(legacy_root, true));
        header.SetCatalogRootPageId(legacy_root);
        bpm.FlushAllPages();
    }

    // The legacy schema must load without inventing a physical table page.
    // Saving it upgrades the catalog page chain to the current CAT1 format.
    {
        DiskManager disk(legacy_file);
        FreeSpaceManager space(disk);
        DatabaseHeader header(disk);
        BufferPoolManager bpm(8, disk, space);
        Catalog catalog;
        SqlExecutor sql(catalog, bpm, &header);

        CHECK(catalog.HasTable("legacy_students"));
        const TableSchema* table = catalog.GetTable("legacy_students");
        CHECK(table != nullptr);
        CHECK(table != nullptr && table->first_page_id == INVALID_PAGE_ID);
        CHECK(header.GetCatalogRootPageId() == legacy_root);

        CHECK(CatalogStorage::Save(catalog, bpm, header, &error));
        CHECK(header.GetCatalogRootPageId() != legacy_root);
        bpm.FlushAllPages();
    }

    // The rewritten current-format catalog must also preserve the legacy
    // schema-only entry across another process-style reopen.
    {
        DiskManager disk(legacy_file);
        FreeSpaceManager space(disk);
        DatabaseHeader header(disk);
        BufferPoolManager bpm(8, disk, space);
        Catalog catalog;
        SqlExecutor sql(catalog, bpm, &header);

        CHECK(catalog.HasTable("legacy_students"));
        const TableSchema* table = catalog.GetTable("legacy_students");
        CHECK(table != nullptr && table->first_page_id == INVALID_PAGE_ID);
    }

    std::remove(legacy_file.c_str());

    // Version 1 of the original CTA1 image omitted first_page_id entirely.
    const std::string legacy_v1_file = "test_legacy_catalog_v1_persistence.db";
    std::remove(legacy_v1_file.c_str());
    {
        DiskManager disk(legacy_v1_file);
        FreeSpaceManager space(disk);
        DatabaseHeader header(disk);
        BufferPoolManager bpm(8, disk, space);

        page_id_t root = INVALID_PAGE_ID;
        Page* page = bpm.NewPage(&root);
        CHECK(page != nullptr);
        const auto image = MakeLegacyCatalog(1);
        std::memset(page->data, 0, PAGE_SIZE);
        WriteU32(page->data, static_cast<std::uint32_t>(INVALID_PAGE_ID));
        WriteU32(page->data + 4, static_cast<std::uint32_t>(image.size()));
        std::memcpy(page->data + 8, image.data(), image.size());
        CHECK(bpm.UnpinPage(root, true));
        header.SetCatalogRootPageId(root);
        bpm.FlushAllPages();
    }
    {
        DiskManager disk(legacy_v1_file);
        FreeSpaceManager space(disk);
        DatabaseHeader header(disk);
        BufferPoolManager bpm(8, disk, space);
        Catalog catalog;
        SqlExecutor sql(catalog, bpm, &header);
        CHECK(catalog.HasTable("legacy_students"));
        const TableSchema* table = catalog.GetTable("legacy_students");
        CHECK(table != nullptr && table->first_page_id == INVALID_PAGE_ID);
    }
    std::remove(legacy_v1_file.c_str());

    std::remove(file.c_str());

    std::cout << "Restart persistence test passed.\n";
    return 0;
}
