#pragma once

#include <cstdint>
#include "common/page.h"

namespace ocera {

struct RID {
    page_id_t page_id = INVALID_PAGE_ID;
    std::uint32_t slot_num = 0;
    std::uint32_t generation = 0;
};

inline bool operator==(const RID& a, const RID& b) {
    return a.page_id == b.page_id &&
           a.slot_num == b.slot_num &&
           a.generation == b.generation;
}

inline bool operator!=(const RID& a, const RID& b) {
    return !(a == b);
}

}  // namespace ocera