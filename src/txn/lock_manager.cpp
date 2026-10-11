#include "txn/lock_manager.h"

#include <algorithm>

namespace ocera {

bool LockManager::CompatibleWithHolders(const LockState& s, txn_id_t txn_id,
                                        LockMode mode) {
    for (const auto& [holder, held_mode] : s.holders) {
        if (holder == txn_id) continue;  // our own lock never blocks us
        if (Conflicts(held_mode, mode)) return false;
    }
    return true;
}

bool LockManager::LockShared(txn_id_t txn_id, ResourceId resource) {
    return Acquire(txn_id, resource, LockMode::Shared);
}

bool LockManager::LockExclusive(txn_id_t txn_id, ResourceId resource) {
    return Acquire(txn_id, resource, LockMode::Exclusive);
}

bool LockManager::Acquire(txn_id_t txn_id, ResourceId resource,
                          LockMode mode) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (aborted_.count(txn_id)) return false;

    LockState& state = table_[resource];

    // Already hold a lock that is strong enough?
    auto held = state.holders.find(txn_id);
    if (held != state.holders.end() &&
        (held->second == LockMode::Exclusive || mode == LockMode::Shared)) {
        return true;
    }
    bool is_upgrade = (held != state.holders.end());  // holds S, wants X

    // Fast path: nobody waiting ahead of us and no conflict -> grant now.
    if (state.queue.empty() && CompatibleWithHolders(state, txn_id, mode)) {
        state.holders[txn_id] = mode;
        return true;
    }

    // Otherwise queue up (upgrades go to the front) and wait.
    Request req{txn_id, mode};
    if (is_upgrade) state.queue.push_front(req);
    else state.queue.push_back(req);

    auto is_mine = [&](const Request& r) { return r.txn_id == txn_id; };
    while (true) {
        // table_ may rehash while we sleep, so look the state up each time.
        LockState& s = table_[resource];
        if (aborted_.count(txn_id)) {
            s.queue.erase(std::find_if(s.queue.begin(), s.queue.end(), is_mine));
            cv_.notify_all();  // others queued behind us may now proceed
            return false;
        }
        if (s.queue.front().txn_id == txn_id &&
            CompatibleWithHolders(s, txn_id, mode)) {
            s.queue.pop_front();
            s.holders[txn_id] = mode;  // also performs S -> X upgrade
            cv_.notify_all();          // next in line may be grantable too
            return true;
        }
        cv_.wait(lock);
    }
}

void LockManager::ReleaseAllLocks(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = table_.begin(); it != table_.end();) {
        LockState& s = it->second;
        s.holders.erase(txn_id);
        s.queue.erase(std::remove_if(s.queue.begin(), s.queue.end(),
                                     [&](const Request& r) {
                                         return r.txn_id == txn_id;
                                     }),
                      s.queue.end());
        if (s.holders.empty() && s.queue.empty()) it = table_.erase(it);
        else ++it;
    }
    aborted_.erase(txn_id);
    cv_.notify_all();
}

void LockManager::AbortTransaction(txn_id_t txn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_.insert(txn_id);
    cv_.notify_all();
}

std::vector<std::pair<txn_id_t, txn_id_t>> LockManager::GetWaitForEdges()
    const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<txn_id_t, txn_id_t>> edges;
    for (const auto& [resource, s] : table_) {
        (void)resource;
        for (std::size_t i = 0; i < s.queue.size(); ++i) {
            const Request& w = s.queue[i];
            // waits for conflicting current holders...
            for (const auto& [holder, mode] : s.holders) {
                if (holder != w.txn_id && Conflicts(mode, w.mode))
                    edges.emplace_back(w.txn_id, holder);
            }
            // ...and for conflicting requests queued ahead of it (FIFO).
            for (std::size_t j = 0; j < i; ++j) {
                if (s.queue[j].txn_id != w.txn_id &&
                    Conflicts(s.queue[j].mode, w.mode))
                    edges.emplace_back(w.txn_id, s.queue[j].txn_id);
            }
        }
    }
    return edges;
}

bool LockManager::HoldsLock(txn_id_t txn_id, ResourceId resource,
                            LockMode mode) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(resource);
    if (it == table_.end()) return false;
    auto h = it->second.holders.find(txn_id);
    if (h == it->second.holders.end()) return false;
    return mode == LockMode::Shared || h->second == LockMode::Exclusive;
}

std::size_t LockManager::NumLocksHeld(txn_id_t txn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (const auto& [resource, s] : table_) {
        (void)resource;
        n += s.holders.count(txn_id);
    }
    return n;
}

}  // namespace ocera
