
#include "storage/database_instance.h"

#include <stdexcept>

namespace ocera {

DatabaseInstance::DatabaseInstance(
    const std::string& db_file,
    std::size_t pool_size,
    DatabaseOpenMode open_mode
)
    : disk_manager_(db_file, open_mode),
      bootstrap_result_(DatabaseHeader::Bootstrap(disk_manager_, db_file, db_file + ".wal")),
      log_manager_(db_file + ".wal", bootstrap_result_.database_id,
                   bootstrap_result_.allow_wal_creation,
                   bootstrap_result_.allow_wal_repair),
      recovery_manager_(disk_manager_, log_manager_),
      free_space_manager_(disk_manager_, &log_manager_),
      database_header_(disk_manager_, &log_manager_),
      buffer_pool_manager_(
          pool_size,
          disk_manager_,
          free_space_manager_,
          &log_manager_
      ),
      lock_manager_(),
      transaction_manager_(lock_manager_, &log_manager_,
          [this](txn_id_t txn_id) {
              return buffer_pool_manager_.UndoTransaction(txn_id);
          }),
      catalog_(),
      executor_(catalog_, buffer_pool_manager_, &database_header_,
                &transaction_manager_) {
    DatabaseHeader::CompleteBootstrap(db_file);
}

void DatabaseInstance::Flush() {
    const std::uint64_t last_lsn = log_manager_.GetLastLSN();
    if (!log_manager_.FlushUpTo(last_lsn)) {
        throw std::runtime_error("DatabaseInstance::Flush: WAL sync failed: " +
                                 log_manager_.GetLastError());
    }
    buffer_pool_manager_.FlushAllPages();
    disk_manager_.Sync();
}

void DatabaseInstance::Checkpoint() {
    const bool completed = transaction_manager_.RunWhenIdle([this]() {
        buffer_pool_manager_.FlushAllPages();
        disk_manager_.Sync();

        std::uint64_t checkpoint_lsn = 0;
        if (!log_manager_.AppendCheckpoint(&checkpoint_lsn) ||
            !log_manager_.FlushUpTo(checkpoint_lsn)) {
            throw std::runtime_error("DatabaseInstance::Checkpoint: WAL sync failed: " +
                                     log_manager_.GetLastError());
        }
    });
    if (!completed) {
        throw std::runtime_error("DatabaseInstance::Checkpoint: active transactions prevent checkpoint");
    }
}

DiskManager& DatabaseInstance::GetDiskManager() {
    return disk_manager_;
}

DatabaseHeader& DatabaseInstance::GetDatabaseHeader() {
    return database_header_;
}

LogManager& DatabaseInstance::GetLogManager() {
    return log_manager_;
}

TransactionManager& DatabaseInstance::GetTransactionManager() {
    return transaction_manager_;
}

LockManager& DatabaseInstance::GetLockManager() {
    return lock_manager_;
}

BufferPoolManager& DatabaseInstance::GetBufferPoolManager() {
    return buffer_pool_manager_;
}

Catalog& DatabaseInstance::GetCatalog() {
    return catalog_;
}

SqlExecutor& DatabaseInstance::GetExecutor() {
    return executor_;
}

}  // namespace ocera
