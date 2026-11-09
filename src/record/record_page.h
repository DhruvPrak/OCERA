#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <utility>

#include "common/page.h"
#include "common/rid.h"

namespace ocera {

// Stores opaque, non-empty record payloads in one page.
// INT/TEXT serialization belongs to the layer above RecordPage.
class RecordPage {
public:
    static bool Init(Page* page);
    static bool IsValid(const Page* page);

    static bool Insert(Page* page, page_id_t page_id,
                       const std::vector<std::uint8_t>& record, RID* rid);

    static bool Get(const Page* page, page_id_t page_id, const RID& rid,
                    std::vector<std::uint8_t>* record);

    static bool Update(Page* page, page_id_t page_id, const RID& rid,
                       const std::vector<std::uint8_t>& record);

    static bool Scan(
    const Page* page,
    page_id_t page_id,
    std::vector<std::pair<RID, std::vector<std::uint8_t>>>* records
);

    static bool Delete(Page* page, page_id_t page_id, const RID& rid);

    // record_size is payload bytes only. Slot overhead is included
    // automatically when no reusable slot exists.
    static bool HasSpace(const Page* page, std::size_t record_size);

    static bool Compact(Page* page);

    static page_id_t GetNextPageId(const Page* page);
    static bool SetNextPageId(Page* page, page_id_t next_page_id);
};

}  // namespace ocera