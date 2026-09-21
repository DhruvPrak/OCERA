#pragma once
#include <cstdint>
#include "disk_manager.h"
#include "../common/config.h"

namespace ocera {

// DatabaseHeader owns the contents of page HEADER_PAGE_ID (page 1): a small
// fixed-layout record that identifies a file as a OCERA database and
// records the format it was written with. It is a peer of FreeSpaceManager,
// not a layer built on top of it -- FreeSpaceManager must have already
// reserved page 1 (see its constructor) before a DatabaseHeader is
// constructed, or nothing prevents that page from being handed out to
// something else first.
//
// Fields are serialized manually at fixed byte offsets rather than writing
// a raw C++ struct to disk, so the on-disk format doesn't depend on any one
// compiler's struct padding/alignment rules, and a file written on one
// machine can be read correctly on another.
class DatabaseHeader {
public:
    // Magic bytes identifying a OCERA database file.
    static constexpr char kMagic[4] = {'M', 'D', 'B', '1'};
    // Current on-disk format version this build reads and writes.
    static constexpr std::uint32_t kCurrentVersion = 1;

    // On a brand-new file (page 1 never physically written), writes the
    // default header. On an existing file, reads page 1 back and validates
    // it, throwing std::runtime_error with a specific message if the magic
    // number, format version, or page size don't match what this build
    // expects.
    explicit DatabaseHeader(DiskManager& disk_manager);

    std::uint32_t GetVersion() const { return version_; }
    std::uint32_t GetPageSize() const { return page_size_; }
    page_id_t GetCatalogRootPageId() const { return catalog_root_page_id_; }

private:
    DiskManager& disk_manager_;
    std::uint32_t version_;
    std::uint32_t page_size_;
    page_id_t catalog_root_page_id_;

    void WriteDefaults();
    void Load();
    void Flush() const;
};

}  // namespace ocera
