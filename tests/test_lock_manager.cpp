// Lock Manager tests: compatibility, blocking, release, upgrade, abort,
// wait-for edges. Each test prints PASSED or exits with failure.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "txn/lock_manager.h"

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

int main() {
    std::cout << "=== OCERA Lock Manager Test ===\n\n";

    std::cout << "[1] Shared + Shared are compatible...\n";
    {
        LockManager lm;
        CHECK(lm.LockShared(1, 10));
        CHECK(lm.LockShared(2, 10));
        CHECK(lm.HoldsLock(1, 10, LockMode::Shared));
        CHECK(lm.HoldsLock(2, 10, LockMode::Shared));
        lm.ReleaseAllLocks(1);
        lm.ReleaseAllLocks(2);
    }
    std::cout << "    PASSED\n";

    std::cout << "[2] Exclusive blocks Shared until released...\n";
    {
        LockManager lm;
        CHECK(lm.LockExclusive(1, 10));
        std::atomic<bool> got{false};
        std::thread t([&] { got = lm.LockShared(2, 10); });
        Sleep();
        CHECK(!got);  // still blocked
        lm.ReleaseAllLocks(1);
        t.join();
        CHECK(got);
        CHECK(lm.HoldsLock(2, 10, LockMode::Shared));
        lm.ReleaseAllLocks(2);
    }
    std::cout << "    PASSED\n";

    std::cout << "[3] Shared blocks Exclusive until released...\n";
    {
        LockManager lm;
        CHECK(lm.LockShared(1, 10));
        std::atomic<bool> got{false};
        std::thread t([&] { got = lm.LockExclusive(2, 10); });
        Sleep();
        CHECK(!got);
        lm.ReleaseAllLocks(1);
        t.join();
        CHECK(got);
        lm.ReleaseAllLocks(2);
    }
    std::cout << "    PASSED\n";

    std::cout << "[4] Re-requesting a held lock succeeds; S->X upgrade "
                 "when sole holder...\n";
    {
        LockManager lm;
        CHECK(lm.LockShared(1, 10));
        CHECK(lm.LockShared(1, 10));
        CHECK(lm.LockExclusive(1, 10));  // upgrade, nobody else holds it
        CHECK(lm.HoldsLock(1, 10, LockMode::Exclusive));
        CHECK(lm.LockShared(1, 10));     // weaker request still fine
        CHECK(lm.NumLocksHeld(1) == 1);
        lm.ReleaseAllLocks(1);
    }
    std::cout << "    PASSED\n";

    std::cout << "[5] Upgrade waits for the other reader to leave...\n";
    {
        LockManager lm;
        CHECK(lm.LockShared(1, 10));
        CHECK(lm.LockShared(2, 10));
        std::atomic<bool> got{false};
        std::thread t([&] { got = lm.LockExclusive(1, 10); });
        Sleep();
        CHECK(!got);
        lm.ReleaseAllLocks(2);
        t.join();
        CHECK(got);
        lm.ReleaseAllLocks(1);
    }
    std::cout << "    PASSED\n";

    std::cout << "[6] ReleaseAllLocks drops every lock (Strict 2PL "
                 "commit/abort)...\n";
    {
        LockManager lm;
        CHECK(lm.LockExclusive(1, 10));
        CHECK(lm.LockShared(1, 11));
        CHECK(lm.LockExclusive(1, 12));
        CHECK(lm.NumLocksHeld(1) == 3);
        lm.ReleaseAllLocks(1);
        CHECK(lm.NumLocksHeld(1) == 0);
        CHECK(lm.LockExclusive(2, 10));  // free again
        lm.ReleaseAllLocks(2);
    }
    std::cout << "    PASSED\n";

    std::cout << "[7] Aborting a blocked transaction makes its Lock call "
                 "return false...\n";
    {
        LockManager lm;
        CHECK(lm.LockExclusive(1, 10));
        std::atomic<int> result{-1};
        std::thread t([&] { result = lm.LockExclusive(2, 10) ? 1 : 0; });
        Sleep();
        CHECK(result == -1);
        lm.AbortTransaction(2);
        t.join();
        CHECK(result == 0);
        CHECK(!lm.LockShared(2, 11));  // still marked aborted
        lm.ReleaseAllLocks(2);
        CHECK(lm.LockShared(2, 11));   // cleared after release
        lm.ReleaseAllLocks(2);
        lm.ReleaseAllLocks(1);
    }
    std::cout << "    PASSED\n";

    std::cout << "[8] Wait-for graph shows a deadlock cycle "
                 "(T1->T2, T2->T1)...\n";
    {
        LockManager lm;
        CHECK(lm.LockExclusive(1, 10));
        CHECK(lm.LockExclusive(2, 20));
        std::atomic<int> r1{-1}, r2{-1};
        std::thread t1([&] { r1 = lm.LockExclusive(1, 20) ? 1 : 0; });
        std::thread t2([&] { r2 = lm.LockExclusive(2, 10) ? 1 : 0; });
        Sleep();
        auto edges = lm.GetWaitForEdges();
        bool e12 = false, e21 = false;
        for (auto& [a, b] : edges) {
            if (a == 1 && b == 2) e12 = true;
            if (a == 2 && b == 1) e21 = true;
        }
        CHECK(e12 && e21);
        lm.AbortTransaction(2);  // pretend the detector picked T2
        t2.join();
        CHECK(r2 == 0);
        lm.ReleaseAllLocks(2);   // victim's locks released -> T1 proceeds
        t1.join();
        CHECK(r1 == 1);
        lm.ReleaseAllLocks(1);
    }
    std::cout << "    PASSED\n";

    std::cout << "[9] Stress: 8 threads incrementing a shared counter "
                 "under an X lock...\n";
    {
        LockManager lm;
        int counter = 0;  // deliberately unprotected except by the lock
        std::vector<std::thread> ts;
        for (txn_id_t id = 1; id <= 8; ++id) {
            ts.emplace_back([&, id] {
                for (int i = 0; i < 500; ++i) {
                    CHECK(lm.LockExclusive(id, 7));
                    ++counter;
                    lm.ReleaseAllLocks(id);
                }
            });
        }
        for (auto& t : ts) t.join();
        CHECK(counter == 8 * 500);
    }
    std::cout << "    PASSED: no lost updates\n";

    std::cout << "\nAll lock manager tests passed.\n";
    return 0;
}
