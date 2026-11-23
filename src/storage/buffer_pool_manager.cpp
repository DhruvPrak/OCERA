#include "buffer_pool_manager.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ocera {
namespace {

constexpr page_id_t MAX_TRACKABLE_PAGE_ID =
    static_cast<page_id_t>(PAGE_SIZE * 8);

bool IsValidPageId(page_id_t page_id) {
    return page_id >= 0 && page_id < MAX_TRACKABLE_PAGE_ID;
}

}  // namespace

BufferPoolManager::BufferPoolManager(
    std::size_t pool_size,
    DiskManager& disk_manager,
    FreeSpaceManager& free_space_manager,
    LogManager* log_manager
)
    : pool_size_(pool_size),
      disk_manager_(disk_manager),
      free_space_manager_(free_space_manager),
      log_manager_(log_manager),
      frames_(pool_size) {
    if (pool_size_ == 0) {
        throw std::invalid_argument(
            "Buffer pool size must be greater than zero"
        );
    }

    // Reserve capacity so returning a frame to this vector does not allocate.
    free_frames_.reserve(pool_size_);
    page_table_.reserve(pool_size_);
    lru_positions_.reserve(pool_size_);

    for (std::size_t frame_id = 0; frame_id < pool_size_; ++frame_id) {
        free_frames_.push_back(frame_id);
    }
}

void BufferPoolManager::AddToLRU(std::size_t frame_id) {
    if (frame_id >= frames_.size()) {
        throw std::out_of_range("Buffer pool frame ID is out of range");
    }

    if (lru_positions_.find(frame_id) != lru_positions_.end()) {
        return;
    }

    lru_list_.push_front(frame_id);

    try {
        const auto inserted = lru_positions_.emplace(
            frame_id,
            lru_list_.begin()
        );

        if (!inserted.second) {
            lru_list_.pop_front();
            throw std::logic_error("Duplicate frame in the LRU index");
        }
    } catch (...) {
        if (!lru_list_.empty() && lru_list_.front() == frame_id) {
            lru_list_.pop_front();
        }
        throw;
    }
}

void BufferPoolManager::RemoveFromLRU(std::size_t frame_id) {
    const auto it = lru_positions_.find(frame_id);

    if (it == lru_positions_.end()) {
        return;
    }

    lru_list_.erase(it->second);
    lru_positions_.erase(it);
}

void BufferPoolManager::UpdateLRU(std::size_t frame_id) {
    RemoveFromLRU(frame_id);
    AddToLRU(frame_id);
}

// The caller must hold mutex_ while calling this method.
bool BufferPoolManager::GetAvailableFrame(std::size_t* frame_id) {
    if (frame_id == nullptr) {
        return false;
    }

    if (!free_frames_.empty()) {
        *frame_id = free_frames_.back();
        free_frames_.pop_back();
        return true;
    }

    if (lru_list_.empty()) {
        return false;
    }

    const std::size_t victim_id = lru_list_.back();
    Frame& victim = frames_[victim_id];

    if (victim.pin_count != 0 || victim.page_id == INVALID_PAGE_ID) {
        throw std::logic_error("Buffer pool LRU state is inconsistent");
    }

    // Do not discard a dirty page unless its write succeeds.
    if (victim.is_dirty) {
        FlushFrame(victim);
    }

    page_table_.erase(victim.page_id);
    RemoveFromLRU(victim_id);

    victim.page_id = INVALID_PAGE_ID;
    victim.pin_count = 0;
    victim.is_dirty = false;

    *frame_id = victim_id;
    return true;
}

