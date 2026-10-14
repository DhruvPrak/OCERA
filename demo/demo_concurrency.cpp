// OCERA concurrency demo: two small scenarios built on the real
// LockManager / TransactionManager / DeadlockDetector.
//
// NOTE: there is no record layer or data rollback yet. The "plate counter"
// below is a plain variable protected only by an exclusive page lock, which
// is exactly what the lock manager is for.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "txn/deadlock_detector.h"
#include "txn/lock_manager.h"
#include "txn/transaction_manager.h"

using namespace ocera;
using namespace std::chrono_literals;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::cerr << "DEMO CHECK FAILED: " #cond " (line "         \
                      << __LINE__ << ")\n";                            \
            std::exit(1);                                              \
        }                                                              \
    } while (0)

static std::mutex g_log_mutex;
static void Log(const std::string& msg) {
    std::lock_guard<std::mutex> g(g_log_mutex);
    std::cout << "    " << msg << "\n";
}

// Tiny reusable "everyone wait here until N threads arrive" helper.
class Latch {
public:
    explicit Latch(int n) : remaining_(n) {}
    void ArriveAndWait() {
        std::unique_lock<std::mutex> lk(m_);
        if (--remaining_ == 0) {
            cv_.notify_all();
        } else {
            cv_.wait(lk, [this] { return remaining_ == 0; });
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    int remaining_;
};

// ---------------------------------------------------------------------
// Scenario A: two customers race for the last plate of biryani.
// ---------------------------------------------------------------------
static void ScenarioA() {
    std::cout << "=== Scenario A: the last plate of biryani ===\n";
    std::cout << "    Two customers both want the only plate. The plate\n"
                 "    counter is protected by an exclusive lock on page 10.\n";

    LockManager lm;
    TransactionManager tm(lm);
    const ResourceId kPlatePage = static_cast<ResourceId>(10);

    int plates = 1;  // only touched while holding the exclusive lock
    std::atomic<int> winners{0};
    Latch start_line(2);

    auto customer = [&](const std::string& name) {
        start_line.ArriveAndWait();  // both leave the starting line together
        txn_id_t id = tm.Begin();
        if (!tm.Lock(id, kPlatePage, LockMode::Exclusive)) {
            tm.Abort(id);
            Log(name + " could not get the lock (unexpected).");
            return;
        }
        int seen = plates;
        std::this_thread::sleep_for(100ms);  // widen the race window
        if (seen > 0) {
            plates = seen - 1;
            winners++;
            Log(name + " (txn " + std::to_string(id) + ") got the plate!");
        } else {
            Log(name + " (txn " + std::to_string(id) +
                ") found the plate already sold.");
        }
        tm.Commit(id);
    };

    std::thread a(customer, "Customer A");
    std::thread b(customer, "Customer B");
    a.join();
    b.join();

    std::cout << "    Plates left: " << plates
              << ", customers served: " << winners.load() << "\n";
    CHECK(winners.load() == 1);
    CHECK(plates == 0);
    std::cout << "    RESULT: exactly one customer got the plate.\n\n";
}

// ---------------------------------------------------------------------
// Scenario B: forced deadlock, resolved automatically.
// ---------------------------------------------------------------------
static void ScenarioB() {
    std::cout << "=== Scenario B: forced deadlock, automatic resolution ===\n";
    std::cout << "    T1 holds page 10 and wants page 20.\n"
                 "    T2 holds page 20 and wants page 10.\n";

    LockManager lm;
    TransactionManager tm(lm);
    DeadlockDetector detector(lm);
    const ResourceId kA = static_cast<ResourceId>(10);
    const ResourceId kB = static_cast<ResourceId>(20);

    txn_id_t t1 = tm.Begin();  // older (smaller id)
    txn_id_t t2 = tm.Begin();  // younger (larger id) -> expected victim
    std::cout << "    T1 = txn " << t1 << " (older), T2 = txn " << t2
              << " (younger)\n";

    detector.Start(50ms);

    Latch both_hold_first(2);
    std::atomic<int> r1{-1}, r2{-1};  // 1 = committed, 0 = aborted

    auto worker = [&](txn_id_t id, ResourceId first, ResourceId second,
                      const char* name, std::atomic<int>& result) {
        CHECK(tm.Lock(id, first, LockMode::Exclusive));
        Log(std::string(name) + " holds its first page.");
        both_hold_first.ArriveAndWait();  // now the deadlock is guaranteed
        Log(std::string(name) + " requests the other page (will block)...");
        if (tm.Lock(id, second, LockMode::Exclusive)) {
            Log(std::string(name) + " got the second page, committing.");
            tm.Commit(id);
            result = 1;
        } else {
            Log(std::string(name) +
                " was chosen as deadlock victim, aborting itself.");
            tm.Abort(id);
            result = 0;
        }
    };

    std::thread th1(worker, t1, kA, kB, "T1", std::ref(r1));
    std::thread th2(worker, t2, kB, kA, "T2", std::ref(r2));
    th1.join();
    th2.join();
    detector.Stop();

    TransactionState s1, s2;
    CHECK(tm.GetState(t1, &s1));
    CHECK(tm.GetState(t2, &s2));
    std::cout << "    Deadlocks resolved by detector: "
              << detector.DeadlocksResolved() << "\n";

    CHECK(r1.load() == 1);  // older transaction survives
    CHECK(r2.load() == 0);  // youngest is the victim
    CHECK(s1 == TransactionState::Committed);
    CHECK(s2 == TransactionState::Aborted);
    CHECK(lm.NumLocksHeld(t1) == 0);
    CHECK(lm.NumLocksHeld(t2) == 0);
    std::cout << "    RESULT: T1 committed, T2 (youngest) aborted, "
                 "no locks left held.\n\n";
}

int main() {
    std::cout << "=== OCERA Concurrency Demo ===\n\n";
    ScenarioA();
    ScenarioB();
    std::cout << "Demo finished: all checks passed.\n";
    return 0;
}
