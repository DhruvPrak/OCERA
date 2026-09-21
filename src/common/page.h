#pragma once
#include <cstdint>
#include <cstring>
#include "config.h"

namespace ocera {

// A page id uniquely identifies a fixed-size slot in the database file.
// -1 is reserved to mean "no page" / "invalid", the same convention SQLite
// and most textbook storage engines use.
using page_id_t = int32_t;
constexpr page_id_t INVALID_PAGE_ID = -1;

// A single fixed-size page of raw bytes. At this stage of the project a Page
// is deliberately "dumb": it doesn't know if it holds a table row, an index
// node, or the free-space bitmap. Higher layers (B+Tree, buffer pool) will
// interpret the bytes later.
struct Page {
    char data[PAGE_SIZE];

    Page() { std::memset(data, 0, PAGE_SIZE); }
};

}  // namespace ocera
