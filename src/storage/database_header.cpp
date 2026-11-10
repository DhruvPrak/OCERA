
#include "database_header.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace ocera {

namespace {

// Fixed offsets in database header page 1.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kPageSizeOffset = 8;
constexpr std::size_t kCatalogRootOffset = 12;

// Writes a 32-bit integer in little-endian format.
void WriteU32LE(
    char* buf,
    std::size_t offset,
    std::uint32_t value
) {
    buf[offset + 0] = static_cast<char>(value & 0xFFu);
    buf[offset + 1] = static_cast<char>((value >> 8) & 0xFFu);
    buf[offset + 2] = static_cast<char>((value >> 16) & 0xFFu);
    buf[offset + 3] = static_cast<char>((value >> 24) & 0xFFu);
}

// Reads a 32-bit integer in little-endian format.
std::uint32_t ReadU32LE(
    const char* buf,
    std::size_t offset
) {
    return
        static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(buf[offset + 0])
        ) |
        (static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(buf[offset + 1])
        ) << 8) |
        (static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(buf[offset + 2])
        ) << 16) |
        (static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(buf[offset + 3])
        ) << 24);
}

} // namespace

DatabaseHeader::DatabaseHeader(DiskManager& disk_manager)
    : disk_manager_(disk_manager),
      version_(0),
      page_size_(0),
      catalog_root_page_id_(INVALID_PAGE_ID) {

    // Check whether page 1 has physically been written.
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

    std::memcpy(
        buf + kMagicOffset,
        kMagic,
        sizeof(kMagic)
    );

    WriteU32LE(buf, kVersionOffset, version_);
    WriteU32LE(buf, kPageSizeOffset, page_size_);

    WriteU32LE(
        buf,
        kCatalogRootOffset,
        static_cast<std::uint32_t>(catalog_root_page_id_)
    );

    disk_manager_.WritePage(HEADER_PAGE_ID, buf);
}

void DatabaseHeader::Load() {
    char buf[PAGE_SIZE];

    disk_manager_.ReadPage(HEADER_PAGE_ID, buf);

    // Validate database magic.
    if (std::memcmp(
            buf + kMagicOffset,
            kMagic,
            sizeof(kMagic)
        ) != 0) {

        throw std::runtime_error(
            "DatabaseHeader: not a valid OCERA file "
            "(magic number mismatch)"
        );
    }

    // Validate format version.
    version_ = ReadU32LE(buf, kVersionOffset);

    if (version_ != kCurrentVersion) {
        throw std::runtime_error(
            "DatabaseHeader: unsupported format version " +
            std::to_string(version_) +
            " (this build supports version " +
            std::to_string(kCurrentVersion) + ")"
        );
    }

    // Validate page size.
    page_size_ = ReadU32LE(buf, kPageSizeOffset);

    if (page_size_ != static_cast<std::uint32_t>(PAGE_SIZE)) {
        throw std::runtime_error(
            "DatabaseHeader: page size mismatch (file was created with " +
            std::to_string(page_size_) +
            "-byte pages, this build uses " +
            std::to_string(PAGE_SIZE) + "-byte pages)"
        );
    }

    // Load Catalog root page ID.
    catalog_root_page_id_ =
        static_cast<page_id_t>(
            ReadU32LE(buf, kCatalogRootOffset)
        );
}

void DatabaseHeader::SetCatalogRootPageId(page_id_t page_id) {
    // Pages 0 and 1 are reserved by OCERA.
    if (page_id != INVALID_PAGE_ID && page_id < 2) {
        throw std::invalid_argument(
            "DatabaseHeader: catalog root page ID must be "
            "INVALID_PAGE_ID or a non-reserved page ID"
        );
    }

    catalog_root_page_id_ = page_id;

    // Immediately persist the updated header.
    Flush();
}

} // namespace ocera
