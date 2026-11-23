#pragma once
#include <mutex>
#include <unordered_map>
#include <functional>
#include "../wal/log_manager.h"

#include "txn/lock_manager.h"
#include "txn/txn_types.h"

namespace ocera {

enum class TransactionState { Active, Committed, Aborted };

// TransactionManager owns the transaction lifecycle: Begin -> Commit/Abort.
// It enforces Strict 2PL together with the LockManager:
//   * growing phase  = while Active, locks may be acquired (via Lock()).
//   * shrinking      = Commit()/Abort() release ALL locks at once; after
//                      that the transaction can never acquire a lock again.
//
// When configured with a LogManager and undo callback, transaction lifecycle
// records are durable: COMMIT is synchronized before lock release, and ABORT
// restores before-images and synchronizes its record before lock release.
// Threading rule: Commit()/Abort() for a transaction must be called by the
// thread that runs it (not from the deadlock detector). The detector only
// *marks* a victim through LockManager::AbortTransaction; the victim's own
// thread sees its Lock() fail and then calls Abort().
class TransactionManager {
public:
    using UndoCallback = std::function<bool(txn_id_t)>;

    explicit TransactionManager(LockManager& lock_manager,
                                LogManager* log_manager = nullptr,
                                UndoCallback undo_callback = {});
    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    // Start a new transaction. IDs are unique, increasing, and start at 1.
    // Higher ID = younger transaction (used for deadlock victim choice).
    txn_id_t Begin();

    // Returns true if the transaction was Active and is now finished.
    // Returns false (and does nothing) for unknown or already-finished ids.
    bool Commit(txn_id_t txn_id);
    bool Abort(txn_id_t txn_id);

    // Acquire a lock for an Active transaction. Returns false if the
    // transaction is not Active (2PL: no locks after release) or if it was
    // chosen as a deadlock victim while waiting; in the latter case the
    // caller should call Abort(txn_id).
    bool Lock(txn_id_t txn_id, ResourceId resource, LockMode mode);

    // Returns false if the id is unknown.
    bool GetState(txn_id_t txn_id, TransactionState* state) const;
    bool HasActiveTransactions() const;
    bool RunWhenIdle(const std::function<void()>& action);

private:
    bool Finish(txn_id_t txn_id, TransactionState final_state);

    LockManager& lock_manager_;
    LogManager* log_manager_;
    UndoCallback undo_callback_;
    mutable std::mutex mutex_;
    txn_id_t next_id_ = 1;
    std::unordered_map<txn_id_t, TransactionState> states_;
};

}  // namespace ocera
