#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include "disk_manager.h"

namespace ocera {

// Tracks which pages in the database file are in use, using a bitmap: one
// bit per page, 1 = allocated, 0 = free. This is the same idea a real
// filesystem uses for its free-block bitmap (OS topic: "file allocation
// methods, free space management").
//
// Page 0 of the file is reserved to store this bitmap itself, and page 1 is
// reserved for the database header (see DatabaseHeader), so neither is ever
// handed out to callers via AllocatePage(), nor accepted by
// DeallocatePage().
//
// NOTE (documented scope limit): a single PAGE_SIZE-byte bitmap can track up
// to PAGE_SIZE * 8 pages (32,768 pages = 128 MB at 4 KB pages). That's fine
// for this project's demo scale. A natural "future work" extension is
// chaining multiple bitmap pages together for a larger database.
class FreeSpaceManager {
public:
    explicit FreeSpaceManager(DiskManager& disk_manager);

    // Finds a free page, marks it used, and returns its id.
    // Throws std::runtime_error if the bitmap is full.
    page_id_t AllocatePage();

    // Marks a previously-allocated page as free again, so future
    // AllocatePage() calls can reuse it instead of growing the file.
    void DeallocatePage(page_id_t page_id);

    bool IsAllocated(page_id_t page_id) const;

private:
    DiskManager& disk_manager_;
    std::vector<std::uint8_t> bitmap_;  // packed bits, one PAGE_SIZE-byte page's worth
    mutable std::mutex mutex_;

    void LoadBitmap();
    void FlushBitmap();
    void SetBit(page_id_t page_id, bool value);
    bool GetBit(page_id_t page_id) const;
};

}  // namespace ocera
