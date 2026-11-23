#include "storage/recovery_manager.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace ocera {

RecoveryManager::RecoveryManager(DiskManager& disk_manager,
                                 LogManager& log_manager,
                                 Observer observer)
    : disk_manager_(disk_manager), log_manager_(log_manager),
      observer_(std::move(observer)) {
    Recover();
}

void RecoveryManager::Recover() {
    std::vector<LogRecord> records;
    if (!log_manager_.Scan(&records)) {
        throw std::runtime_error("RecoveryManager: WAL scan failed: " +
                                 log_manager_.GetLastError());
    }

    // Before recovery writes any data page, make the records it is replaying
    // durable. This is also important when a prior process crashed after
    // appending a complete record but before explicitly syncing the WAL.
    if (!log_manager_.FlushUpTo(log_manager_.GetLastLSN())) {
        throw std::runtime_error("RecoveryManager: WAL sync failed: " +
                                 log_manager_.GetLastError());
    }

    struct Transaction {
        bool active = false;
        bool committed = false;
        bool aborted = false;
        std::vector<const LogRecord*> updates;
    };
    std::unordered_map<std::uint64_t, Transaction> transactions;

    auto undo = [this](const std::vector<const LogRecord*>& updates) {
        for (auto it = updates.rbegin(); it != updates.rend(); ++it) {
            const LogRecord& record = **it;
            if (record.payload.size() != 2U * PAGE_SIZE ||
                record.page_id < 0) {
                throw std::runtime_error("RecoveryManager: malformed page update");
            }
            disk_manager_.WritePage(
                record.page_id,
                reinterpret_cast<const char*>(record.payload.data()));
            if (observer_) observer_(Action::UNDO, record);
        }
    };

    // The log is replayed in LSN order. An ABORT is an ordering boundary:
    // undo that transaction before applying later transactions' page images.
    // This is correct for the project's strict-2PL model, where another
    // writer cannot modify a page until the aborting transaction releases
    // its exclusive lock.
    for (const LogRecord& record : records) {
        if (record.type == LogRecordType::CHECKPOINT) {
            continue;  // Full-log replay: checkpoints do not truncate history.
        }

        if (record.type == LogRecordType::PAGE_UPDATE) {
            if (record.page_id < 0 ||
                record.page_id >= static_cast<page_id_t>(PAGE_SIZE * 8) ||
                record.payload.size() != 2U * PAGE_SIZE) {
                throw std::runtime_error("RecoveryManager: invalid PAGE_UPDATE at LSN " +
                                         std::to_string(record.lsn));
            }

            if (record.txn_id != 0) {
                auto it = transactions.find(record.txn_id);
                if (it == transactions.end() || !it->second.active ||
                    it->second.committed || it->second.aborted) {
                    throw std::runtime_error("RecoveryManager: PAGE_UPDATE outside an active transaction");
                }
                it->second.updates.push_back(&record);
            }

            // System updates (txn_id == 0) are redo-only. User transaction
            // updates are redone now and undone later if they did not commit.
            disk_manager_.WritePage(
                record.page_id,
                reinterpret_cast<const char*>(record.payload.data() + PAGE_SIZE));
            if (observer_) observer_(Action::REDO, record);
            continue;
        }

        if (record.txn_id == 0) {
            throw std::runtime_error("RecoveryManager: transaction record has invalid transaction ID 0");
        }

        Transaction& txn = transactions[record.txn_id];
        if (record.type == LogRecordType::BEGIN) {
            if (txn.active || txn.committed || txn.aborted) {
                throw std::runtime_error("RecoveryManager: duplicate BEGIN record");
            }
            txn.active = true;
        } else if (record.type == LogRecordType::COMMIT) {
            if (!txn.active || txn.committed || txn.aborted) {
                throw std::runtime_error("RecoveryManager: COMMIT without active transaction");
            }
            txn.committed = true;
            txn.active = false;
        } else if (record.type == LogRecordType::ABORT) {
            if (!txn.active || txn.committed || txn.aborted) {
                throw std::runtime_error("RecoveryManager: ABORT without active transaction");
            }
            undo(txn.updates);
            txn.aborted = true;
            txn.active = false;
        }
    }

    // Transactions still active at end-of-log are losers. Strict 2PL means
    // none of their pages can have been subsequently changed by a committed
    // transaction, so their before-images can be restored now.
    for (auto& entry : transactions) {
        Transaction& txn = entry.second;
        if (txn.active) {
            undo(txn.updates);
            // Record the recovery decision so a later restart never treats
            // this old loser as active after newer transactions have run.
            std::uint64_t abort_lsn = 0;
            if (!log_manager_.AppendAbort(entry.first, &abort_lsn) ||
                !log_manager_.FlushUpTo(abort_lsn)) {
                throw std::runtime_error("RecoveryManager: could not persist loser transaction abort: " +
                                         log_manager_.GetLastError());
            }
            txn.active = false;
            txn.aborted = true;
        }
    }

    disk_manager_.Sync();
}

}  // namespace ocera
