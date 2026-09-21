#include "free_space_manager.h"
#include <stdexcept>
#include <string>

namespace ocera {

FreeSpaceManager::FreeSpaceManager(DiskManager& disk_manager)
    : disk_manager_(disk_manager), bitmap_(PAGE_SIZE, 0) {
    if (disk_manager_.GetNumPages() == 0) {
        // Brand new database file: reserve page 0 for the bitmap itself and
        // page 1 for the database header (see DatabaseHeader). Both must be
        // marked used here, before any real AllocatePage() call happens, so
        // the ordinary linear scan below naturally skips them -- no special
        // "start scanning at page 2" logic is needed.
        SetBit(BITMAP_PAGE_ID, true);
        SetBit(HEADER_PAGE_ID, true);
        FlushBitmap();
    } else {
        LoadBitmap();
    }
}

void FreeSpaceManager::LoadBitmap() {
    disk_manager_.ReadPage(BITMAP_PAGE_ID, reinterpret_cast<char*>(bitmap_.data()));
}

void FreeSpaceManager::FlushBitmap() {
    disk_manager_.WritePage(BITMAP_PAGE_ID, reinterpret_cast<const char*>(bitmap_.data()));
}

void FreeSpaceManager::SetBit(page_id_t page_id, bool value) {
    std::size_t byte_index = static_cast<std::size_t>(page_id) / 8;
    std::uint8_t bit_index = static_cast<std::uint8_t>(page_id % 8);
    if (value) {
        bitmap_[byte_index] |= static_cast<std::uint8_t>(1u << bit_index);
    } else {
        bitmap_[byte_index] &= static_cast<std::uint8_t>(~(1u << bit_index));
    }
}

bool FreeSpaceManager::GetBit(page_id_t page_id) const {
    std::size_t byte_index = static_cast<std::size_t>(page_id) / 8;
    std::uint8_t bit_index = static_cast<std::uint8_t>(page_id % 8);
    return (bitmap_[byte_index] & (1u << bit_index)) != 0;
}

page_id_t FreeSpaceManager::AllocatePage() {
    std::lock_guard<std::mutex> lock(mutex_);
    const page_id_t max_trackable_pages = static_cast<page_id_t>(PAGE_SIZE * 8);

    // Pages 0 and 1 are permanently reserved (bitmap, header) and were
    // already marked used in the constructor, so this scan finds the first
    // real free page without needing any special-cased starting point.
    for (page_id_t candidate = BITMAP_PAGE_ID + 1; candidate < max_trackable_pages; ++candidate) {
        if (!GetBit(candidate)) {
            SetBit(candidate, true);
            FlushBitmap();
            return candidate;
        }
    }

    throw std::runtime_error(
        "FreeSpaceManager::AllocatePage: bitmap page is full "
        "(single-bitmap-page limit reached -- see class-level note about "
        "extending with multiple bitmap pages)");
}

void FreeSpaceManager::DeallocatePage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (page_id == BITMAP_PAGE_ID || page_id == HEADER_PAGE_ID) {
        throw std::invalid_argument(
            "FreeSpaceManager::DeallocatePage: page " + std::to_string(page_id) +
            " is permanently reserved and cannot be freed");
    }
    SetBit(page_id, false);
    FlushBitmap();
}

bool FreeSpaceManager::IsAllocated(page_id_t page_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetBit(page_id);
}

}  // namespace ocera
