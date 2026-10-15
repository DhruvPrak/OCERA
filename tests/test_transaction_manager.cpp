// Transaction Manager tests: lifecycle, ID uniqueness, lock release on
// commit/abort, Strict 2PL (no locks after finish), victim handling.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "txn/transaction_manager.h"

using namespace ocera;
using namespace std::chrono_literals;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::cerr << "    FAILED: " #cond " (line " << __LINE__      \
                      << ")\n";                                          \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

static void Sleep() { std::this_thread::sleep_for(150ms); }

static TransactionState StateOf(const TransactionManager& tm, txn_id_t id) {
    TransactionState s;
    CHECK(tm.GetState(id, &s));
    return s;
}

int main() {
    std::cout << "=== OCERA Transaction Manager Test ===\n\n";

    std::cout << "[1] Begin gives increasing ids starting at 1, state "
                 "Active...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        txn_id_t a = tm.Begin(), b = tm.Begin(), c = tm.Begin();
        CHECK(a == 1 && b == 2 && c == 3);
        CHECK(StateOf(tm, a) == TransactionState::Active);
        TransactionState s;
        CHECK(!tm.GetState(999, &s));  // unknown id
    }
    std::cout << "    PASSED\n";

    std::cout << "[2] Commit releases all locks...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        CHECK(tm.Lock(t1, 10, LockMode::Exclusive));
        CHECK(tm.Lock(t1, 11, LockMode::Shared));
        CHECK(lm.NumLocksHeld(t1) == 2);
        CHECK(tm.Commit(t1));
        CHECK(StateOf(tm, t1) == TransactionState::Committed);
        CHECK(lm.NumLocksHeld(t1) == 0);
        CHECK(tm.Lock(t2, 10, LockMode::Exclusive));  // now free
        CHECK(tm.Commit(t2));
    }
    std::cout << "    PASSED\n";

    std::cout << "[3] Abort releases all locks...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        CHECK(tm.Lock(t1, 10, LockMode::Exclusive));
        CHECK(tm.Abort(t1));
        CHECK(StateOf(tm, t1) == TransactionState::Aborted);
        CHECK(lm.NumLocksHeld(t1) == 0);
        CHECK(tm.Lock(t2, 10, LockMode::Exclusive));
        CHECK(tm.Commit(t2));
    }
    std::cout << "    PASSED\n";

    std::cout << "[4] Finished transactions cannot commit/abort again...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        CHECK(tm.Commit(t1));
        CHECK(!tm.Commit(t1));
        CHECK(!tm.Abort(t1));
        CHECK(tm.Abort(t2));
        CHECK(!tm.Commit(t2));
        CHECK(!tm.Commit(12345));  // unknown id
        CHECK(StateOf(tm, t1) == TransactionState::Committed);
        CHECK(StateOf(tm, t2) == TransactionState::Aborted);
    }
    std::cout << "    PASSED\n";

    std::cout << "[5] Strict 2PL: no new locks after commit or abort...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        CHECK(tm.Lock(t1, 10, LockMode::Shared));
        CHECK(tm.Commit(t1));
        CHECK(!tm.Lock(t1, 11, LockMode::Shared));
        CHECK(tm.Abort(t2));
        CHECK(!tm.Lock(t2, 11, LockMode::Exclusive));
        CHECK(lm.NumLocksHeld(t1) == 0 && lm.NumLocksHeld(t2) == 0);
    }
    std::cout << "    PASSED\n";

    std::cout << "[6] Waiting transaction proceeds when the holder "
                 "commits...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        CHECK(tm.Lock(t1, 10, LockMode::Exclusive));
        std::atomic<bool> got{false};
        std::thread t([&] { got = tm.Lock(t2, 10, LockMode::Exclusive); });
        Sleep();
        CHECK(!got);
        CHECK(tm.Commit(t1));
        t.join();
        CHECK(got);
        CHECK(tm.Commit(t2));
    }
    std::cout << "    PASSED\n";

    std::cout << "[7] Deadlock victim: Lock fails, owner aborts, other "
                 "transaction proceeds...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        CHECK(tm.Lock(t1, 10, LockMode::Exclusive));
        CHECK(tm.Lock(t2, 20, LockMode::Exclusive));
        std::atomic<int> r1{-1}, r2{-1};
        std::thread th1([&] { r1 = tm.Lock(t1, 20, LockMode::Exclusive); });
        std::thread th2([&] {
            r2 = tm.Lock(t2, 10, LockMode::Exclusive);
            if (!r2) tm.Abort(t2);  // victim's own thread cleans up
        });
        Sleep();
        lm.AbortTransaction(t2);  // what the deadlock detector will do
        th2.join();
        th1.join();
        CHECK(r2 == 0 && r1 == 1);
        CHECK(StateOf(tm, t2) == TransactionState::Aborted);
        CHECK(tm.Commit(t1));
    }
    std::cout << "    PASSED\n";

    std::cout << "[8] Abort of a transaction that is still blocked does "
                 "not crash...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        txn_id_t t1 = tm.Begin(), t2 = tm.Begin();
        CHECK(tm.Lock(t1, 10, LockMode::Exclusive));
        std::atomic<int> r{-1};
        std::thread t([&] { r = tm.Lock(t2, 10, LockMode::Exclusive); });
        Sleep();
        CHECK(tm.Abort(t2));  // called from another thread on purpose
        t.join();
        CHECK(r == 0);
        CHECK(tm.Commit(t1));
    }
    std::cout << "    PASSED\n";

    std::cout << "[9] 8 threads calling Begin() get 800 unique ids...\n";
    {
        LockManager lm;
        TransactionManager tm(lm);
        std::mutex m;
        std::set<txn_id_t> ids;
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i)
            ts.emplace_back([&] {
                for (int j = 0; j < 100; ++j) {
                    txn_id_t id = tm.Begin();
                    std::lock_guard<std::mutex> g(m);
                    ids.insert(id);
                }
            });
        for (auto& t : ts) t.join();
        CHECK(ids.size() == 800);
    }
    std::cout << "    PASSED\n";

    std::cout << "\nAll transaction manager tests passed.\n";
    return 0;
}
