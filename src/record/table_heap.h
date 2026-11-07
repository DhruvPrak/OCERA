#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <utility>

#include "common/rid.h"
#include "record/record_page.h"
#include "storage/buffer_pool_manager.h"

namespace ocera {

class TableHeap {
public:
    // Creates the first RecordPage and returns its page ID.
    // The caller must retain this ID to reopen the heap later.
    static bool Create(
        BufferPoolManager& bpm,
        page_id_t* first_page_id
    );
    bool Scan(
    std::vector<std::pair<RID, std::vector<std::uint8_t>>>* records
    );

    // Opens an existing heap using its first RecordPage ID.
    TableHeap(
        BufferPoolManager& bpm,
        page_id_t first_page_id
    );

    bool Insert(
        const std::vector<std::uint8_t>& record,
        RID* rid
    );

    bool Get(
        const RID& rid,
        std::vector<std::uint8_t>* record
    );

    bool Update(
        const RID& rid,
        const std::vector<std::uint8_t>& record
    );

    bool Delete(const RID& rid);

    page_id_t GetFirstPageId() const {
        return first_page_id_;
    }

private:
    BufferPoolManager& bpm_;
    page_id_t first_page_id_;

    // Returns the requested page pinned if it belongs to this heap.
    // The caller is responsible for unpinning the returned page.
    Page* FetchPageInHeap(page_id_t target_page_id) const;
};

}  // namespace ocera