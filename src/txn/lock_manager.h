#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "txn/txn_types.h"

namespace ocera {

// LockManager hands out Shared (read) and Exclusive (write) locks on
// resources (pages). It follows PROPOSED decisions P-03/P-04:
//   * page-level locks
//   * Strict 2PL: locks are only given up all at once, by ReleaseAllLocks(),
//     which the Transaction Manager calls at commit or abort.
//
// Policy for conflicts: the caller BLOCKS (like waiting on a semaphore) until
// the lock can be granted, or until the transaction is told to abort
// (deadlock victim), in which case the Lock call returns false.
//
// Fairness: waiters are served in FIFO order per resource, so a stream of
// readers cannot starve a waiting writer. A lock UPGRADE (S -> X by a
// transaction that already holds S) jumps to the front of the queue.
//
// Thread-safe. One mutex protects the whole lock table (simple and correct;
// finer-grained locking is an optimisation we do not need).
class LockManager {
public:
    LockManager() = default;
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

    // Return true once the lock is held. Return false if the transaction was
    // marked aborted (see AbortTransaction) before or while waiting.
    // Re-requesting a lock you already hold (or a weaker one) returns true.
    bool LockShared(txn_id_t txn_id, ResourceId resource);
    bool LockExclusive(txn_id_t txn_id, ResourceId resource);

    // Drop every lock this transaction holds, remove any request it is still
    // waiting on, and wake up waiters. Called at commit/abort.
    void ReleaseAllLocks(txn_id_t txn_id);

    // Used by the deadlock detector: tell a (possibly blocked) transaction it
    // is a victim. Its pending/future Lock calls return false until
    // ReleaseAllLocks(txn_id) is called for it.
    void AbortTransaction(txn_id_t txn_id);

    // Snapshot of the wait-for graph: pair (a, b) means "a is waiting for b".
    std::vector<std::pair<txn_id_t, txn_id_t>> GetWaitForEdges() const;

    // Inspection helpers (mainly for tests).
    bool HoldsLock(txn_id_t txn_id, ResourceId resource, LockMode mode) const;
    std::size_t NumLocksHeld(txn_id_t txn_id) const;

private:
    struct Request {
        txn_id_t txn_id;
        LockMode mode;
    };
    struct LockState {
        std::unordered_map<txn_id_t, LockMode> holders;
        std::deque<Request> queue;  // waiting requests, FIFO
    };

    bool Acquire(txn_id_t txn_id, ResourceId resource, LockMode mode);
    // Can `txn_id` be granted `mode` right now, ignoring the queue?
    static bool CompatibleWithHolders(const LockState& s, txn_id_t txn_id,
                                      LockMode mode);
    static bool Conflicts(LockMode a, LockMode b) {
        return a == LockMode::Exclusive || b == LockMode::Exclusive;
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<ResourceId, LockState> table_;
    std::unordered_set<txn_id_t> aborted_;
};

}  // namespace ocera
