// This file is a proposed addition to CGAL (www.cgal.org).
//
// $URL$
// $Id$
// SPDX-License-Identifier: LGPL-3.0-or-later OR LicenseRef-Commercial
//

#ifndef CGAL_STL_EXTENSION_INTERNAL_ONCE_H
#define CGAL_STL_EXTENSION_INTERNAL_ONCE_H

#include <CGAL/config.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

// CGAL::internal::once_flag and CGAL::internal::call_once
// ========================================================
//
// A replacement for std::once_flag / std::call_once for flags that exist in
// very large numbers and are tested on a hot path, such as the one in every
// node of the lazy-exact DAG (Lazy_rep::exact()).
//
// Why not std::call_once: its cost is a property of the standard library, and
// the libraries cannot improve it without breaking their ABI.  In libstdc++,
// every call - including the calls made after the flag is set - stores two
// thread-local pointers, makes an out-of-line call to pthread_once() and
// clears the pointers again (see GCC PR 99341 for why the inline fast path
// had to be reverted).  On top of that, with winpthreads (mingw-w64) a
// profile of an exact Boolean-set workload showed every completing flag
// going through a process-wide lock and a kernel wake-up, so that all
// threads doing exact computations serialize on it.
//
// Here the fast path is one inlined atomic load, the state is private to the
// flag, and the kernel is entered only if a thread really has to wait for
// another thread that is running the function of the same flag.
//
// Configuration macros
// --------------------
//   CGAL_USE_STD_CALL_ONCE     once_flag is std::once_flag and call_once()
//                              forwards to std::call_once(): the behavior
//                              before this header existed.  Meant for
//                              platforms where benchmarks favor the standard
//                              library.
//   CGAL_ONCE_FLAG_STATE_TYPE  integer type of the state word; the default is
//                              unsigned char, which makes the flag 1 byte.
//
// Differences from std::call_once
// -------------------------------
//   * Only the form call_once(flag, f) with a nullary callable exists; there
//     are no extra arguments and no INVOKE.
//   * Calling call_once() for a flag from within the function running for
//     that same flag deadlocks.  The standard does not define that case, and
//     the usual implementations deadlock as well.  Nested calls for
//     *different* flags are fine, and the lazy-exact DAG relies on them.
//   * Without C++20 atomic wait/notify (__cpp_lib_atomic_wait), a thread that
//     has to wait for another one polls the flag instead of blocking: it
//     yields first, then sleeps with exponential back-off, at most 1 ms at a
//     time.  No state outside the flag is involved in either mode.
//   * It is not fork-aware.  glibc's pthread_once() lets the child of a
//     fork() rerun a function that was in progress in another thread of the
//     parent; here such a child would wait forever.
//
// Everything else follows [thread.once.callonce]: exactly one execution of f
// returns normally, all other calls for that flag wait for it and then
// observe its side effects; if f throws, the exception reaches the caller,
// the flag is rearmed and one of the waiting calls runs its own function.

namespace CGAL {
namespace internal {

#ifdef CGAL_USE_STD_CALL_ONCE

typedef std::once_flag once_flag;

template <class F>
inline void call_once(once_flag& flag, F&& f)
{
  std::call_once(flag, std::forward<F>(f));
}

#else // not CGAL_USE_STD_CALL_ONCE

#ifndef CGAL_ONCE_FLAG_STATE_TYPE
#  define CGAL_ONCE_FLAG_STATE_TYPE unsigned char
#endif

class once_flag
{
  typedef CGAL_ONCE_FLAG_STATE_TYPE State;

  static constexpr State not_run = 0;   // nobody has run the function yet
  static constexpr State running = 1;   // one thread is running it
  static constexpr State done = 2;      // it has returned normally
  static constexpr State contended = 3; // running, and other threads wait

  std::atomic<State> state_{not_run};

  // Leaves the `running`/`contended` state when the function exits: `done`
  // after a normal return, back to `not_run` if it throws.  The release
  // store is what publishes the side effects of the function to the acquire
  // loads of the other threads.  A wake-up is issued only if a waiter has
  // announced itself, so an uncontended flag never enters the kernel.
  struct Finisher
  {
    std::atomic<State>& state;
    State result;

    ~Finisher()
    {
      if (state.exchange(result, std::memory_order_release) == contended)
        wake(state);
    }
  };

  static void wait(std::atomic<State>& state) noexcept
  {
#if defined(__cpp_lib_atomic_wait)
    state.wait(contended, std::memory_order_acquire);
#else
    // Nothing to block on before C++20, so poll.  The function is usually
    // short: yield first.  If it is not, back off to sleeping, so that a
    // long exact computation does not keep one core busy per waiter (and so
    // that the waiters cannot starve the thread they are waiting for).
    for (unsigned polls = 0; state.load(std::memory_order_acquire) == contended; ++polls) {
      if (polls < 64) {
        std::this_thread::yield();
      } else {
        const unsigned shift = polls - 64 < 10 ? polls - 64 : 10;
        std::this_thread::sleep_for(std::chrono::microseconds(1u << shift));
      }
    }
#endif
  }

  static void wake(std::atomic<State>& state) noexcept
  {
#if defined(__cpp_lib_atomic_wait)
    state.notify_all();
#else
    (void)state;
#endif
  }

  template <class F>
  void call_slow(F&& f)
  {
    State s = state_.load(std::memory_order_acquire);
    for (;;) {
      if (s == done)
        return;

      if (s == not_run) {
        // Claim the flag.  This is a lock acquisition: after an execution
        // that threw, the new one must see what the old one wrote, hence
        // acquire on success.  On failure `s` is reloaded and may be `done`,
        // in which case we return and rely on the same guarantee.
        if (!state_.compare_exchange_weak(s, running,
                                          std::memory_order_acquire,
                                          std::memory_order_acquire))
          continue;
        Finisher finisher{state_, not_run};
        std::forward<F>(f)();
        finisher.result = done;
        return;
      }

      if (s == running) {
        // Announce a waiter, so that the running thread knows it has to
        // issue a wake-up.  Success publishes nothing and reads nothing we
        // depend on, so relaxed would do for it, but a failure order
        // stronger than the success order was not allowed before C++17 and
        // compilers still warn about it; this is the slow path anyway.
        // Failure reloads `s`, as above.
        if (!state_.compare_exchange_weak(s, contended,
                                          std::memory_order_acquire,
                                          std::memory_order_acquire))
          continue;
      }

      // The state was `contended` when last seen: sleep until it changes.
      wait(state_);
      s = state_.load(std::memory_order_acquire);
    }
  }

public:
  constexpr once_flag() noexcept {}
  once_flag(const once_flag&) = delete;
  once_flag& operator=(const once_flag&) = delete;

  template <class F>
  friend void call_once(once_flag& flag, F&& f);
};

template <class F>
inline void call_once(once_flag& flag, F&& f)
{
  // The only path taken once the flag is set.  Acquire pairs with the
  // release store in Finisher.
  if (flag.state_.load(std::memory_order_acquire) != once_flag::done)
    flag.call_slow(std::forward<F>(f));
}

#endif // not CGAL_USE_STD_CALL_ONCE

} // namespace internal
} // namespace CGAL

#endif // CGAL_STL_EXTENSION_INTERNAL_ONCE_H
