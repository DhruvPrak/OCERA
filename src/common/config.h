#pragma once
#include <cstddef>

// Shared constants used across every MiniDB module.
namespace minidb {

// Every page on disk (and every page cached in memory later, in the buffer
// pool) is exactly this many bytes. Fixed-size pages are what let the
// DiskManager jump straight to "page N" with simple arithmetic, the same way
// an OS's paging hardware maps a page number to a physical frame.
constexpr std::size_t PAGE_SIZE = 4096;

}  // namespace minidb
