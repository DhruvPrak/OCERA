
#include "storage/database_instance.h"

#include <array>
#include <chrono>
#include <cstring>
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

void WriteU32LE(char* data, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        data[offset + i] = static_cast<char>((value >> (8U * i)) & 0xFFU);
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

    const std::filesystem::path wal_path = db_path.string() + ".wal";
    const std::filesystem::path legacy_path = directory / ("ocera_legacy_database_test_" + std::to_string(unique_id) + ".db");
    const std::filesystem::path legacy_wal_path = legacy_path.string() + ".wal";

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

            Require(database.GetLogManager().GetDatabaseId() == database.GetDatabaseHeader().GetDatabaseId(),
                    "Database and WAL identities do not match");

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

        // 6. Upgrade a v1 database in place and preserve user pages.
        constexpr std::uint32_t kLegacyVersion = 1;
        std::array<char, ocera::PAGE_SIZE> legacy_header{};
        std::array<char, ocera::PAGE_SIZE> preserved_page{};
        std::memcpy(legacy_header.data(), "MDB1", 4);
        WriteU32LE(legacy_header.data(), 4, kLegacyVersion);
        WriteU32LE(legacy_header.data(), 8, static_cast<std::uint32_t>(ocera::PAGE_SIZE));
        WriteU32LE(legacy_header.data(), 12, static_cast<std::uint32_t>(ocera::INVALID_PAGE_ID));
        std::memcpy(preserved_page.data(), "legacy-page-payload", 19);
        {
            ocera::DiskManager legacy_disk(legacy_path.string(), ocera::DatabaseOpenMode::CREATE_NEW);
            ocera::FreeSpaceManager legacy_fsm(legacy_disk);
            legacy_disk.WritePage(ocera::HEADER_PAGE_ID, legacy_header.data());
            const auto user_page = legacy_fsm.AllocatePage();
            Require(user_page == 2, "Legacy test allocated an unexpected first user page");
            legacy_disk.WritePage(user_page, preserved_page.data());
            legacy_disk.Sync();
        }
        {
            ocera::DatabaseInstance upgraded(legacy_path.string(), 16, ocera::DatabaseOpenMode::OPEN_EXISTING);
            Require(upgraded.GetDatabaseHeader().GetVersion() == ocera::DatabaseHeader::kCurrentVersion,
                    "Legacy database was not upgraded to the current format");
            Require(upgraded.GetLogManager().GetDatabaseId() == upgraded.GetDatabaseHeader().GetDatabaseId(),
                    "Migrated database and WAL identities do not match");
            std::array<char, ocera::PAGE_SIZE> actual{};
            upgraded.GetDiskManager().ReadPage(2, actual.data());
            Require(actual == preserved_page, "Database migration modified an existing user page");
        }

        // Clean up test files.
        std::filesystem::remove(db_path);
        std::filesystem::remove(wal_path);
        std::filesystem::remove(empty_path);
        std::filesystem::remove(legacy_path);
        std::filesystem::remove(legacy_wal_path);
        std::filesystem::remove(legacy_path.string() + ".upgrade");
        std::filesystem::remove(legacy_path.string() + ".upgrade.tmp");
        std::filesystem::remove(legacy_path.string() + ".upgrade.done");

        std::cout
            << "Database instance lifecycle tests passed.\n";

        return 0;
    } catch (const std::exception& exception) {
        std::error_code error;

        std::filesystem::remove(db_path, error);
        std::filesystem::remove(wal_path, error);
        std::filesystem::remove(legacy_path, error);
        std::filesystem::remove(legacy_wal_path, error);
        std::filesystem::remove(legacy_path.string() + ".upgrade", error);
        std::filesystem::remove(legacy_path.string() + ".upgrade.tmp", error);
        std::filesystem::remove(legacy_path.string() + ".upgrade.done", error);
        std::filesystem::remove(missing_path, error);
        std::filesystem::remove(empty_path, error);

        std::cerr
            << "Database instance lifecycle tests failed: "
            << exception.what() << '\n';

        return 1;
    }
}
