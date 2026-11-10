
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "../src/common/config.h"
#include "../src/storage/database_header.h"
#include "../src/storage/disk_manager.h"
#include "../src/storage/free_space_manager.h"

using namespace ocera;

namespace {

// Must match DatabaseHeader's on-disk layout.
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kPageSizeOffset = 8;

// Overwrites 4 bytes in the database header to simulate corruption.
void CorruptPage1U32(
    const std::string& db_file,
    std::size_t offset,
    std::uint32_t bad_value
) {
    std::fstream f(
        db_file,
        std::ios::in | std::ios::out | std::ios::binary
    );

    if (!f) {
        throw std::runtime_error("Could not open database file for corruption");
    }

    f.seekp(
        static_cast<std::streamoff>(HEADER_PAGE_ID) * PAGE_SIZE +
        static_cast<std::streamoff>(offset)
    );

    char bytes[4];

    bytes[0] = static_cast<char>(bad_value & 0xFFu);
    bytes[1] = static_cast<char>((bad_value >> 8) & 0xFFu);
    bytes[2] = static_cast<char>((bad_value >> 16) & 0xFFu);
    bytes[3] = static_cast<char>((bad_value >> 24) & 0xFFu);

    f.write(bytes, 4);

    if (!f) {
        throw std::runtime_error("Could not corrupt database header");
    }
}

} // namespace

int main() {
    std::cout << "=== OCERA Database Header Test ===\n\n";

    const std::string db_file = "test_header.db";
    std::remove(db_file.c_str());

    page_id_t saved_catalog_page = INVALID_PAGE_ID;

    // A: Create a new database and check default header values.
    std::cout << "[A] Creating a new database...\n";

    {
        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        DatabaseHeader header(disk_manager);

        assert(header.GetVersion() == DatabaseHeader::kCurrentVersion);
        assert(
            header.GetPageSize() ==
            static_cast<std::uint32_t>(PAGE_SIZE)
        );
        assert(header.GetCatalogRootPageId() == INVALID_PAGE_ID);

        std::cout << "    PASSED: default header values are correct.\n";

        // Allocate a page to use as the Catalog root.
        saved_catalog_page = free_space_manager.AllocatePage();

        assert(saved_catalog_page >= 2);

        // Save the Catalog root page ID in the database header.
        header.SetCatalogRootPageId(saved_catalog_page);

        assert(header.GetCatalogRootPageId() == saved_catalog_page);

        std::cout << "    PASSED: Catalog root page ID was updated.\n\n";

        // C: Allocated pages must never be reserved pages 0 or 1.
        std::cout << "[C] Checking reserved page allocation...\n";

        for (int i = 0; i < 5; ++i) {
            page_id_t page_id = free_space_manager.AllocatePage();

            assert(page_id != BITMAP_PAGE_ID);
            assert(page_id != HEADER_PAGE_ID);
        }

        std::cout << "    PASSED: reserved pages were not allocated.\n\n";

        // D: Reserved pages cannot be deallocated.
        std::cout << "[D] Checking reserved page deallocation...\n";

        bool threw_for_bitmap = false;

        try {
            free_space_manager.DeallocatePage(BITMAP_PAGE_ID);
        } catch (const std::invalid_argument&) {
            threw_for_bitmap = true;
        }

        assert(threw_for_bitmap);

        bool threw_for_header = false;

        try {
            free_space_manager.DeallocatePage(HEADER_PAGE_ID);
        } catch (const std::invalid_argument&) {
            threw_for_header = true;
        }

        assert(threw_for_header);

        std::cout << "    PASSED: reserved pages cannot be freed.\n\n";
    }

    // B: Reopen the database and verify the saved Catalog root.
    std::cout << "[B] Reopening the database...\n";

    {
        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        DatabaseHeader header(disk_manager);

        assert(header.GetVersion() == DatabaseHeader::kCurrentVersion);

        assert(
            header.GetPageSize() ==
            static_cast<std::uint32_t>(PAGE_SIZE)
        );

        assert(header.GetCatalogRootPageId() == saved_catalog_page);

        std::cout << "    PASSED: Catalog root page ID survived reopening.\n";
        std::cout << "    PASSED: database header reopened successfully.\n\n";
    }

    // E: A garbage/non-OCERA file must be rejected.
    std::cout << "[E] Testing invalid database file rejection...\n";

    {
        const std::string garbage_file = "test_header_garbage.db";
        std::remove(garbage_file.c_str());

        {
            std::ofstream garbage(garbage_file, std::ios::binary);

            std::vector<char> junk(
                3 * PAGE_SIZE,
                static_cast<char>(0xAB)
            );

            garbage.write(
                junk.data(),
                static_cast<std::streamsize>(junk.size())
            );
        }

        DiskManager disk_manager(garbage_file);
        FreeSpaceManager free_space_manager(disk_manager);

        bool threw = false;

        try {
            DatabaseHeader header(disk_manager);
        } catch (const std::runtime_error& e) {
            threw = true;
            std::cout << "    Correctly rejected: " << e.what() << "\n";
        }

        assert(threw);

        std::cout << "    PASSED: invalid database file was rejected.\n\n";
    }

    // F: An unsupported database format version must be rejected.
    std::cout << "[F] Testing unsupported format version...\n";

    {
        const std::string version_file = "test_header_badversion.db";
        std::remove(version_file.c_str());

        {
            DiskManager disk_manager(version_file);
            FreeSpaceManager free_space_manager(disk_manager);
            DatabaseHeader header(disk_manager);
        }

        CorruptPage1U32(version_file, kVersionOffset, 99);

        DiskManager disk_manager(version_file);
        FreeSpaceManager free_space_manager(disk_manager);

        bool threw = false;

        try {
            DatabaseHeader header(disk_manager);
        } catch (const std::runtime_error& e) {
            threw = true;
            std::cout << "    Correctly rejected: " << e.what() << "\n";
        }

        assert(threw);

        std::cout << "    PASSED: unsupported version was rejected.\n\n";
    }

    // G: A mismatched page size must be rejected.
    std::cout << "[G] Testing mismatched page size...\n";

    {
        const std::string pagesize_file = "test_header_badpagesize.db";
        std::remove(pagesize_file.c_str());

        {
            DiskManager disk_manager(pagesize_file);
            FreeSpaceManager free_space_manager(disk_manager);
            DatabaseHeader header(disk_manager);
        }

        CorruptPage1U32(pagesize_file, kPageSizeOffset, 1024);

        DiskManager disk_manager(pagesize_file);
        FreeSpaceManager free_space_manager(disk_manager);

        bool threw = false;

        try {
            DatabaseHeader header(disk_manager);
        } catch (const std::runtime_error& e) {
            threw = true;
            std::cout << "    Correctly rejected: " << e.what() << "\n";
        }

        assert(threw);

        std::cout << "    PASSED: mismatched page size was rejected.\n\n";
    }

    std::cout << "All database header tests passed.\n";

    return 0;
}