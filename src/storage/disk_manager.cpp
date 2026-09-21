#include "disk_manager.h"
#include <cstring>
#include <stdexcept>

namespace ocera {

DiskManager::DiskManager(const std::string& db_file) : file_name_(db_file) {
    // Try opening an existing file first.
    db_io_.open(file_name_, std::ios::in | std::ios::out | std::ios::binary);
    if (!db_io_.is_open()) {
        // File doesn't exist yet: create it, then reopen for read + write.
        db_io_.clear();
        std::ofstream create_stream(file_name_, std::ios::binary);
        create_stream.close();
        db_io_.open(file_name_, std::ios::in | std::ios::out | std::ios::binary);
    }
    if (!db_io_.is_open()) {
        throw std::runtime_error("DiskManager: could not open db file " + file_name_);
    }

    db_io_.seekg(0, std::ios::end);
    std::streampos file_size = db_io_.tellg();
    num_pages_ = static_cast<page_id_t>(file_size / static_cast<std::streamoff>(PAGE_SIZE));
}

DiskManager::~DiskManager() {
    if (db_io_.is_open()) {
        db_io_.flush();
        db_io_.close();
    }
}

void DiskManager::ReadPage(page_id_t page_id, char* page_data) {
    std::lock_guard<std::mutex> lock(db_io_mutex_);
    if (page_id < 0) {
        throw std::invalid_argument("DiskManager::ReadPage: negative page id");
    }

    if (page_id >= num_pages_) {
        // Logically allocated but never physically written yet.
        std::memset(page_data, 0, PAGE_SIZE);
        return;
    }

    db_io_.seekg(static_cast<std::streamoff>(page_id) * PAGE_SIZE);
    db_io_.read(page_data, static_cast<std::streamsize>(PAGE_SIZE));

    if (db_io_.bad()) {
        throw std::runtime_error("DiskManager::ReadPage: I/O error on page " + std::to_string(page_id));
    }

    auto bytes_read = db_io_.gcount();
    if (bytes_read < static_cast<std::streamsize>(PAGE_SIZE)) {
        // Short read near EOF (e.g. a truncated file): zero-fill the rest.
        std::memset(page_data + bytes_read, 0, PAGE_SIZE - static_cast<std::size_t>(bytes_read));
    }
    db_io_.clear();  // clear eof/fail bits so the stream stays usable
}

void DiskManager::WritePage(page_id_t page_id, const char* page_data) {
    std::lock_guard<std::mutex> lock(db_io_mutex_);
    if (page_id < 0) {
        throw std::invalid_argument("DiskManager::WritePage: negative page id");
    }

    db_io_.seekp(static_cast<std::streamoff>(page_id) * PAGE_SIZE);
    db_io_.write(page_data, static_cast<std::streamsize>(PAGE_SIZE));

    if (db_io_.bad()) {
        throw std::runtime_error("DiskManager::WritePage: I/O error on page " + std::to_string(page_id));
    }
    db_io_.flush();

    if (page_id >= num_pages_) {
        num_pages_ = page_id + 1;
    }
}

}  // namespace ocera