Page* BufferPoolManager::FetchPage(page_id_t page_id) {
    if (!IsValidPageId(page_id)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto existing = page_table_.find(page_id);

    if (existing != page_table_.end()) {
        const std::size_t frame_id = existing->second;
        Frame& frame = frames_[frame_id];

        if (frame.pin_count == std::numeric_limits<std::size_t>::max()) {
            return nullptr;
        }

        if (frame.pin_count == 0) {
            RemoveFromLRU(frame_id);
        }

        ++frame.pin_count;
        return &frame.page;
    }

    if (!free_space_manager_.IsAllocated(page_id)) {
        return nullptr;
    }

    std::size_t frame_id = 0;

    if (!GetAvailableFrame(&frame_id)) {
        return nullptr;
    }

    Frame& frame = frames_[frame_id];

    try {
        disk_manager_.ReadPage(page_id, frame.page.data);

        const auto inserted = page_table_.emplace(page_id, frame_id);
        if (!inserted.second) {
            throw std::logic_error(
                "Page is already present in the buffer pool"
            );
        }

        frame.page_id = page_id;
        frame.pin_count = 1;
        frame.is_dirty = false;
        frame.logged_image = frame.page;
        frame.page_lsn = 0;
    } catch (...) {
        frame.page = Page{};
        frame.page_id = INVALID_PAGE_ID;
        frame.pin_count = 0;
        frame.is_dirty = false;
        free_frames_.push_back(frame_id);
        throw;
    }

    return &frame.page;
}

bool BufferPoolManager::UnpinPage(
    page_id_t page_id,
    bool is_dirty,
    std::uint64_t txn_id
) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    const std::size_t frame_id = it->second;
    Frame& frame = frames_[frame_id];
    if (frame.pin_count == 0) return false;

    auto release_pin = [&]() {
        --frame.pin_count;
        if (frame.pin_count == 0) {
            try {
                UpdateLRU(frame_id);
            } catch (...) {
                ++frame.pin_count;
                throw;
            }
        }
    };

    const bool changed =
        std::memcmp(frame.logged_image.data, frame.page.data, PAGE_SIZE) != 0;
    if (changed) {
        if (log_manager_) {
            std::uint64_t lsn = 0;
            if (!log_manager_->AppendPageUpdate(
                    txn_id, page_id, frame.logged_image.data,
                    frame.page.data, &lsn)) {
                // The mutation has not been accepted: restore the last
                // successfully logged image before releasing this pin.
                frame.page = frame.logged_image;
                release_pin();
                return false;
            }
            frame.page_lsn = lsn;
        } else if (txn_id != 0) {
            frame.page = frame.logged_image;
            release_pin();
            return false;
        }
        frame.logged_image = frame.page;
        frame.is_dirty = true;
    } else if (is_dirty && !log_manager_) {
        frame.is_dirty = true;
    }

    release_pin();
    return true;
}

void BufferPoolManager::FlushFrame(Frame& frame) {
    if (!frame.is_dirty) return;
    if (frame.pin_count != 0)
        throw std::runtime_error("BufferPoolManager: cannot flush a pinned page");
    if (log_manager_ && frame.page_lsn != 0 &&
        !log_manager_->FlushUpTo(frame.page_lsn)) {
        throw std::runtime_error("BufferPoolManager: WAL sync failed: " +
                                 log_manager_->GetLastError());
    }
    disk_manager_.WritePage(frame.page_id, frame.page.data);
    frame.is_dirty = false;
}

bool BufferPoolManager::FlushPage(page_id_t page_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;
    FlushFrame(frames_[it->second]);
    return true;
}

void BufferPoolManager::FlushAllPages() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Frame& frame : frames_) {
        if (frame.page_id != INVALID_PAGE_ID && frame.is_dirty) FlushFrame(frame);
    }
}

