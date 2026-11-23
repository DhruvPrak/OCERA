#include "free_space_manager.h"
#include <stdexcept>
#include <string>

namespace ocera {

FreeSpaceManager::FreeSpaceManager(DiskManager& disk_manager, LogManager* log_manager)
    : disk_manager_(disk_manager), bitmap_(PAGE_SIZE, 0),
      persisted_bitmap_(PAGE_SIZE, 0), log_manager_(log_manager) {
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
    persisted_bitmap_ = bitmap_;
}

void FreeSpaceManager::FlushBitmap() {
    if (!healthy_) {
        throw std::runtime_error("FreeSpaceManager: bitmap manager is in a failed state");
    }
    if (bitmap_ == persisted_bitmap_) return;
    try {
        if (log_manager_) {
            std::uint64_t lsn = 0;
            if (!log_manager_->AppendPageUpdate(
                    0, BITMAP_PAGE_ID,
                    reinterpret_cast<const char*>(persisted_bitmap_.data()),
                    reinterpret_cast<const char*>(bitmap_.data()), &lsn) ||
                !log_manager_->FlushUpTo(lsn)) {
                throw std::runtime_error("FreeSpaceManager: WAL write failed: " +
                                         log_manager_->GetLastError());
            }
        }
        disk_manager_.WritePage(BITMAP_PAGE_ID, reinterpret_cast<const char*>(bitmap_.data()));
        persisted_bitmap_ = bitmap_;
    } catch (...) {
        // After a durable WAL append, a failed data-page write has an
        // uncertain outcome. Stop further allocation decisions until reopen.
        healthy_ = false;
        throw;
    }
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
    if (!healthy_) throw std::runtime_error("FreeSpaceManager: bitmap manager is in a failed state");
    const page_id_t max_trackable_pages = static_cast<page_id_t>(PAGE_SIZE * 8);

    // Pages 0 and 1 are permanently reserved (bitmap, header) and were
    // already marked used in the constructor, so this scan finds the first
    // real free page without needing any special-cased starting point.
    for (page_id_t candidate = BITMAP_PAGE_ID + 1; candidate < max_trackable_pages; ++candidate) {
        if (!GetBit(candidate)) {
            SetBit(candidate, true);
            try {
                FlushBitmap();
            } catch (...) {
                SetBit(candidate, false);
                throw;
            }
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
    if (!healthy_) throw std::runtime_error("FreeSpaceManager: bitmap manager is in a failed state");
    if (page_id == BITMAP_PAGE_ID || page_id == HEADER_PAGE_ID) {
        throw std::invalid_argument(
            "FreeSpaceManager::DeallocatePage: page " + std::to_string(page_id) +
            " is permanently reserved and cannot be freed");
    }
    if (page_id < 2 || page_id >= static_cast<page_id_t>(PAGE_SIZE * 8) ||
        !GetBit(page_id)) {
        throw std::invalid_argument("FreeSpaceManager::DeallocatePage: invalid or unallocated page");
    }
    SetBit(page_id, false);
    try {
        FlushBitmap();
    } catch (...) {
        SetBit(page_id, true);
        throw;
    }
}

bool FreeSpaceManager::IsAllocated(page_id_t page_id) const {
    if (page_id < 0 || page_id >= static_cast<page_id_t>(PAGE_SIZE * 8)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!healthy_) throw std::runtime_error("FreeSpaceManager: bitmap manager is in a failed state");
    return GetBit(page_id);
}

}  // namespace ocera
