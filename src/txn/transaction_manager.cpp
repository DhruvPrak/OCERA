#include "txn/transaction_manager.h"

namespace ocera {

txn_id_t TransactionManager::Begin() {
    std::lock_guard<std::mutex> guard(mutex_);
    txn_id_t id = next_id_++;
    states_[id] = TransactionState::Active;
    return id;
}

bool TransactionManager::Finish(txn_id_t txn_id, TransactionState final_state) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = states_.find(txn_id);
        if (it == states_.end() || it->second != TransactionState::Active)
            return false;
        it->second = final_state;  // no new locks can be requested from now on
    }
    // Release outside our mutex: it may wake other threads.
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
    // May block here, so the state mutex must not be held.
    return mode == LockMode::Shared
               ? lock_manager_.LockShared(txn_id, resource)
               : lock_manager_.LockExclusive(txn_id, resource);
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
