#include "txn/transaction_manager.h"

namespace ocera {

txn_id_t TransactionManager::Begin() {
    std::lock_guard<std::mutex> guard(mutex_);
    txn_id_t id = next_id_++;
    states_[id] = TransactionState::Active;
    return id;
}

bool TransactionManager::Finish(txn_id_t txn_id,
                                TransactionState final_state) {
    {
        std::lock_guard<std::mutex> guard(mutex_);

        auto it = states_.find(txn_id);
        if (it == states_.end() ||
            it->second != TransactionState::Active ||
            finishing_.count(txn_id)) {
            return false;
        }

        // Prevent any new Lock() calls from starting.
        finishing_.insert(txn_id);
    }

    // Cancel pending lock requests and wake blocked Lock() calls.
    lock_manager_.AbortTransaction(txn_id);

    {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait until every Lock() call that started earlier has returned.
        cv_.wait(lock, [&]() {
            auto it = in_flight_locks_.find(txn_id);
            return it == in_flight_locks_.end() || it->second == 0;
        });
    }

    // All Lock() calls have finished. Release every held lock.
    lock_manager_.ReleaseAllLocks(txn_id);

    {
        std::lock_guard<std::mutex> guard(mutex_);

        states_[txn_id] = final_state;
        finishing_.erase(txn_id);
        in_flight_locks_.erase(txn_id);
    }

    cv_.notify_all();
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
        if (it == states_.end() ||
            it->second != TransactionState::Active ||
            finishing_.count(txn_id)) {
            return false;
        }

        // Register this request before entering the Lock Manager.
        ++in_flight_locks_[txn_id];
    }

    bool acquired = false;

    try {
        // Do not hold the Transaction Manager mutex while this may block.
        acquired = mode == LockMode::Shared
                       ? lock_manager_.LockShared(txn_id, resource)
                       : lock_manager_.LockExclusive(txn_id, resource);
    } catch (...) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto it = in_flight_locks_.find(txn_id);
            if (it != in_flight_locks_.end() && --it->second == 0) {
                in_flight_locks_.erase(it);
            }
        }
        cv_.notify_all();
        throw;
    }

    {
        std::lock_guard<std::mutex> guard(mutex_);

        auto it = in_flight_locks_.find(txn_id);
        if (it != in_flight_locks_.end() && --it->second == 0) {
            in_flight_locks_.erase(it);
        }
    }

    cv_.notify_all();
    return acquired;
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
