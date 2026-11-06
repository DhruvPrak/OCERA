#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "../src/common/config.h"
#include "../src/storage/buffer_pool_manager.h"
#include "../src/storage/disk_manager.h"
#include "../src/storage/free_space_manager.h"

using namespace ocera;

namespace {

bool IsZeroPage(const Page& page) {
    for (char byte : page.data) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

void WriteText(Page* page, const char* text) {
    assert(page != nullptr);
    std::memset(page->data, 0, PAGE_SIZE);
    std::strncpy(page->data, text, PAGE_SIZE - 1);
}

void ExpectText(const Page* page, const char* expected) {
    assert(page != nullptr);
    assert(std::strcmp(page->data, expected) == 0);
}

}  // namespace

int main() {
    const std::string db_file = "test_buffer_pool.db";
    std::remove(db_file.c_str());

    std::cout << "=== OCERA Buffer Pool Manager Test ===\n\n";

    {
        DiskManager disk_manager(db_file);
        FreeSpaceManager free_space_manager(disk_manager);
        BufferPoolManager buffer_pool(2, disk_manager, free_space_manager);

        std::cout << "[1] NewPage returns a zeroed, pinned page...\n";
        page_id_t page_a_id = INVALID_PAGE_ID;
        Page* page_a = buffer_pool.NewPage(&page_a_id);
        assert(page_a != nullptr);
        assert(page_a_id >= 2);
        assert(IsZeroPage(*page_a));
        WriteText(page_a, "buffer-pool-page-A");
        assert(buffer_pool.UnpinPage(page_a_id, true));

        page_id_t page_b_id = INVALID_PAGE_ID;
        Page* page_b = buffer_pool.NewPage(&page_b_id);
        assert(page_b != nullptr);
        assert(page_b_id >= 2);
        assert(page_b_id != page_a_id);
        assert(IsZeroPage(*page_b));
        WriteText(page_b, "buffer-pool-page-B");
        assert(buffer_pool.UnpinPage(page_b_id, true));
        std::cout << "    PASSED\n\n";

        std::cout << "[2] Dirty LRU eviction persists page contents...\n";
        page_a = buffer_pool.FetchPage(page_a_id);
        assert(page_a != nullptr);
        ExpectText(page_a, "buffer-pool-page-A");
        assert(buffer_pool.UnpinPage(page_a_id, false));

        // Page A was made most-recently-used by the fetch above. Page B is
        // now the least-recently-used unpinned frame.
        page_id_t page_c_id = INVALID_PAGE_ID;
        Page* page_c = buffer_pool.NewPage(&page_c_id);
        assert(page_c != nullptr);
        assert(page_c_id >= 2);
        assert(IsZeroPage(*page_c));

        page_b = buffer_pool.FetchPage(page_b_id);
        assert(page_b != nullptr);
        ExpectText(page_b, "buffer-pool-page-B");
        assert(buffer_pool.UnpinPage(page_b_id, false));
        assert(buffer_pool.UnpinPage(page_c_id, false));
        std::cout << "    PASSED\n\n";

        std::cout << "[3] Pinned pages cannot be evicted; exhaustion rolls back...\n";
        page_a = buffer_pool.FetchPage(page_a_id);
        page_b = buffer_pool.FetchPage(page_b_id);
        assert(page_a != nullptr);
        assert(page_b != nullptr);

        page_id_t failed_page_id = INVALID_PAGE_ID;
        Page* failed_page = buffer_pool.NewPage(&failed_page_id);
        assert(failed_page == nullptr);
        assert(failed_page_id == INVALID_PAGE_ID);

        assert(buffer_pool.UnpinPage(page_a_id, false));
        assert(buffer_pool.UnpinPage(page_b_id, false));

        // The failed NewPage allocation must have been returned to the
        // free-space bitmap, so the next allocation reuses that page ID.
        page_id_t reused_page_id = INVALID_PAGE_ID;
        Page* reused_page = buffer_pool.NewPage(&reused_page_id);
        assert(reused_page != nullptr);
        assert(reused_page_id == page_c_id + 1);
        assert(IsZeroPage(*reused_page));
        std::cout << "    PASSED\n\n";

        std::cout << "[4] DeletePage rejects pinned/reserved pages...\n";
        assert(!buffer_pool.DeletePage(reused_page_id));
        assert(!buffer_pool.DeletePage(BITMAP_PAGE_ID));
        assert(!buffer_pool.DeletePage(HEADER_PAGE_ID));
        assert(buffer_pool.UnpinPage(reused_page_id, false));
        assert(buffer_pool.DeletePage(reused_page_id));
        assert(buffer_pool.FetchPage(reused_page_id) == nullptr);
        std::cout << "    PASSED\n\n";

        std::cout << "[5] FlushPage persists dirty contents...\n";
        page_id_t flush_page_id = INVALID_PAGE_ID;
        Page* flush_page = buffer_pool.NewPage(&flush_page_id);
        assert(flush_page != nullptr);
        WriteText(flush_page, "explicit-flush-check");
        assert(buffer_pool.UnpinPage(flush_page_id, true));
        assert(buffer_pool.FlushPage(flush_page_id));

        char disk_data[PAGE_SIZE] = {};
        disk_manager.ReadPage(flush_page_id, disk_data);
        assert(std::strcmp(disk_data, "explicit-flush-check") == 0);

        page_id_t flush_all_page_id = INVALID_PAGE_ID;
        Page* flush_all_page = buffer_pool.NewPage(&flush_all_page_id);
        assert(flush_all_page != nullptr);
        WriteText(flush_all_page, "flush-all-check");
        assert(buffer_pool.UnpinPage(flush_all_page_id, true));
        buffer_pool.FlushAllPages();

        std::memset(disk_data, 0, PAGE_SIZE);
        disk_manager.ReadPage(flush_all_page_id, disk_data);
        assert(std::strcmp(disk_data, "flush-all-check") == 0);
        std::cout << "    PASSED\n\n";
    }

    std::remove(db_file.c_str());
    std::cout << "All Buffer Pool Manager tests passed.\n";
    return 0;
}
