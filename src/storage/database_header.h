
#pragma once

#include <cstdint>

#include "disk_manager.h"
#include "../common/config.h"

namespace ocera {

// DatabaseHeader owns page HEADER_PAGE_ID (page 1).
// It stores the database format information and Catalog root page ID.
class DatabaseHeader {
public:
    // Magic bytes identifying an OCERA database.
    static constexpr char kMagic[4] = {'M', 'D', 'B', '1'};

    // Current on-disk format version.
    static constexpr std::uint32_t kCurrentVersion = 1;

    // Creates a default header for a new database or loads and validates
    // the existing header.
    explicit DatabaseHeader(DiskManager& disk_manager);

    std::uint32_t GetVersion() const {
        return version_;
    }

    std::uint32_t GetPageSize() const {
        return page_size_;
    }

    page_id_t GetCatalogRootPageId() const {
        return catalog_root_page_id_;
    }

    // Updates and persists the Catalog root page ID.
    // INVALID_PAGE_ID means no Catalog root page exists yet.
    void SetCatalogRootPageId(page_id_t page_id);

private:
    DiskManager& disk_manager_;

    std::uint32_t version_;
    std::uint32_t page_size_;
    page_id_t catalog_root_page_id_;

    void WriteDefaults();
    void Load();
    void Flush() const;
};

} // namespace ocera
