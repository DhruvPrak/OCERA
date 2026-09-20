#pragma once
#include <cstddef>
#include <cstdint>

// Shared constants used across every MiniDB module.
namespace minidb {

// Every page on disk (and every page cached in memory later, in the buffer
// pool) is exactly this many bytes. Fixed-size pages are what let the
// DiskManager jump straight to "page N" with simple arithmetic, the same way
// an OS's paging hardware maps a page number to a physical frame.
constexpr std::size_t PAGE_SIZE = 4096;

// Well-known reserved page ids, shared by FreeSpaceManager and
// DatabaseHeader so both refer to the same page numbers instead of each
// hardcoding its own copy. (Declared using the same underlying integer type
// as page_id_t rather than including page.h here, to avoid a circular
// include: page.h includes this file, not the other way around.)
constexpr std::int32_t BITMAP_PAGE_ID = 0;  // page 0: free-space bitmap
constexpr std::int32_t HEADER_PAGE_ID = 1;  // page 1: database header

}  // namespace minidb
