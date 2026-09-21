#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include "../src/storage/disk_manager.h"
#include "../src/storage/free_space_manager.h"

using namespace ocera;

int main() {
    std::cout << "=== OCERA Storage Layer Test ===\n\n";

    const std::string db_file = "test.db";
    std::remove(db_file.c_str());  // start from a clean file every run

    DiskManager disk_manager(db_file);
    FreeSpaceManager free_space_manager(disk_manager);

    std::cout << "[1] Allocating 3 pages...\n";
    page_id_t p1 = free_space_manager.AllocatePage();
    page_id_t p2 = free_space_manager.AllocatePage();
    page_id_t p3 = free_space_manager.AllocatePage();
    std::cout << "    Got page ids: " << p1 << ", " << p2 << ", " << p3 << "\n\n";

    std::cout << "[2] Writing data to page " << p1 << " and reading it back...\n";
    char write_buf[PAGE_SIZE];
    std::memset(write_buf, 0, PAGE_SIZE);
    std::strcpy(write_buf, "Hello from OCERA's storage engine!");
    disk_manager.WritePage(p1, write_buf);

    char read_buf[PAGE_SIZE];
    disk_manager.ReadPage(p1, read_buf);
    std::cout << "    Read back: \"" << read_buf << "\"\n";
    assert(std::strcmp(write_buf, read_buf) == 0);
    std::cout << "    PASSED: data survived a write + read cycle.\n\n";

    std::cout << "[3] Freeing page " << p2 << " and re-allocating...\n";
    free_space_manager.DeallocatePage(p2);
    page_id_t p4 = free_space_manager.AllocatePage();
    std::cout << "    New allocation reused page id: " << p4
               << (p4 == p2 ? " (correctly reused the freed page)\n\n" : " (unexpected: did not reuse)\n\n");
    assert(p4 == p2);

    std::cout << "[4] Simulating an unwritten page (should read back as all zero)...\n";
    page_id_t p5 = free_space_manager.AllocatePage();
    char zero_check[PAGE_SIZE];
    disk_manager.ReadPage(p5, zero_check);
    bool all_zero = true;
    for (char c : zero_check) {
        if (c != 0) { all_zero = false; break; }
    }
    std::cout << "    " << (all_zero ? "PASSED" : "FAILED") << ": unwritten page is zero-filled.\n\n";
    assert(all_zero);

    (void)p3;
    std::cout << "All storage layer tests passed.\n";
    return 0;
}
