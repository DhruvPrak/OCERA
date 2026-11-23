
#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "../common/page.h"
#include "disk_manager.h"
#include "free_space_manager.h"
#include "../wal/log_manager.h"

namespace ocera {

class BufferPoolManager {
public:
    BufferPoolManager(
        std::size_t pool_size,
        DiskManager& disk_manager,
        FreeSpaceManager& free_space_manager,
        LogManager* log_manager = nullptr
    );

    Page* FetchPage(page_id_t page_id);

    // If bytes changed, append a full before/after WAL record before releasing
    // the pin. txn_id == 0 denotes a redo-only system update.
    bool UnpinPage(page_id_t page_id, bool is_dirty, std::uint64_t txn_id = 0);

    bool FlushPage(page_id_t page_id);

    void FlushAllPages();

    Page* NewPage(page_id_t* page_id);

    bool DeletePage(page_id_t page_id);

    // Restore the before-images of all updates belonging to txn_id. The
    // caller must retain the transaction's locks until this returns.
    bool UndoTransaction(std::uint64_t txn_id);

private:
    // One memory slot that can hold one database page.
    struct Frame {
        Page page;
        page_id_t page_id = INVALID_PAGE_ID;
        std::size_t pin_count = 0;
        bool is_dirty = false;
        Page logged_image;
        std::uint64_t page_lsn = 0;
    };

    std::size_t pool_size_;

    DiskManager& disk_manager_;
    FreeSpaceManager& free_space_manager_;
    LogManager* log_manager_;

    // All frames currently available in the buffer pool.
    std::vector<Frame> frames_;

    // Maps a database page ID to its buffer frame index.
    std::unordered_map<page_id_t, std::size_t> page_table_;

    // Frame indices that have not yet been assigned a page.
    std::vector<std::size_t> free_frames_;

    // LRU list: most recently used at the front.
    // Only unpinned frames belong in this list.
    std::list<std::size_t> lru_list_;

    // Allows quick removal of a frame from the LRU list.
    std::unordered_map<
        std::size_t,
        std::list<std::size_t>::iterator
    > lru_positions_;

    // Protects Buffer Pool metadata and frame operations.
    std::mutex mutex_;

    void AddToLRU(std::size_t frame_id);

    void RemoveFromLRU(std::size_t frame_id);

    void UpdateLRU(std::size_t frame_id);

    bool GetAvailableFrame(std::size_t* frame_id);
    void FlushFrame(Frame& frame);
    bool RestorePage(page_id_t page_id, const char* image, std::uint64_t lsn);
};

}  // namespace ocera
