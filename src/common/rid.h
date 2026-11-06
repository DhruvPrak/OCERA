#pragma once
#include <cstdint>
#include "common/page.h"

namespace ocera {

// RID = "record id": the address of one row, as (which page, which slot in
// that page). A B+Tree leaf stores a key together with the RID of the row
// that has that key, so the index can jump straight to the row.
//
// PROPOSED layout (matches docs/api-contracts.md section 4); not yet
// team-approved. Not stored on disk anywhere yet.
struct RID {
    page_id_t page_id = INVALID_PAGE_ID;
    std::uint32_t slot_num = 0;
};

inline bool operator==(const RID& a, const RID& b) {
    return a.page_id == b.page_id && a.slot_num == b.slot_num;
}
inline bool operator!=(const RID& a, const RID& b) { return !(a == b); }

}  // namespace ocera
