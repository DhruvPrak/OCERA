#include "storage/database_instance.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace ocera {
namespace {
constexpr std::size_t kCatalogPageHeaderSize = 8;
constexpr std::size_t kCatalogPagePayload = PAGE_SIZE - kCatalogPageHeaderSize;
void PutU32(char* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<char>((v >> (i * 8)) & 0xff);
}
std::uint32_t GetU32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (i * 8);
    return v;
}
}

DatabaseInstance::DatabaseInstance(const std::string& db_file, std::size_t pool_size,
                                   DatabaseOpenMode open_mode)
    : disk_manager_(db_file, open_mode),
      free_space_manager_(disk_manager_),
      database_header_(disk_manager_),
      buffer_pool_manager_(pool_size, disk_manager_, free_space_manager_),
      catalog_(),
      executor_(catalog_, buffer_pool_manager_) {
    LoadCatalog();
}

std::vector<page_id_t> DatabaseInstance::ReadCatalogPageChain() const {
    std::vector<page_id_t> ids;
    page_id_t current = database_header_.GetCatalogRootPageId();
    std::unordered_set<page_id_t> seen;
    while (current != INVALID_PAGE_ID) {
        if (current < 2 || current >= disk_manager_.GetNumPages() || !seen.insert(current).second)
            throw std::runtime_error("DatabaseInstance: invalid or cyclic Catalog page chain");
        ids.push_back(current);
        Page* page = const_cast<BufferPoolManager&>(buffer_pool_manager_).FetchPage(current);
        if (!page) throw std::runtime_error("DatabaseInstance: unable to fetch Catalog page");
        const std::uint32_t used = GetU32(page->data + 4);
        const std::uint32_t next = GetU32(page->data);
        const bool valid = used <= kCatalogPagePayload;
        const page_id_t next_id = static_cast<page_id_t>(next);
        const bool unpinned = const_cast<BufferPoolManager&>(buffer_pool_manager_).UnpinPage(current, false);
        if (!unpinned || !valid) throw std::runtime_error("DatabaseInstance: corrupt Catalog page");
        current = next_id;
    }
    return ids;
}

void DatabaseInstance::LoadCatalog() {
    const page_id_t root = database_header_.GetCatalogRootPageId();
    if (root == INVALID_PAGE_ID) return;
    std::vector<std::uint8_t> bytes;
    const auto ids = ReadCatalogPageChain();
    for (page_id_t id : ids) {
        Page* page = buffer_pool_manager_.FetchPage(id);
        if (!page) throw std::runtime_error("DatabaseInstance: unable to fetch Catalog page");
        const std::uint32_t used = GetU32(page->data + 4);
        if (used > kCatalogPagePayload) {
            buffer_pool_manager_.UnpinPage(id, false);
            throw std::runtime_error("DatabaseInstance: corrupt Catalog payload length");
        }
        bytes.insert(bytes.end(), reinterpret_cast<std::uint8_t*>(page->data + kCatalogPageHeaderSize),
                     reinterpret_cast<std::uint8_t*>(page->data + kCatalogPageHeaderSize + used));
        buffer_pool_manager_.UnpinPage(id, false);
    }
        // Restore persisted table and index metadata first.
    catalog_.Deserialize(bytes);

    // Rebuild runtime TableHeap and RecordTable objects.
    std::string restore_error;

    if (!executor_.RestoreTables(&restore_error)) {
        throw std::runtime_error(
            "DatabaseInstance: failed to restore table runtime: " +
            restore_error
        );
    }
}

void DatabaseInstance::SaveCatalog() {
    const auto old_pages = ReadCatalogPageChain();
    const std::vector<std::uint8_t> bytes = catalog_.Serialize();
    if (bytes.empty()) throw std::runtime_error("DatabaseInstance: empty Catalog image");
    const std::size_t count = (bytes.size() + kCatalogPagePayload - 1) / kCatalogPagePayload;
    std::vector<page_id_t> new_pages;
    new_pages.reserve(count);
    try {
        for (std::size_t i = 0; i < count; ++i) {
            page_id_t id = INVALID_PAGE_ID;
            Page* fresh = buffer_pool_manager_.NewPage(&id);
            if (!fresh) throw std::runtime_error("DatabaseInstance: buffer pool cannot allocate Catalog page");
            new_pages.push_back(id);
            if (!buffer_pool_manager_.UnpinPage(id, false))
                throw std::runtime_error("DatabaseInstance: failed to unpin allocated Catalog page");
        }
        std::size_t offset = 0;
        for (std::size_t i = 0; i < count; ++i) {
            Page* page = buffer_pool_manager_.FetchPage(new_pages[i]);
            if (!page) throw std::runtime_error("DatabaseInstance: unable to fetch new Catalog page");
            std::memset(page->data, 0, PAGE_SIZE);
            const std::size_t used = std::min(kCatalogPagePayload, bytes.size() - offset);
            const page_id_t next = (i + 1 < count) ? new_pages[i + 1] : INVALID_PAGE_ID;
            PutU32(page->data, static_cast<std::uint32_t>(next));
            PutU32(page->data + 4, static_cast<std::uint32_t>(used));
            std::memcpy(page->data + kCatalogPageHeaderSize, bytes.data() + offset, used);
            offset += used;
            if (!buffer_pool_manager_.UnpinPage(new_pages[i], true))
                throw std::runtime_error("DatabaseInstance: failed to unpin Catalog page");
        }
        // Write all Catalog pages before publishing the new root in page 1.
        buffer_pool_manager_.FlushAllPages();
        database_header_.SetCatalogRootPageId(new_pages.front());
        // The header now points to the new complete chain; old pages can be reclaimed.
        for (page_id_t id : old_pages) buffer_pool_manager_.DeletePage(id);
    } catch (...) {
        // Only reclaim pages that were allocated for this attempt and are not published.
        if (database_header_.GetCatalogRootPageId() != (new_pages.empty() ? INVALID_PAGE_ID : new_pages.front())) {
            for (page_id_t id : new_pages) buffer_pool_manager_.DeletePage(id);
        }
        throw;
    }
}

void DatabaseInstance::Flush() {
    SaveCatalog();
    buffer_pool_manager_.FlushAllPages();
}

DiskManager& DatabaseInstance::GetDiskManager() { return disk_manager_; }
DatabaseHeader& DatabaseInstance::GetDatabaseHeader() { return database_header_; }
BufferPoolManager& DatabaseInstance::GetBufferPoolManager() { return buffer_pool_manager_; }
Catalog& DatabaseInstance::GetCatalog() { return catalog_; }
SqlExecutor& DatabaseInstance::GetExecutor() { return executor_; }

}  // namespace ocera
