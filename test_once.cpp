// Stress test for CGAL::internal::call_once on CONTENDED flags.
//
// The benchmark never has two threads on the same flag, so it says nothing
// about the waiting side of the implementation.  Here all threads walk the
// same array of flags at the same time and check the contract of
// [thread.once.callonce]:
//
//   1. the function returns normally exactly once per flag, and every caller
//      sees its side effects afterwards (a plain, non-atomic read: under
//      ThreadSanitizer this is what verifies the memory orders);
//   2. if the function throws, the exception reaches that caller, the flag
//      stays unset and another call runs the function again.
//
// Built twice: as C++20 (waiters block in std::atomic::wait) and as C++17
// (waiters spin with yield).

#include <CGAL/STL_Extension/internal/once.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using CGAL::internal::call_once;
using CGAL::internal::once_flag;

struct Cell {
    once_flag flag;
    int value = 0;               // written by the function, read by everyone
    std::atomic<int> returns{0}; // normal returns of the function
    std::atomic<int> throws{0};  // exceptional exits of the function
};

template <class Body>
void runThreads(unsigned threads, Body body) {
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    for(unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            while(!go.load(std::memory_order_acquire)) std::this_thread::yield();
            body(t);
        });
    go.store(true, std::memory_order_release);
    for(std::thread& thread: pool) thread.join();
}

// Every 16th function yields in the middle, so that other threads reliably
// arrive while it is running and have to take the waiting path.
void dawdle(std::size_t index) {
    if(index % 16 == 0) std::this_thread::yield();
}

bool testExactlyOnce(unsigned threads, std::size_t count) {
    std::vector<Cell> cells(count);
    std::atomic<bool> ok{true};
    runThreads(threads, [&](unsigned) {
        for(std::size_t i = 0; i < count; ++i) {
            Cell& cell = cells[i];
            call_once(cell.flag, [&] {
                dawdle(i);
                cell.value = 42;
                ++cell.returns;
            });
            if(cell.value != 42) ok = false;
        }
    });
    for(const Cell& cell: cells)
        if(cell.returns != 1) ok = false;
    return ok;
}

bool testExceptions(unsigned threads, std::size_t count) {
    const int failures = 3; // the first three executions for each flag throw
    std::vector<Cell> cells(count);
    std::atomic<bool> ok{true};
    runThreads(threads, [&](unsigned) {
        for(std::size_t i = 0; i < count; ++i) {
            Cell& cell = cells[i];
            for(;;) {
                try {
                    call_once(cell.flag, [&] {
                        dawdle(i);
                        // Executions for one flag never overlap, so this
                        // check-then-increment cannot be raced.
                        if(cell.throws < failures) {
                            ++cell.throws;
                            throw std::runtime_error("not yet");
                        }
                        cell.value = 42;
                        ++cell.returns;
                    });
                    break;
                } catch(const std::runtime_error&) {
                    // This thread ran the function and it threw: try again.
                }
            }
            if(cell.value != 42) ok = false;
        }
    });
    for(const Cell& cell: cells)
        if(cell.returns != 1 || cell.throws != failures) ok = false;
    return ok;
}

} // namespace

int main() {
    const unsigned threads = std::max(std::thread::hardware_concurrency(), 4u);
    const std::size_t count = 20000;
    bool ok = true;
    for(int round = 0; round < 5; ++round) {
        ok = testExactlyOnce(threads, count) && ok;
        ok = testExceptions(threads, count) && ok;
    }
    std::printf("test_once: %u threads, %zu flags, sizeof(once_flag) = %zu: %s\n", threads, count,
        sizeof(once_flag), ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
