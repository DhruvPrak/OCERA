#include <iostream>
#include <thread>
#include <mutex>
#include <vector>

int shared_counter = 0;
std::mutex counter_mutex;

void increment_many_times(int times) {
    for (int i = 0; i < times; i++) {
        std::lock_guard<std::mutex> lock(counter_mutex);
        shared_counter++;
    }
}

int main() {
    std::cout << "OCERA environment check starting...\n";

    const int num_threads = 4;
    const int increments_per_thread = 50000;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(increment_many_times, increments_per_thread);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Final counter value: " << shared_counter << "\n";
    std::cout << "Expected value:      " << (num_threads * increments_per_thread) << "\n";

    if (shared_counter == num_threads * increments_per_thread) {
        std::cout << "Environment OK - threads + mutex working correctly.\n";
    } else {
        std::cout << "Mismatch! Something is wrong with the build/threading setup.\n";
    }

    return 0;
}
