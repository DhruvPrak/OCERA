
#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "../common/page.h"
#include "disk_manager.h"
#include "free_space_manager.h"

namespace ocera {

class BufferPoolManager {
public:
    BufferPoolManager(
        std::size_t pool_size,
        DiskManager& disk_manager,
        FreeSpaceManager& free_space_manager
    );

    Page* FetchPage(page_id_t page_id);

    bool UnpinPage(page_id_t page_id, bool is_dirty);

    bool FlushPage(page_id_t page_id);

    void FlushAllPages();

    Page* NewPage(page_id_t* page_id);

    bool DeletePage(page_id_t page_id);

private:
    // One memory slot that can hold one database page.
    struct Frame {
        Page page;
        page_id_t page_id = INVALID_PAGE_ID;
        std::size_t pin_count = 0;
        bool is_dirty = false;
    };

    std::size_t pool_size_;

    DiskManager& disk_manager_;
    FreeSpaceManager& free_space_manager_;

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
};

}  // namespace ocera
