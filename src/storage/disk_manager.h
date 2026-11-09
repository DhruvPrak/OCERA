
#pragma once

#include <fstream>
#include <mutex>
#include <string>

#include "../common/page.h"

namespace ocera {

enum class DatabaseOpenMode {
    CREATE_NEW,
    OPEN_EXISTING,
    CREATE_OR_OPEN
};

// DiskManager is the only piece of OCERA allowed to talk to the operating
// system's file I/O directly.
class DiskManager {
public:
    // CREATE_OR_OPEN preserves the previous constructor behavior.
    explicit DiskManager(
        const std::string& db_file,
        DatabaseOpenMode mode = DatabaseOpenMode::CREATE_OR_OPEN
    );

    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    void ReadPage(page_id_t page_id, char* page_data);
    void WritePage(page_id_t page_id, const char* page_data);

    page_id_t GetNumPages() const { return num_pages_; }

private:
    std::fstream db_io_;
    std::string file_name_;
    page_id_t num_pages_;
    std::mutex db_io_mutex_;
};

}  // namespace ocera
