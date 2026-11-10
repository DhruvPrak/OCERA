
#include "storage/database_instance.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool ThrowsRuntimeError(
    const std::function<void()>& operation
) {
    try {
        operation();
    } catch (const std::runtime_error&) {
        return true;
    }

    return false;
}

ocera::sql::Value Int(std::int64_t value) {
    ocera::sql::Value result;
    result.type = ocera::sql::Value::Type::INTEGER;
    result.int_value = value;
    return result;
}

ocera::sql::Value Text(const std::string& value) {
    ocera::sql::Value result;
    result.type = ocera::sql::Value::Type::STRING;
    result.string_value = value;
    return result;
}

bool SameValue(
    const ocera::sql::Value& left,
    const ocera::sql::Value& right
) {
    if (left.type != right.type) {
        return false;
    }

    if (left.type == ocera::sql::Value::Type::INTEGER) {
        return left.int_value == right.int_value;
    }

    return left.string_value == right.string_value;
}

}  // namespace

int main() {
    const auto unique_id =
        std::chrono::steady_clock::now().time_since_epoch().count();

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path();

    const std::filesystem::path db_path =
        directory /
        ("ocera_database_instance_test_" +
         std::to_string(unique_id) + ".db");

    const std::filesystem::path missing_path =
        directory /
        ("ocera_missing_database_test_" +
         std::to_string(unique_id) + ".db");

    const std::filesystem::path empty_path =
        directory /
        ("ocera_empty_database_test_" +
         std::to_string(unique_id) + ".db");

    try {
        // 1. CREATE_NEW should create a valid database.
        {
            ocera::DatabaseInstance database(
                db_path.string(),
                16,
                ocera::DatabaseOpenMode::CREATE_NEW
            );

            Require(
                database.GetDatabaseHeader().GetVersion() ==
                    ocera::DatabaseHeader::kCurrentVersion,
                "New database has an incorrect format version"
            );

            Require(
                database.GetDatabaseHeader().GetPageSize() ==
                    ocera::PAGE_SIZE,
                "New database has an incorrect page size"
            );

            database.Flush();
        }

        // 2. Catalog metadata and SQL table rows must survive restart.
        {
            ocera::DatabaseInstance database(
                db_path.string(),
                16,
                ocera::DatabaseOpenMode::OPEN_EXISTING
            );

            // Preserve the existing metadata-only Catalog test.
            ocera::sql::CreateTableStatement metadata_table;
            metadata_table.table = "restart_students";
            metadata_table.columns.push_back(
                {"id", ocera::sql::ColumnType::INT, 0}
            );
            metadata_table.columns.push_back(
                {"name", ocera::sql::ColumnType::TEXT, 0}
            );

            database.GetCatalog().CreateTable(metadata_table);

            // Create a physical table through the SQL executor.
            std::string error;

            Require(
                database.GetExecutor().Execute(
                    "CREATE TABLE runtime_students (id INT, name TEXT);",
                    nullptr,
                    &error
                ),
                "CREATE TABLE failed: " + error
            );

            // Insert a row that must survive restart.
            Require(
                database.GetExecutor().Execute(
                    "INSERT INTO runtime_students VALUES (7, 'Asha');",
                    nullptr,
                    &error
                ),
                "INSERT failed: " + error
            );

            database.Flush();
        }

        // 3. Reopen the database and verify catalog metadata and SQL rows.
        {
            ocera::DatabaseInstance database(
                db_path.string(),
                16,
                ocera::DatabaseOpenMode::OPEN_EXISTING
            );

        std::string error;

        // Verify metadata-only catalog entry.
            const auto* metadata_table =
                database.GetCatalog().GetTable("restart_students");

            Require(
                metadata_table != nullptr,
                "Catalog table was not restored after restart"
            );

            Require(
                metadata_table->columns.size() == 2,
                "Restored Catalog schema has wrong column count"
            );

            Require(
                metadata_table->columns[0].name == "id" &&
                metadata_table->columns[1].name == "name",
                "Restored Catalog schema has incorrect column names"
            );

            // Verify physical SQL table metadata.
            const auto* runtime_table =
                database.GetCatalog().GetTable("runtime_students");

            Require(
                runtime_table != nullptr,
                "SQL-created table was not restored after restart"
            );

            Require(
                runtime_table->columns.size() == 2,
                "Restored SQL table has incorrect column count"
            );

            Require(
                runtime_table->columns[0].name == "id" &&
                runtime_table->columns[1].name == "name",
                "Restored SQL table has incorrect column names"
            );

            // Most important test:
            // SELECT must work after closing and reopening the database.
            ocera::QueryResult result;

            Require(
                database.GetExecutor().Execute(
                    "SELECT * FROM runtime_students;",
                    nullptr,
                    &error,
                    &result
                ),
                "SELECT after restart failed: " + error
            );

            Require(
                result.column_names.size() == 2,
                "SELECT returned an incorrect number of columns"
            );

            Require(
                result.column_names[0] == "id" &&
                result.column_names[1] == "name",
                "SELECT returned incorrect column names"
            );

            Require(
                result.rows.size() == 1,
                "Persisted row was not found after restart"
            );

            Require(
                result.rows[0].size() == 2,
                "Persisted row has an incorrect number of values"
            );

            Require(
                SameValue(result.rows[0][0], Int(7)),
                "Persisted ID value is incorrect"
            );

            Require(
                SameValue(result.rows[0][1], Text("Asha")),
                "Persisted name value is incorrect"
            );

            database.Flush();
        }

        // 4. CREATE_NEW must reject an existing database.
        Require(
            ThrowsRuntimeError([&]() {
                ocera::DatabaseInstance database(
                    db_path.string(),
                    16,
                    ocera::DatabaseOpenMode::CREATE_NEW
                );
            }),
            "CREATE_NEW unexpectedly accepted an existing database"
        );

        // 5. OPEN_EXISTING should reopen the valid database.
        {
            ocera::DatabaseInstance database(
                db_path.string(),
                16,
                ocera::DatabaseOpenMode::OPEN_EXISTING
            );

            Require(
                database.GetDatabaseHeader().GetVersion() ==
                    ocera::DatabaseHeader::kCurrentVersion,
                "Reopened database has an incorrect format version"
            );

            Require(
                database.GetDatabaseHeader().GetPageSize() ==
                    ocera::PAGE_SIZE,
                "Reopened database has an incorrect page size"
            );

            database.Flush();
        }

        // 6. OPEN_EXISTING must reject a missing file.
        Require(
            ThrowsRuntimeError([&]() {
                ocera::DatabaseInstance database(
                    missing_path.string(),
                    16,
                    ocera::DatabaseOpenMode::OPEN_EXISTING
                );
            }),
            "OPEN_EXISTING unexpectedly accepted a missing database"
        );

        // 7. OPEN_EXISTING must reject an empty file.
        {
            std::ofstream empty_file(
                empty_path,
                std::ios::out | std::ios::binary
            );

            Require(
                empty_file.is_open(),
                "Could not create the empty test database file"
            );
        }

        Require(
            ThrowsRuntimeError([&]() {
                ocera::DatabaseInstance database(
                    empty_path.string(),
                    16,
                    ocera::DatabaseOpenMode::OPEN_EXISTING
                );
            }),
            "OPEN_EXISTING unexpectedly accepted an empty database"
        );

        // 8. Clean up test files.
        std::filesystem::remove(db_path);
        std::filesystem::remove(empty_path);

        std::cout
            << "Database instance lifecycle tests passed.\n";

        return 0;

    } catch (const std::exception& exception) {
        std::error_code error;

        std::filesystem::remove(db_path, error);
        std::filesystem::remove(missing_path, error);
        std::filesystem::remove(empty_path, error);

        std::cerr
            << "Database instance lifecycle tests failed: "
            << exception.what() << '\n';

        return 1;
    }
}
