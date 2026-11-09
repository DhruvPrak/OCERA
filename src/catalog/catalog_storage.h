#pragma once
#include "catalog/catalog.h"
#include "storage/buffer_pool_manager.h"
#include "storage/database_header.h"

namespace ocera {

// Saves and restores table definitions and their starting page numbers.
// This supports clean restarts; it is not a crash-recovery mechanism.
class CatalogStorage {
public:
    static bool Save(const Catalog& catalog, BufferPoolManager& bpm,
                     DatabaseHeader& header, std::string* error = nullptr);
    static bool Load(Catalog& catalog, BufferPoolManager& bpm,
                     const DatabaseHeader& header, std::string* error = nullptr);
};
}  // namespace ocera
