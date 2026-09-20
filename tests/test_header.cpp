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

using namespace minidb;

namespace {

// Must match DatabaseHeader's on-disk layout (see database_header.cpp).
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kPageSizeOffset = 8;

// Overwrites 4 bytes at `offset` within page HEADER_PAGE_ID, to simulate a
// corrupted/incompatible header for the rejection tests below.
void CorruptPage1U32(const std::string& db_file, std::size_t offset, std::uint32_t bad_value) {
    std::fstream f(db_file, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(HEADER_PAGE_ID) * PAGE_SIZE + static_cast<std::streamoff>(offset));
    char bytes[4];
    bytes[0] = static_cast<char>(bad_value & 0xFFu);
    bytes[1] = static_cast<char>((bad_value >> 8) & 0xFFu);
    bytes[2] = static_cast<char>((bad_value >> 16) & 0xFFu);
    bytes[3] = static_cast<char>((bad_value >> 24) & 0xFFu);
    f.write(bytes, 4);
}

}  // namespace

int main() {
    std::cout << "=== MiniDB Database Header Test ===\n\n";
    const std::string db_file = "test_header.db";
    std::remove(db_file.c_str());

    // --- A: create a new database, verify the header's default contents ---
    std::cout << "[A] Creating a new database and checking the header...\n";
    {
        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        DatabaseHeader header(disk_manager);

        assert(header.GetVersion() == DatabaseHeader::kCurrentVersion);
        assert(header.GetPageSize() == static_cast<std::uint32_t>(PAGE_SIZE));
        assert(header.GetCatalogRootPageId() == INVALID_PAGE_ID);
        std::cout << "    PASSED: version=" << header.GetVersion()
                   << ", page_size=" << header.GetPageSize()
                   << ", catalog_root=" << header.GetCatalogRootPageId() << "\n\n";

        // --- C: AllocatePage() must never return page 0 or page 1 ---
        std::cout << "[C] Verifying AllocatePage() skips reserved pages 0 and 1...\n";
        for (int i = 0; i < 5; ++i) {
            page_id_t p = free_space_manager.AllocatePage();
            assert(p != BITMAP_PAGE_ID);
            assert(p != HEADER_PAGE_ID);
        }
        std::cout << "    PASSED: 5 allocations, none returned page 0 or page 1.\n\n";

        // --- D: reserved pages cannot be deallocated ---
        std::cout << "[D] Verifying reserved pages cannot be freed...\n";
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
        std::cout << "    PASSED: freeing page 0 or page 1 is rejected.\n\n";
    }

    // --- B: reopen the same database, header must validate cleanly ---
    std::cout << "[B] Reopening the database and re-validating the header...\n";
    {
        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        DatabaseHeader header(disk_manager);
        (void)free_space_manager;

        assert(header.GetVersion() == DatabaseHeader::kCurrentVersion);
        assert(header.GetPageSize() == static_cast<std::uint32_t>(PAGE_SIZE));
        assert(header.GetCatalogRootPageId() == INVALID_PAGE_ID);
        std::cout << "    PASSED: reopened header matches what was originally written.\n\n";
    }

    // --- E: a garbage/non-MiniDB file is rejected cleanly ---
    std::cout << "[E] Opening a garbage (non-MiniDB) file...\n";
    {
        const std::string garbage_file = "test_header_garbage.db";
        std::remove(garbage_file.c_str());
        {
            std::ofstream garbage(garbage_file, std::ios::binary);
            std::vector<char> junk(3 * PAGE_SIZE, static_cast<char>(0xAB));
            garbage.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        }

        DiskManager disk_manager(garbage_file);
        FreeSpaceManager free_space_manager(disk_manager);
        (void)free_space_manager;
        bool threw = false;
        try {
            DatabaseHeader header(disk_manager);
        } catch (const std::runtime_error& e) {
            threw = true;
            std::cout << "    Correctly rejected: " << e.what() << "\n";
        }
        assert(threw);
        std::cout << "    PASSED: garbage file was rejected, not silently misread.\n\n";
    }

    // --- F: an unsupported format version is rejected ---
    std::cout << "[F] Opening a file with an unsupported format version...\n";
    {
        const std::string version_file = "test_header_badversion.db";
        std::remove(version_file.c_str());
        {
            DiskManager disk_manager(version_file);
            FreeSpaceManager free_space_manager(disk_manager);
            DatabaseHeader header(disk_manager);  // writes a valid header first
            (void)free_space_manager;
        }
        CorruptPage1U32(version_file, kVersionOffset, 99);  // pretend format version 99

        DiskManager disk_manager(version_file);
        FreeSpaceManager free_space_manager(disk_manager);
        (void)free_space_manager;
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

    // --- G: a mismatched page size is rejected ---
    std::cout << "[G] Opening a file with a mismatched page size...\n";
    {
        const std::string pagesize_file = "test_header_badpagesize.db";
        std::remove(pagesize_file.c_str());
        {
            DiskManager disk_manager(pagesize_file);
            FreeSpaceManager free_space_manager(disk_manager);
            DatabaseHeader header(disk_manager);
            (void)free_space_manager;
        }
        CorruptPage1U32(pagesize_file, kPageSizeOffset, 1024);  // pretend 1024-byte pages

        DiskManager disk_manager(pagesize_file);
        FreeSpaceManager free_space_manager(disk_manager);
        (void)free_space_manager;
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
