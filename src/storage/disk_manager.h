#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include "../common/page.h"

namespace minidb {

// DiskManager is the only piece of MiniDB allowed to talk to the operating
// system's file I/O directly. Everyone else (buffer pool, B+Tree, WAL...)
// only ever thinks in terms of "page N", never raw byte offsets.
//
// This maps to the OS "disk structure" topic: the database file is treated
// like a simulated disk divided into equal-size blocks (pages), addressed by
// block number.
class DiskManager {
public:
    explicit DiskManager(const std::string& db_file);
    ~DiskManager();

    // Non-copyable: there should be exactly one DiskManager per db file.
    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    // Reads PAGE_SIZE bytes for `page_id` into `page_data`.
    // If the page has never been written, `page_data` is zero-filled instead
    // of failing (a page "doesn't exist" physically until first written).
    void ReadPage(page_id_t page_id, char* page_data);

    // Writes PAGE_SIZE bytes from `page_data` to `page_id`, growing the file
    // if necessary.
    void WritePage(page_id_t page_id, const char* page_data);

    // Number of pages currently backed by the file on disk.
    page_id_t GetNumPages() const { return num_pages_; }

private:
    std::fstream db_io_;
    std::string file_name_;
    page_id_t num_pages_;
    std::mutex db_io_mutex_;
};

}  // namespace minidb
