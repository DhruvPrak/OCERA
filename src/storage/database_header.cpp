#include "database_header.h"
#include <cstring>
#include <stdexcept>
#include <string>

namespace minidb {

namespace {

// Fixed byte offsets within page 1. Chosen so future fields (Month 2+) can
// be appended after catalog_root without disturbing anything read here.
constexpr std::size_t kMagicOffset = 0;         // 4 bytes
constexpr std::size_t kVersionOffset = 4;       // 4 bytes (uint32, little-endian)
constexpr std::size_t kPageSizeOffset = 8;      // 4 bytes (uint32, little-endian)
constexpr std::size_t kCatalogRootOffset = 12;  // 4 bytes (int32,  little-endian)

// Manual little-endian packing/unpacking, independent of host byte order or
// struct padding -- part of keeping the file format itself portable.
void WriteU32LE(char* buf, std::size_t offset, std::uint32_t value) {
    buf[offset + 0] = static_cast<char>(value & 0xFFu);
    buf[offset + 1] = static_cast<char>((value >> 8) & 0xFFu);
    buf[offset + 2] = static_cast<char>((value >> 16) & 0xFFu);
    buf[offset + 3] = static_cast<char>((value >> 24) & 0xFFu);
}

std::uint32_t ReadU32LE(const char* buf, std::size_t offset) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 0]))) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[offset + 3])) << 24);
}

}  // namespace

DatabaseHeader::DatabaseHeader(DiskManager& disk_manager)
    : disk_manager_(disk_manager), version_(0), page_size_(0), catalog_root_page_id_(INVALID_PAGE_ID) {
    // Page 1 is logically reserved by FreeSpaceManager as soon as the file
    // is created, but that alone doesn't mean it has been *physically*
    // written yet. GetNumPages() reflects real on-disk content, so "page 1
    // has never been written" -- not "the file is empty" -- is the correct
    // fresh-vs-existing check here.
    if (disk_manager_.GetNumPages() <= HEADER_PAGE_ID) {
        WriteDefaults();
    } else {
        Load();
    }
}

void DatabaseHeader::WriteDefaults() {
    version_ = kCurrentVersion;
    page_size_ = static_cast<std::uint32_t>(PAGE_SIZE);
    catalog_root_page_id_ = INVALID_PAGE_ID;
    Flush();
}

void DatabaseHeader::Flush() const {
    char buf[PAGE_SIZE];
    std::memset(buf, 0, PAGE_SIZE);
    std::memcpy(buf + kMagicOffset, kMagic, sizeof(kMagic));
    WriteU32LE(buf, kVersionOffset, version_);
    WriteU32LE(buf, kPageSizeOffset, page_size_);
    WriteU32LE(buf, kCatalogRootOffset, static_cast<std::uint32_t>(catalog_root_page_id_));
    disk_manager_.WritePage(HEADER_PAGE_ID, buf);
}

void DatabaseHeader::Load() {
    char buf[PAGE_SIZE];
    disk_manager_.ReadPage(HEADER_PAGE_ID, buf);

    if (std::memcmp(buf + kMagicOffset, kMagic, sizeof(kMagic)) != 0) {
        throw std::runtime_error(
            "DatabaseHeader: not a valid MiniDB file (magic number mismatch)");
    }

    version_ = ReadU32LE(buf, kVersionOffset);
    if (version_ != kCurrentVersion) {
        throw std::runtime_error(
            "DatabaseHeader: unsupported format version " + std::to_string(version_) +
            " (this build supports version " + std::to_string(kCurrentVersion) + ")");
    }

    page_size_ = ReadU32LE(buf, kPageSizeOffset);
    if (page_size_ != static_cast<std::uint32_t>(PAGE_SIZE)) {
        throw std::runtime_error(
            "DatabaseHeader: page size mismatch (file was created with " +
            std::to_string(page_size_) + "-byte pages, this build uses " +
            std::to_string(PAGE_SIZE) + "-byte pages)");
    }

    catalog_root_page_id_ = static_cast<page_id_t>(ReadU32LE(buf, kCatalogRootOffset));
}

}  // namespace minidb
