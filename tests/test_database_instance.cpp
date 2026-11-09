
#include "storage/database_instance.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
    const auto unique_id =
        std::chrono::steady_clock::now().time_since_epoch().count();

    const std::filesystem::path db_path =
        std::filesystem::temp_directory_path() /
        ("ocera_database_instance_test_" +
         std::to_string(unique_id) + ".db");

    try {
        {
            ocera::DatabaseInstance database(db_path.string(), 16);

            assert(database.GetDatabaseHeader().GetVersion() ==
                   ocera::DatabaseHeader::kCurrentVersion);

            assert(database.GetDatabaseHeader().GetPageSize() ==
                   ocera::PAGE_SIZE);

            database.Flush();
        }

        // Reopen the same file and verify its database header.
        {
            ocera::DatabaseInstance database(db_path.string(), 16);

            assert(database.GetDatabaseHeader().GetVersion() ==
                   ocera::DatabaseHeader::kCurrentVersion);

            assert(database.GetDatabaseHeader().GetPageSize() ==
                   ocera::PAGE_SIZE);

            database.Flush();
        }

        std::filesystem::remove(db_path);

        std::cout << "Database instance lifecycle test passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::filesystem::remove(db_path);
        std::cerr << "Database instance lifecycle test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
