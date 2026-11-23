#include "txn/transaction_manager.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace ocera {

TransactionManager::TransactionManager(LockManager& lock_manager,
                                       LogManager* log_manager,
                                       UndoCallback undo_callback)
    : lock_manager_(lock_manager),
      log_manager_(log_manager),
      undo_callback_(std::move(undo_callback)) {
    if (log_manager_) {
        std::vector<LogRecord> records;
        if (!log_manager_->Scan(&records)) {
            throw std::runtime_error("TransactionManager: cannot scan WAL: " +
                                     log_manager_->GetLastError());
        }
        for (const auto& record : records) {
            if (record.txn_id >= next_id_) {
                if (record.txn_id == std::numeric_limits<txn_id_t>::max()) {
                    throw std::runtime_error("TransactionManager: transaction ID space exhausted");
                }
                next_id_ = record.txn_id + 1;
            }
        }
    }
}

txn_id_t TransactionManager::Begin() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (next_id_ == INVALID_TXN_ID ||
        next_id_ == std::numeric_limits<txn_id_t>::max()) {
        throw std::runtime_error("TransactionManager: transaction ID space exhausted");
    }
    const txn_id_t id = next_id_;
    if (log_manager_ && !log_manager_->AppendBegin(id)) {
        throw std::runtime_error("TransactionManager: WAL BEGIN failed: " +
                                 log_manager_->GetLastError());
    }
    ++next_id_;
    states_[id] = TransactionState::Active;
    return id;
}

bool TransactionManager::Finish(txn_id_t txn_id, TransactionState final_state) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = states_.find(txn_id);
        if (it == states_.end() || it->second != TransactionState::Active)
            return false;

        if (final_state == TransactionState::Aborted && log_manager_) {
            if (!undo_callback_ || !undo_callback_(txn_id)) return false;
        }

        if (log_manager_) {
            std::uint64_t lsn = 0;
            const bool appended = final_state == TransactionState::Committed
                ? log_manager_->AppendCommit(txn_id, &lsn)
                : log_manager_->AppendAbort(txn_id, &lsn);
            if (!appended || !log_manager_->FlushUpTo(lsn)) {
                // Keep the transaction active and retain its locks. A failed
                // commit/abort sync has an uncertain durable outcome.
                return false;
            }
        }

        it->second = final_state;
    }
    lock_manager_.ReleaseAllLocks(txn_id);
    return true;
}

bool TransactionManager::Commit(txn_id_t txn_id) {
    return Finish(txn_id, TransactionState::Committed);
}

bool TransactionManager::Abort(txn_id_t txn_id) {
    return Finish(txn_id, TransactionState::Aborted);
}

bool TransactionManager::Lock(txn_id_t txn_id, ResourceId resource,
                              LockMode mode) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = states_.find(txn_id);
        if (it == states_.end() || it->second != TransactionState::Active)
            return false;
    }
    return mode == LockMode::Shared
               ? lock_manager_.LockShared(txn_id, resource)
               : lock_manager_.LockExclusive(txn_id, resource);
}

bool TransactionManager::HasActiveTransactions() const {
    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto& entry : states_) {
        if (entry.second == TransactionState::Active) return true;
    }
    return false;
}

bool TransactionManager::RunWhenIdle(const std::function<void()>& action) {
    if (!action) return false;
    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto& entry : states_) {
        if (entry.second == TransactionState::Active) return false;
    }
    // Keep the lifecycle mutex held so Begin() cannot race into the
    // checkpoint after the idle check but before the checkpoint completes.
    action();
    return true;
}

bool TransactionManager::GetState(txn_id_t txn_id,
                                  TransactionState* state) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = states_.find(txn_id);
    if (it == states_.end()) return false;
    *state = it->second;
    return true;
}

}  // namespace ocera
