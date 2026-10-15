// Deadlock detector tests: pure cycle finding, victim choice, and real
// multi-threaded deadlocks resolved on demand and by the background thread.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "txn/deadlock_detector.h"
#include "txn/transaction_manager.h"

using namespace ocera;
using namespace std::chrono_literals;
using Edge = DeadlockDetector::Edge;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::cerr << "    FAILED: " #cond " (line " << __LINE__      \
                      << ")\n";                                          \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

static void Sleep(int ms = 150) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// A worker: lock `first`, then (after a pause) lock `second`. If the second
// lock fails (we were chosen as victim) abort ourselves; otherwise commit.
struct Worker {
    std::thread th;
    std::atomic<int> result{-1};  // 1 = committed, 0 = aborted as victim
    void Run(TransactionManager& tm, txn_id_t id, ResourceId first,
             ResourceId second) {
        th = std::thread([&tm, id, first, second, this] {
            if (!tm.Lock(id, first, LockMode::Exclusive)) { tm.Abort(id); result = 0; return; }
            std::this_thread::sleep_for(100ms);  // let the others grab theirs
            if (tm.Lock(id, second, LockMode::Exclusive)) {
                tm.Commit(id);
                result = 1;
            } else {
                tm.Abort(id);
                result = 0;
            }
        });
    }
};

