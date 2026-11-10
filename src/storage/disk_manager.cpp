
#include "disk_manager.h"

#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace ocera {

DiskManager::DiskManager(
    const std::string& db_file,
    DatabaseOpenMode mode
)
    : file_name_(db_file),
      num_pages_(0) {

    namespace fs = std::filesystem;

    const bool exists = fs::exists(file_name_);

    if (mode == DatabaseOpenMode::CREATE_NEW && exists) {
        throw std::runtime_error(
            "DiskManager: database file already exists: " + file_name_
        );
    }

    if (mode == DatabaseOpenMode::OPEN_EXISTING && !exists) {
        throw std::runtime_error(
            "DiskManager: database file does not exist: " + file_name_
        );
    }

    if (mode == DatabaseOpenMode::CREATE_NEW ||
        (mode == DatabaseOpenMode::CREATE_OR_OPEN && !exists)) {

        std::ofstream create_stream(
            file_name_,
            std::ios::out | std::ios::binary
        );

        if (!create_stream.is_open()) {
            throw std::runtime_error(
                "DiskManager: could not create db file " + file_name_
            );
        }

        create_stream.close();

        if (!create_stream) {
            throw std::runtime_error(
                "DiskManager: failed to initialize db file " + file_name_
            );
        }
    }

    db_io_.open(
        file_name_,
        std::ios::in | std::ios::out | std::ios::binary
    );

    if (!db_io_.is_open()) {
        throw std::runtime_error(
            "DiskManager: could not open db file " + file_name_
        );
    }

    db_io_.seekg(0, std::ios::end);
    const std::streampos file_size = db_io_.tellg();

    if (file_size < 0) {
        throw std::runtime_error(
            "DiskManager: could not determine database file size"
        );
    }

    const auto size = static_cast<std::streamoff>(file_size);

    if (mode == DatabaseOpenMode::OPEN_EXISTING) {
        if (size == 0) {
            throw std::runtime_error(
                "DiskManager: existing database file is empty"
            );
        }

        if (size % static_cast<std::streamoff>(PAGE_SIZE) != 0) {
            throw std::runtime_error(
                "DiskManager: database file size is not page-aligned"
            );
        }
    }

    num_pages_ = static_cast<page_id_t>(
        size / static_cast<std::streamoff>(PAGE_SIZE)
    );

    db_io_.clear();
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
        throw std::invalid_argument(
            "DiskManager::ReadPage: negative page id"
        );
    }

    if (page_id >= num_pages_) {
        std::memset(page_data, 0, PAGE_SIZE);
        return;
    }

    db_io_.seekg(
        static_cast<std::streamoff>(page_id) * PAGE_SIZE
    );

    db_io_.read(
        page_data,
        static_cast<std::streamsize>(PAGE_SIZE)
    );

    if (db_io_.bad()) {
        throw std::runtime_error(
            "DiskManager::ReadPage: I/O error on page " +
            std::to_string(page_id)
        );
    }

    const auto bytes_read = db_io_.gcount();

    if (bytes_read < static_cast<std::streamsize>(PAGE_SIZE)) {
        std::memset(
            page_data + bytes_read,
            0,
            PAGE_SIZE - static_cast<std::size_t>(bytes_read)
        );
    }

    db_io_.clear();
}

void DiskManager::WritePage(page_id_t page_id, const char* page_data) {
    std::lock_guard<std::mutex> lock(db_io_mutex_);

    if (page_id < 0) {
        throw std::invalid_argument(
            "DiskManager::WritePage: negative page id"
        );
    }

    db_io_.seekp(
        static_cast<std::streamoff>(page_id) * PAGE_SIZE
    );

    db_io_.write(
        page_data,
        static_cast<std::streamsize>(PAGE_SIZE)
    );

    if (!db_io_) {
        throw std::runtime_error(
            "DiskManager::WritePage: I/O error on page " +
            std::to_string(page_id)
        );
    }

    db_io_.flush();

    if (!db_io_) {
        throw std::runtime_error(
            "DiskManager::WritePage: failed to flush page " +
            std::to_string(page_id)
        );
    }

    if (page_id >= num_pages_) {
        num_pages_ = page_id + 1;
    }
}

}  // namespace ocera
