#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "txn/lock_manager.h"
#include "txn/txn_types.h"

namespace ocera {

// DeadlockDetector finds cycles in the lock manager's wait-for graph and
// breaks each one by choosing a victim (PROPOSED decisions P-05 / #4):
//   * detection : depth-first search for a cycle in "A waits for B" edges
//   * victim    : the YOUNGEST transaction in the cycle (highest id), since
//                 it has usually done the least work
//   * recovery  : mark the victim via LockManager::AbortTransaction. Its
//                 blocked Lock() call then returns false and the victim's
//                 own thread must call TransactionManager::Abort(), which
//                 releases its locks and lets the others continue.
//
// Can be called on demand (DetectAndResolve) or run periodically on a
// background thread (Start / Stop).
class DeadlockDetector {
public:
    explicit DeadlockDetector(LockManager& lock_manager)
        : lock_manager_(lock_manager) {}
    ~DeadlockDetector() { Stop(); }
    DeadlockDetector(const DeadlockDetector&) = delete;
    DeadlockDetector& operator=(const DeadlockDetector&) = delete;

    using Edge = std::pair<txn_id_t, txn_id_t>;  // (waiter, holder)

    // Pure functions (easy to unit-test without threads).
    // Returns the transactions forming ONE cycle, or empty if there is none.
    static std::vector<txn_id_t> FindCycle(const std::vector<Edge>& edges);
    // Youngest transaction (highest id) in the cycle.
    static txn_id_t ChooseVictim(const std::vector<txn_id_t>& cycle);

    // Look for deadlocks right now and mark one victim per cycle.
    // Returns the number of victims chosen by this call.
    std::size_t DetectAndResolve();

    // Run DetectAndResolve() every `interval` on a background thread.
    void Start(std::chrono::milliseconds interval);
    void Stop();  // safe to call more than once

    std::size_t DeadlocksResolved() const { return resolved_.load(); }

private:
    LockManager& lock_manager_;
    std::mutex detect_mutex_;                  // one detection pass at a time
    std::unordered_set<txn_id_t> pending_victims_;  // marked, not yet gone
    std::atomic<std::size_t> resolved_{0};

    std::thread thread_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    bool stop_requested_ = false;
};

}  // namespace ocera