int main() {
    std::cout << "=== OCERA Deadlock Detector Test ===\n\n";

    std::cout << "[1] FindCycle: no edges / no cycle -> empty...\n";
    CHECK(DeadlockDetector::FindCycle({}).empty());
    CHECK(DeadlockDetector::FindCycle({{1, 2}, {2, 3}, {1, 3}}).empty());
    std::cout << "    PASSED\n";

    std::cout << "[2] FindCycle: 2-cycle and 3-cycle...\n";
    {
        auto c2 = DeadlockDetector::FindCycle({{1, 2}, {2, 1}});
        std::sort(c2.begin(), c2.end());
        CHECK((c2 == std::vector<txn_id_t>{1, 2}));
        auto c3 = DeadlockDetector::FindCycle({{1, 2}, {2, 3}, {3, 1}});
        std::sort(c3.begin(), c3.end());
        CHECK((c3 == std::vector<txn_id_t>{1, 2, 3}));
    }
    std::cout << "    PASSED\n";

    std::cout << "[3] FindCycle: cycle reached through a tail (5->1, "
                 "1->2, 2->1) excludes txn 5...\n";
    {
        auto c = DeadlockDetector::FindCycle({{5, 1}, {1, 2}, {2, 1}});
        std::sort(c.begin(), c.end());
        CHECK((c == std::vector<txn_id_t>{1, 2}));
    }
    std::cout << "    PASSED\n";

    std::cout << "[4] ChooseVictim picks the youngest (highest id)...\n";
    CHECK(DeadlockDetector::ChooseVictim({3, 9, 4}) == 9);
    CHECK(DeadlockDetector::ChooseVictim({}) == INVALID_TXN_ID);
    std::cout << "    PASSED\n";

    std::cout << "[5] No deadlock: detector chooses no victim, both "
                 "transactions commit...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        DeadlockDetector dd(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        CHECK(tm.Lock(t1, 10, LockMode::Exclusive));
        std::atomic<bool> got{false};
        std::thread t([&] { got = tm.Lock(t2, 10, LockMode::Exclusive); });
        Sleep();
        CHECK(dd.DetectAndResolve() == 0);  // t2 just waits, no cycle
        CHECK(tm.Commit(t1));
        t.join();
        CHECK(got);
        CHECK(tm.Commit(t2));
        CHECK(dd.DeadlocksResolved() == 0);
    }
    std::cout << "    PASSED\n";

    std::cout << "[6] Real 2-transaction deadlock: youngest is the "
                 "victim, the older one commits...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        DeadlockDetector dd(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        Worker w1, w2;
        w1.Run(tm, t1, 10, 20);
        w2.Run(tm, t2, 20, 10);
        Sleep(400);  // both now blocked on each other
        CHECK(dd.DetectAndResolve() == 1);
        w1.th.join();
        w2.th.join();
        CHECK(w1.result == 1);  // older commits
        CHECK(w2.result == 0);  // youngest aborted
        TransactionState s;
        CHECK(tm.GetState(t2, &s) && s == TransactionState::Aborted);
        CHECK(tm.GetState(t1, &s) && s == TransactionState::Committed);
        CHECK(dd.DeadlocksResolved() == 1);
    }
    std::cout << "    PASSED\n";

    std::cout << "[7] Real 3-transaction deadlock cycle...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        DeadlockDetector dd(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin(), t3 = tm.Begin();
        Worker w1, w2, w3;
        w1.Run(tm, t1, 10, 20);
        w2.Run(tm, t2, 20, 30);
        w3.Run(tm, t3, 30, 10);
        Sleep(400);
        CHECK(dd.DetectAndResolve() == 1);
        w1.th.join(); w2.th.join(); w3.th.join();
        CHECK(w3.result == 0);              // youngest (id 3) is the victim
        CHECK(w1.result == 1 && w2.result == 1);
    }
    std::cout << "    PASSED\n";

    std::cout << "[8] Two separate deadlocks resolved in a single "
                 "pass...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        DeadlockDetector dd(lm);
        txn_id_t a1 = tm.Begin(), a2 = tm.Begin(), b1 = tm.Begin(),
                 b2 = tm.Begin();
        Worker wa1, wa2, wb1, wb2;
        wa1.Run(tm, a1, 10, 20); wa2.Run(tm, a2, 20, 10);
        wb1.Run(tm, b1, 30, 40); wb2.Run(tm, b2, 40, 30);
        Sleep(400);
        CHECK(dd.DetectAndResolve() == 2);
        wa1.th.join(); wa2.th.join(); wb1.th.join(); wb2.th.join();
        CHECK(wa1.result == 1 && wa2.result == 0);
        CHECK(wb1.result == 1 && wb2.result == 0);
    }
    std::cout << "    PASSED\n";

    std::cout << "[9] Background detector resolves a deadlock "
                 "automatically...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        DeadlockDetector dd(lm);
        dd.Start(50ms);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        Worker w1, w2;
        w1.Run(tm, t1, 10, 20);
        w2.Run(tm, t2, 20, 10);
        w1.th.join();  // would hang forever without the detector
        w2.th.join();
        CHECK(w1.result == 1 && w2.result == 0);
        dd.Stop();
        dd.Stop();  // calling twice is fine
        CHECK(dd.DeadlocksResolved() == 1);
    }
    std::cout << "    PASSED\n";

    std::cout << "[10] Stress: 6 threads, random lock order over 4 pages, "
                 "background detector; everyone finishes...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        DeadlockDetector dd(lm);
        dd.Start(20ms);
        std::atomic<int> committed{0}, aborted{0};
        std::vector<std::thread> ts;
        for (int w = 0; w < 6; ++w) {
            ts.emplace_back([&, w] {
                std::mt19937 rng(1234 + w);  // portable (rand_r is POSIX-only)
                for (int i = 0; i < 40; ++i) {
                    txn_id_t id = tm.Begin();
                    bool ok = true;
                    for (int k = 0; k < 3 && ok; ++k) {
                        ResourceId r = static_cast<ResourceId>(rng() % 4);
                        LockMode m = (rng() % 2) ? LockMode::Exclusive
                                                         : LockMode::Shared;
                        ok = tm.Lock(id, r, m);
                        std::this_thread::sleep_for(2ms);  // widen the race
                    }
                    if (ok) { tm.Commit(id); ++committed; }
                    else    { tm.Abort(id);  ++aborted; }
                }
            });
        }
        for (auto& t : ts) t.join();
        dd.Stop();
        CHECK(committed + aborted == 6 * 40);
        CHECK(aborted == (int)dd.DeadlocksResolved());  // every abort = one resolved cycle
        std::cout << "    committed=" << committed << " aborted(victims)="
                  << aborted << " deadlocks_resolved="
                  << dd.DeadlocksResolved() << "\n";
    }
    std::cout << "    PASSED: no hangs, every transaction finished\n";

    std::cout << "\nAll deadlock detector tests passed.\n";
    return 0;
}
