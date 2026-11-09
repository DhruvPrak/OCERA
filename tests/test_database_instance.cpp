
#include "storage/database_instance.h"

#include <chrono>
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

        // 2. CREATE_NEW must reject an existing database.
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

        // 3. OPEN_EXISTING should reopen the valid database.
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

        // 4. OPEN_EXISTING must reject a missing file.
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

        // 5. OPEN_EXISTING must reject an empty file.
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

        // Clean up test files.
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
