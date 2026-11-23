#include "storage/database_instance.h"
#include "record/table_heap.h"
#include "catalog/catalog_storage.h"

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void WriteValue(ocera::BufferPoolManager& bpm, ocera::page_id_t page_id,
                const std::string& value, std::uint64_t txn_id) {
    ocera::Page* page = bpm.FetchPage(page_id);
    Require(page != nullptr, "could not fetch page for write");
    std::memset(page->data, 0, ocera::PAGE_SIZE);
    std::memcpy(page->data, value.data(), value.size());
    Require(bpm.UnpinPage(page_id, true, txn_id), "could not unpin/log page update");
}

std::string ReadValue(ocera::BufferPoolManager& bpm, ocera::page_id_t page_id) {
    ocera::Page* page = bpm.FetchPage(page_id);
    Require(page != nullptr, "could not fetch page for read");
    const std::string value(page->data);
    Require(bpm.UnpinPage(page_id, false), "could not unpin read page");
    return value;
}
}  // namespace

int main() {
    using namespace ocera;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("ocera_wal_recovery_" + std::to_string(stamp) + ".db");
    const std::string db_file = path.string();
    const page_id_t invalid = INVALID_PAGE_ID;
    page_id_t page_id = invalid;
    page_id_t heap_first_page = invalid;
    page_id_t catalog_root_page = invalid;
    RID committed_rid;

    try {
        // A committed update must survive even when the data page was never
        // flushed: COMMIT forces all preceding WAL records to stable storage.
        {
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::CREATE_NEW);
            Page* page = db.GetBufferPoolManager().NewPage(&page_id);
            Require(page != nullptr, "could not allocate test page");
            std::memcpy(page->data, "initial", 8);
            Require(db.GetBufferPoolManager().UnpinPage(page_id, true),
                    "could not log initial page image");
            Require(CatalogStorage::Save(db.GetCatalog(),
                                         db.GetBufferPoolManager(),
                                         db.GetDatabaseHeader()),
                    "could not save test catalog metadata");
            catalog_root_page = db.GetDatabaseHeader().GetCatalogRootPageId();

            auto& tm = db.GetTransactionManager();
            const txn_id_t txn = tm.Begin();
            Require(tm.Lock(txn, page_id, LockMode::Exclusive), "could not lock page");
            WriteValue(db.GetBufferPoolManager(), page_id, "committed", txn);
            Require(tm.Commit(txn), "durable commit failed");

            Require(TableHeap::Create(db.GetBufferPoolManager(), &heap_first_page),
                    "could not create test table heap");
            TableHeap heap(db.GetBufferPoolManager(), heap_first_page);
            const txn_id_t insert_txn = tm.Begin();
            Require(tm.Lock(insert_txn, heap_first_page, LockMode::Exclusive),
                    "could not lock table page");
            Require(heap.Insert(std::vector<std::uint8_t>{'o','k'}, &committed_rid,
                                insert_txn),
                    "transactional TableHeap insert failed");
            Require(tm.Commit(insert_txn), "TableHeap insert commit failed");
        }
        {
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            Require(ReadValue(db.GetBufferPoolManager(), page_id) == "committed",
                    "recovery did not redo a committed page update");
            Require(db.GetDatabaseHeader().GetCatalogRootPageId() == catalog_root_page,
                    "recovery did not restore the WAL-protected database header");
            TableHeap heap(db.GetBufferPoolManager(), heap_first_page);
            std::vector<std::uint8_t> record;
            Require(heap.Get(committed_rid, &record) &&
                    record == std::vector<std::uint8_t>({'o','k'}),
                    "recovery did not redo a committed TableHeap insert");
        }

        // A durable PAGE_UPDATE without COMMIT is a loser and must be undone.
        {
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            auto& tm = db.GetTransactionManager();
            const txn_id_t txn = tm.Begin();
            Require(tm.Lock(txn, page_id, LockMode::Exclusive), "could not lock loser page");
            WriteValue(db.GetBufferPoolManager(), page_id, "uncommitted", txn);
            Require(db.GetBufferPoolManager().FlushPage(page_id),
                    "dirty page flush did not enforce WAL-before-data");
            TableHeap heap(db.GetBufferPoolManager(), heap_first_page);
            RID loser_rid;
            Require(tm.Lock(txn, heap_first_page, LockMode::Exclusive),
                    "could not lock table page for loser insert");
            Require(heap.Insert(std::vector<std::uint8_t>{'n','o'}, &loser_rid, txn),
                    "loser TableHeap insert failed");
            Require(db.GetBufferPoolManager().FlushPage(heap_first_page),
                    "table page flush did not enforce WAL-before-data");
            Require(db.GetLogManager().FlushUpTo(db.GetLogManager().GetLastLSN()),
                    "could not force loser WAL for crash simulation");
        }
        {
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            Require(ReadValue(db.GetBufferPoolManager(), page_id) == "committed",
                    "recovery did not undo an incomplete transaction");
            TableHeap heap(db.GetBufferPoolManager(), heap_first_page);
            std::vector<std::pair<RID, std::vector<std::uint8_t>>> rows;
            Require(heap.Scan(&rows) && rows.size() == 1 &&
                    rows.front().first == committed_rid,
                    "recovery did not undo an incomplete TableHeap insert");
        }

        // Runtime ABORT must undo while locks are retained. A later committed
        // writer to the same page must not be overwritten during restart.
        {
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            auto& tm = db.GetTransactionManager();
            const txn_id_t aborted = tm.Begin();
            Require(tm.Lock(aborted, page_id, LockMode::Exclusive), "could not lock abort page");
            WriteValue(db.GetBufferPoolManager(), page_id, "aborted", aborted);
            Require(tm.Abort(aborted), "runtime abort/undo failed");
            Require(ReadValue(db.GetBufferPoolManager(), page_id) == "committed",
                    "runtime abort did not restore the before-image");

            const txn_id_t later = tm.Begin();
            Require(tm.Lock(later, page_id, LockMode::Exclusive), "could not lock later page");
            WriteValue(db.GetBufferPoolManager(), page_id, "later-commit", later);
            Require(tm.Commit(later), "later transaction commit failed");
        }
        {
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            Require(ReadValue(db.GetBufferPoolManager(), page_id) == "later-commit",
                    "recovery reordered an aborted update after a later committed update");
        }
        {
            // Full-log recovery must be idempotent across repeated restarts.
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            Require(ReadValue(db.GetBufferPoolManager(), page_id) == "later-commit",
                    "repeated recovery changed the final page contents");
            auto& tm = db.GetTransactionManager();
            const txn_id_t active = tm.Begin();
            bool checkpoint_rejected = false;
            try { db.Checkpoint(); }
            catch (const std::runtime_error&) { checkpoint_rejected = true; }
            Require(checkpoint_rejected, "checkpoint accepted an active transaction");
            Require(tm.Abort(active), "could not finish checkpoint test transaction");
            db.Checkpoint();
        }
        {
            // Simulate a torn page-1 header write. The WAL file header
            // supplies the database identity so recovery can restore page 1.
            DiskManager raw(db_file, DatabaseOpenMode::OPEN_EXISTING);
            std::array<char, PAGE_SIZE> damaged_header{};
            raw.WritePage(HEADER_PAGE_ID, damaged_header.data());
            raw.Sync();
        }
        {
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            Require(ReadValue(db.GetBufferPoolManager(), page_id) == "later-commit",
                    "checkpoint marker changed recovery results");
            Require(db.GetDatabaseHeader().GetCatalogRootPageId() == catalog_root_page,
                    "WAL identity fallback did not recover a damaged database header");
        }

        std::filesystem::remove(db_file);
        std::filesystem::remove(db_file + ".wal");
        std::cout << "WAL recovery integration tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "WAL recovery integration test failed: " << e.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove(db_file, ignored);
        std::filesystem::remove(db_file + ".wal", ignored);
        return 1;
    }
}
