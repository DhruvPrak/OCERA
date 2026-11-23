#include "storage/database_instance.h"
#include "record/table_heap.h"
#include "catalog/catalog_storage.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef CREATE_NEW
#undef OPEN_EXISTING
#endif

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

int RunInterruptedRecoveryChild(const std::string& action,
                                const std::string& db_file,
                                ocera::page_id_t target_page,
                                std::uint64_t target_txn) {
    using namespace ocera;
    DiskManager disk(db_file, DatabaseOpenMode::OPEN_EXISTING);
    const auto bootstrap = DatabaseHeader::Bootstrap(disk, db_file, db_file + ".wal");
    LogManager log(db_file + ".wal", bootstrap.database_id,
                   bootstrap.allow_wal_creation, bootstrap.allow_wal_repair);

    if (action == "runtime-abort") {
        // Execute the same BufferPool undo callback used by TransactionManager,
        // then die before an ABORT record can be appended or synchronized.
        FreeSpaceManager free_space(disk, &log);
        BufferPoolManager buffer_pool(4, disk, free_space, &log);
        if (!buffer_pool.UndoTransaction(target_txn)) return 1;
        std::_Exit(74);
    }

    const auto wanted_action = action == "redo"
        ? RecoveryManager::Action::REDO : RecoveryManager::Action::UNDO;
    RecoveryManager recovery(disk, log,
        [=](RecoveryManager::Action observed, const LogRecord& record) {
            if (observed == wanted_action && record.page_id == target_page &&
                record.txn_id == target_txn) {
                // Simulate abrupt process termination after the selected page
                // image has been applied, without running C++ destructors.
                std::_Exit(73);
            }
        });
    return 0;
}

std::string QuoteArgument(const std::string& value) {
    return "\"" + value + "\"";
}

