
#include "storage/database_instance.h"

namespace ocera {

DatabaseInstance::DatabaseInstance(
    const std::string& db_file,
    std::size_t pool_size,
    DatabaseOpenMode open_mode
)
    : disk_manager_(db_file, open_mode),
      free_space_manager_(disk_manager_),
      database_header_(disk_manager_),
      buffer_pool_manager_(
          pool_size,
          disk_manager_,
          free_space_manager_
      ),
      catalog_(),
      executor_(catalog_, buffer_pool_manager_, &database_header_) {
}

void DatabaseInstance::Flush() {
    buffer_pool_manager_.FlushAllPages();
}

DiskManager& DatabaseInstance::GetDiskManager() {
    return disk_manager_;
}

DatabaseHeader& DatabaseInstance::GetDatabaseHeader() {
    return database_header_;
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
