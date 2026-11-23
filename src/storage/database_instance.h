
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
#include "storage/recovery_manager.h"
#include "txn/transaction_manager.h"

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

    // Synchronous checkpoint marker. The current recovery implementation
    // retains the complete WAL, so checkpoints do not truncate log history.
    void Checkpoint();

    DiskManager& GetDiskManager();
    DatabaseHeader& GetDatabaseHeader();
    LogManager& GetLogManager();
    TransactionManager& GetTransactionManager();
    LockManager& GetLockManager();
    BufferPoolManager& GetBufferPoolManager();
    Catalog& GetCatalog();
    SqlExecutor& GetExecutor();

private:
    // Startup ordering is deliberate: bootstrap and validate the matching WAL,
    // replay it, then construct components that load database metadata pages.
    DiskManager disk_manager_;
    DatabaseHeader::BootstrapResult bootstrap_result_;
    LogManager log_manager_;
    RecoveryManager recovery_manager_;
    FreeSpaceManager free_space_manager_;
    DatabaseHeader database_header_;
    BufferPoolManager buffer_pool_manager_;
    LockManager lock_manager_;
    TransactionManager transaction_manager_;
    Catalog catalog_;
    SqlExecutor executor_;
};

}  // namespace ocera