int LaunchInterruptedRecoveryChild(const std::string& executable,
                                   const std::string& action,
                                   const std::string& db_file,
                                   ocera::page_id_t page_id,
                                   std::uint64_t txn_id) {
    const std::string command_line = QuoteArgument(executable) +
        " --interrupt-recovery-child " + action + " " +
        QuoteArgument(db_file) + " " + std::to_string(page_id) + " " +
        std::to_string(txn_id);
#ifdef _WIN32
    STARTUPINFOA startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back('\0');
    const BOOL created = CreateProcessA(executable.c_str(), mutable_command_line.data(),
        nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup_info, &process_info);
    Require(created != 0, "CreateProcessA failed for interrupted recovery child: " +
            std::to_string(GetLastError()));
    const DWORD wait_result = WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 1;
    const BOOL got_exit_code = wait_result == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    Require(got_exit_code != 0, "could not read interrupted recovery child exit code");
    return static_cast<int>(exit_code);
#else
    const int status = std::system(command_line.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#endif
}

void RunInterruptedRecoveryScenario(const std::string& executable,
                                    const std::string& action) {
    using namespace ocera;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("ocera_interrupted_recovery_" + action + "_" +
         std::to_string(stamp) + ".db");
    const std::string db_file = path.string();
    page_id_t page_id = INVALID_PAGE_ID;
    txn_id_t committed_txn = 0;
    txn_id_t loser_txn = 0;

    try {
        {
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::CREATE_NEW);
            Page* page = db.GetBufferPoolManager().NewPage(&page_id);
            Require(page != nullptr, "could not allocate interrupted-recovery test page");
            std::memcpy(page->data, "initial", 8);
            Require(db.GetBufferPoolManager().UnpinPage(page_id, true),
                    "could not log interrupted-recovery baseline page");
            Require(db.GetBufferPoolManager().FlushPage(page_id),
                    "could not flush interrupted-recovery baseline page");

            auto& tm = db.GetTransactionManager();
            committed_txn = tm.Begin();
            Require(tm.Lock(committed_txn, page_id, LockMode::Exclusive),
                    "could not lock page for committed recovery update");
            WriteValue(db.GetBufferPoolManager(), page_id, "committed", committed_txn);
            Require(tm.Commit(committed_txn), "could not commit recovery fixture update");

            loser_txn = tm.Begin();
            Require(tm.Lock(loser_txn, page_id, LockMode::Exclusive),
                    "could not lock page for loser recovery update");
            WriteValue(db.GetBufferPoolManager(), page_id, "loser-first", loser_txn);
            WriteValue(db.GetBufferPoolManager(), page_id, "loser", loser_txn);
            Require(db.GetBufferPoolManager().FlushPage(page_id),
                    "could not flush loser page before interrupted recovery");
            Require(db.GetLogManager().FlushUpTo(db.GetLogManager().GetLastLSN()),
                    "could not force loser WAL before interrupted recovery");
        }

        const int child_status = LaunchInterruptedRecoveryChild(
            executable, action, db_file, page_id,
            action == "redo" ? committed_txn : loser_txn);
        const int expected_child_status = action == "runtime-abort" ? 74 : 73;
        Require(child_status == expected_child_status,
                "child did not stop at the requested " + action + " operation; exit=" +
                std::to_string(child_status));

        if (action == "runtime-abort") {
            DiskManager raw(db_file, DatabaseOpenMode::OPEN_EXISTING);
            std::array<char, PAGE_SIZE> page_bytes{};
            raw.ReadPage(page_id, page_bytes.data());
            Require(std::string(page_bytes.data()) == "committed",
                    "runtime undo did not restore the page before the simulated crash");
        }

        {
            DatabaseInstance recovered(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            Require(ReadValue(recovered.GetBufferPoolManager(), page_id) == "committed",
                    "recovery after interrupted " + action + " did not restore committed state");
        }
        {
            DatabaseInstance recovered_again(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            Require(ReadValue(recovered_again.GetBufferPoolManager(), page_id) == "committed",
                    "repeated recovery after interrupted " + action + " was not idempotent");
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(db_file, ignored);
        std::filesystem::remove(db_file + ".wal", ignored);
        throw;
    }

    std::error_code ignored;
    std::filesystem::remove(db_file, ignored);
    std::filesystem::remove(db_file + ".wal", ignored);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 6 && std::string(argv[1]) == "--interrupt-recovery-child") {
        return RunInterruptedRecoveryChild(argv[2], argv[3],
            static_cast<ocera::page_id_t>(std::stoi(argv[4])),
            static_cast<std::uint64_t>(std::stoull(argv[5])));
    }

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
        {
            // Simulate a torn/corrupted data-page write after a durable commit.
            // Full-page redo must restore the committed after-image on reopen.
            DiskManager raw(db_file, DatabaseOpenMode::OPEN_EXISTING);
            std::array<char, PAGE_SIZE> damaged_page{};
            std::memset(damaged_page.data(), 0xA5, damaged_page.size());
            raw.WritePage(page_id, damaged_page.data());
            raw.Sync();
        }
        {
            DatabaseInstance db(db_file, 4, DatabaseOpenMode::OPEN_EXISTING);
            Require(ReadValue(db.GetBufferPoolManager(), page_id) == "committed",
                    "recovery did not repair a corrupted committed data page");
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
            const auto wal_size_before_checkpoint =
                std::filesystem::file_size(db_file + ".wal");
            db.Checkpoint();
            Require(std::filesystem::file_size(db_file + ".wal") >
                        wal_size_before_checkpoint,
                    "checkpoint did not append its durable marker to the retained WAL");
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

        const std::string executable =
            std::filesystem::absolute(std::filesystem::path(argv[0])).string();
        RunInterruptedRecoveryScenario(executable, "redo");
        RunInterruptedRecoveryScenario(executable, "undo");
        RunInterruptedRecoveryScenario(executable, "runtime-abort");

        std::cout << "WAL recovery integration and interrupted-recovery tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "WAL recovery integration test failed: " << e.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove(db_file, ignored);
        std::filesystem::remove(db_file + ".wal", ignored);
        return 1;
    }
}
