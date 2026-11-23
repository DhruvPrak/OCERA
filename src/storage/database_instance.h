
#pragma once

#include <cstddef>
#include <string>

#include "storage/disk_manager.h"
#include "storage/free_space_manager.h"
#include "storage/database_header.h"
#include "storage/buffer_pool_manager.h"
#include "catalog/catalog.h"
#include "sql/executor.h"
#include "../wal/log_manager.h"

namespace ocera {

// Owns all runtime components associated with one open database. The WAL
// path is the database path with ".wal" appended.
//
// Member declaration order is intentional:
// dependencies are constructed first and destroyed last.
class DatabaseInstance {
public:
    DatabaseInstance(
        const std::string& db_file,
        std::size_t pool_size,
        DatabaseOpenMode open_mode = DatabaseOpenMode::CREATE_OR_OPEN
    );

    // Flush dirty buffer-pool pages before closing the database.
    void Flush();

    DiskManager& GetDiskManager();
    DatabaseHeader& GetDatabaseHeader();
    LogManager& GetLogManager();
    BufferPoolManager& GetBufferPoolManager();
    Catalog& GetCatalog();
    SqlExecutor& GetExecutor();

private:
    // Startup ordering is deliberate: bootstrap and validate the matching WAL
    // before any component reads or writes database metadata pages. A future
    // RecoveryManager belongs between log_manager_ and free_space_manager_.
    DiskManager disk_manager_;
    DatabaseHeader::BootstrapResult bootstrap_result_;
    LogManager log_manager_;
    FreeSpaceManager free_space_manager_;
    DatabaseHeader database_header_;
    BufferPoolManager buffer_pool_manager_;
    Catalog catalog_;
    SqlExecutor executor_;
};

}  // namespace ocera
