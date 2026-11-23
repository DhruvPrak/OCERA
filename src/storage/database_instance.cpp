
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
      free_space_manager_(disk_manager_),
      database_header_(disk_manager_),
      buffer_pool_manager_(
          pool_size,
          disk_manager_,
          free_space_manager_
      ),
      catalog_(),
      executor_(catalog_, buffer_pool_manager_, &database_header_) {
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

DiskManager& DatabaseInstance::GetDiskManager() {
    return disk_manager_;
}

DatabaseHeader& DatabaseInstance::GetDatabaseHeader() {
    return database_header_;
}

LogManager& DatabaseInstance::GetLogManager() {
    return log_manager_;
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
