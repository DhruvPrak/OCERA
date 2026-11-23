#pragma once

#include <cstdint>
#include <string>

#include "disk_manager.h"
#include "../common/config.h"
#include "../common/database_id.h"

namespace ocera {

// Page 1 format v2 (little-endian): MDB1 magic at 0, version at 4, page
// size at 8, catalog root at 12, 128-bit DatabaseId at 16, and CRC32 over
// bytes [0, 32) at offset 32. Version-1 headers are upgraded in place by a
// durable sidecar journal before FreeSpaceManager is allowed to load page 0.
class DatabaseHeader {
public:
    static constexpr char kMagic[4] = {'M', 'D', 'B', '1'};
    static constexpr std::uint32_t kCurrentVersion = 2;

    struct BootstrapResult {
        DatabaseId database_id{};
        bool allow_wal_creation = false;
        bool allow_wal_repair = false;
        bool migration_pending = false;
    };

    explicit DatabaseHeader(DiskManager& disk_manager);

    // Must run immediately after DiskManager construction and before
    // FreeSpaceManager. It initializes the reserved bitmap for a new file,
    // validates v2 headers, and performs a journaled v1 -> v2 migration.
    static BootstrapResult Bootstrap(
        DiskManager& disk_manager,
        const std::string& database_file,
        const std::string& wal_file
    );

    // Called only after the matching WAL has been opened successfully.
    static void CompleteBootstrap(const std::string& database_file);

    std::uint32_t GetVersion() const { return version_; }
    std::uint32_t GetPageSize() const { return page_size_; }
    page_id_t GetCatalogRootPageId() const { return catalog_root_page_id_; }
    const DatabaseId& GetDatabaseId() const { return database_id_; }
    void SetCatalogRootPageId(page_id_t page_id);

private:
    DiskManager& disk_manager_;
    std::uint32_t version_ = 0;
    std::uint32_t page_size_ = 0;
    page_id_t catalog_root_page_id_ = INVALID_PAGE_ID;
    DatabaseId database_id_{};

    void WriteDefaults();
    void Load();
    void Flush() const;
};

}  // namespace ocera