Page* BufferPoolManager::NewPage(page_id_t* page_id) {
    if (page_id == nullptr) return nullptr;
    *page_id = INVALID_PAGE_ID;

    std::lock_guard<std::mutex> lock(mutex_);
    const page_id_t new_page_id = free_space_manager_.AllocatePage();
    std::size_t frame_id = 0;
    bool frame_available = false;
    try {
        frame_available = GetAvailableFrame(&frame_id);
    } catch (...) {
        try { free_space_manager_.DeallocatePage(new_page_id); } catch (...) {}
        throw;
    }
    if (!frame_available) {
        free_space_manager_.DeallocatePage(new_page_id);
        return nullptr;
    }

    Frame& frame = frames_[frame_id];
    std::array<char, PAGE_SIZE> before{};
    try {
        disk_manager_.ReadPage(new_page_id, before.data());
        const auto inserted = page_table_.emplace(new_page_id, frame_id);
        if (!inserted.second) throw std::logic_error("New page already exists in the buffer pool");

        frame.page_id = new_page_id;
        frame.pin_count = 1;
        frame.page = Page{};
        std::memcpy(frame.logged_image.data, before.data(), PAGE_SIZE);
        frame.page_lsn = 0;
        frame.is_dirty = true;

        if (log_manager_) {
            std::uint64_t lsn = 0;
            if (!log_manager_->AppendPageUpdate(
                    0, new_page_id, before.data(), frame.page.data, &lsn)) {
                throw std::runtime_error("BufferPoolManager: failed to log new page: " +
                                         log_manager_->GetLastError());
            }
            frame.page_lsn = lsn;
        }
        frame.logged_image = frame.page;
    } catch (...) {
        page_table_.erase(new_page_id);
        frame.page = Page{};
        frame.logged_image = Page{};
        frame.page_id = INVALID_PAGE_ID;
        frame.pin_count = 0;
        frame.is_dirty = false;
        frame.page_lsn = 0;
        free_frames_.push_back(frame_id);
        try { free_space_manager_.DeallocatePage(new_page_id); } catch (...) {}
        throw;
    }

    *page_id = new_page_id;
    return &frame.page;
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
    if (page_id < 2 || !IsValidPageId(page_id)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!free_space_manager_.IsAllocated(page_id)) {
        return false;
    }

    const auto it = page_table_.find(page_id);

    if (it != page_table_.end()) {
        const Frame& frame = frames_[it->second];

        if (frame.pin_count > 0) {
            return false;
        }
    }

    // If bitmap persistence fails, leave the buffer-pool mapping untouched.
    free_space_manager_.DeallocatePage(page_id);

    if (it != page_table_.end()) {
        const std::size_t frame_id = it->second;
        Frame& frame = frames_[frame_id];

        RemoveFromLRU(frame_id);
        page_table_.erase(it);

        frame.page = Page{};
        frame.page_id = INVALID_PAGE_ID;
        frame.pin_count = 0;
        frame.is_dirty = false;
        frame.logged_image = Page{};
        frame.page_lsn = 0;

        free_frames_.push_back(frame_id);
    }

    return true;
}


bool BufferPoolManager::RestorePage(page_id_t page_id, const char* image,
                                    std::uint64_t lsn) {
    if (!image || !IsValidPageId(page_id)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        if (log_manager_ && !log_manager_->FlushUpTo(lsn)) return false;
        disk_manager_.WritePage(page_id, image);
        return true;
    }
    Frame& frame = frames_[it->second];
    if (frame.pin_count != 0) return false;
    std::memcpy(frame.page.data, image, PAGE_SIZE);
    frame.logged_image = frame.page;
    frame.page_lsn = std::max(frame.page_lsn, lsn);
    frame.is_dirty = true;
    return true;
}

bool BufferPoolManager::UndoTransaction(std::uint64_t txn_id) {
    if (!log_manager_ || txn_id == 0) return false;
    std::vector<LogRecord> records;
    if (!log_manager_->Scan(&records)) return false;
    try {
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            if (it->type != LogRecordType::PAGE_UPDATE || it->txn_id != txn_id) continue;
            if (it->payload.size() != 2U * PAGE_SIZE ||
                !RestorePage(it->page_id,
                             reinterpret_cast<const char*>(it->payload.data()),
                             it->lsn)) return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace ocera
