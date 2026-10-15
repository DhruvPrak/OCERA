#include "txn/deadlock_detector.h"

#include <algorithm>
#include <map>

namespace ocera {

namespace {

enum class Color { White, Grey, Black };

// Depth-first search. A "grey" node is on the current path, so reaching a
// grey node again means we found a cycle.
bool Dfs(txn_id_t node,
         const std::map<txn_id_t, std::vector<txn_id_t>>& adj,
         std::map<txn_id_t, Color>& color, std::vector<txn_id_t>& path,
         std::vector<txn_id_t>& cycle) {
    color[node] = Color::Grey;
    path.push_back(node);
    auto it = adj.find(node);
    if (it != adj.end()) {
        for (txn_id_t next : it->second) {
            if (color[next] == Color::Grey) {
                // Cycle = the part of the path starting at `next`.
                auto start = std::find(path.begin(), path.end(), next);
                cycle.assign(start, path.end());
                return true;
            }
            if (color[next] == Color::White &&
                Dfs(next, adj, color, path, cycle))
                return true;
        }
    }
    path.pop_back();
    color[node] = Color::Black;
    return false;
}

}  // namespace

std::vector<txn_id_t> DeadlockDetector::FindCycle(
    const std::vector<Edge>& edges) {
    std::map<txn_id_t, std::vector<txn_id_t>> adj;
    std::map<txn_id_t, Color> color;
    for (const auto& [from, to] : edges) {
        adj[from].push_back(to);
        color[from] = Color::White;
        color[to] = Color::White;
    }
    std::vector<txn_id_t> path, cycle;
    for (const auto& [node, c] : color) {  // ordered -> deterministic
        (void)c;
        if (color[node] == Color::White &&
            Dfs(node, adj, color, path, cycle))
            return cycle;
    }
    return {};
}

txn_id_t DeadlockDetector::ChooseVictim(const std::vector<txn_id_t>& cycle) {
    if (cycle.empty()) return INVALID_TXN_ID;
    return *std::max_element(cycle.begin(), cycle.end());
}

std::size_t DeadlockDetector::DetectAndResolve() {
    std::lock_guard<std::mutex> guard(detect_mutex_);

    std::vector<Edge> edges = lock_manager_.GetWaitForEdges();

    // Forget victims that no longer appear in the graph (they have woken up
    // and released their locks).
    for (auto it = pending_victims_.begin(); it != pending_victims_.end();) {
        bool seen = std::any_of(edges.begin(), edges.end(), [&](const Edge& e) {
            return e.first == *it || e.second == *it;
        });
        it = seen ? std::next(it) : pending_victims_.erase(it);
    }

    std::size_t victims = 0;
    while (true) {
        // Ignore victims we already marked: they are about to disappear.
        std::vector<Edge> live;
        for (const Edge& e : edges) {
            if (!pending_victims_.count(e.first) &&
                !pending_victims_.count(e.second))
                live.push_back(e);
        }
        std::vector<txn_id_t> cycle = FindCycle(live);
        if (cycle.empty()) break;
        txn_id_t victim = ChooseVictim(cycle);
        lock_manager_.AbortTransaction(victim);
        pending_victims_.insert(victim);
        ++victims;
        ++resolved_;
    }
    return victims;
}

void DeadlockDetector::Start(std::chrono::milliseconds interval) {
    std::lock_guard<std::mutex> guard(stop_mutex_);
    if (thread_.joinable()) return;  // already running
    stop_requested_ = false;
    thread_ = std::thread([this, interval] {
        std::unique_lock<std::mutex> lock(stop_mutex_);
        while (!stop_requested_) {
            lock.unlock();
            DetectAndResolve();
            lock.lock();
            stop_cv_.wait_for(lock, interval,
                              [this] { return stop_requested_; });
        }
    });
}

void DeadlockDetector::Stop() {
    {
        std::lock_guard<std::mutex> guard(stop_mutex_);
        stop_requested_ = true;
    }
    stop_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

}  // namespace ocera
