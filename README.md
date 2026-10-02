# CGAL `Lazy_rep::exact()`: `std::call_once` vs a per-flag atomic once

This repository backs a proposed change to CGAL: replace `std::once_flag` /
`std::call_once` in `Lazy_rep::exact()` (`Filtered_kernel/include/CGAL/Lazy.h`)
with a small CGAL-internal equivalent whose fast path is one inlined atomic load.

It contains

* the proposal itself — [`patches/cgal-once.patch`](patches/cgal-once.patch)
  against CGAL 6.2.1: a new header
  [`CGAL/STL_Extension/internal/once.h`](include/CGAL/STL_Extension/internal/once.h)
  and a 7-line change to `Lazy.h`;
* a benchmark that builds the same source against stock and patched headers and
  compares them ([`bench_lazy.cpp`](bench_lazy.cpp), [`lazy_bench.py`](lazy_bench.py));
* a stress test of the new `call_once` on contended flags ([`test_once.cpp`](test_once.cpp));
* a GitHub workflow that runs all of it on Windows (MSVC, mingw-w64 GCC, Clang
  with libc++), Linux (GCC, Clang) and macOS (Apple Clang, GCC) and publishes the
  merged table as a [release](../../releases).

## Results

**Per-platform results (Windows/MSVC, Windows/mingw-w64, Windows/Clang, Linux,
macOS/arm64) are produced by CI and published under [Releases](../../releases).**
Every release contains one table per platform in the format below plus the raw
samples as JSON.

The table below is one local run, quoted here because it already answers part
of the review: Linux 6.18, AMD Ryzen 9 9955HX (16 cores / 32 threads),
GCC 16.2.1, libstdc++, CGAL 6.2.1, `-O2`. Median wall time of 12 samples per
build; in parentheses the speedup over `stock`.

| Scenario | Threads | stock, ms | std_macro, ms | atomic_char, ms | atomic_int, ms |
|---|--:|--:|--:|--:|--:|
| exact_nodes | 1 | 200 | 199 (1.01×) | 119 (**1.68×**) | 116 (1.73×) |
| exact_nodes | 2 | 209 | 209 (1.00×) | 125 (**1.67×**) | 120 (1.74×) |
| exact_nodes | 4 | 225 | 223 (1.01×) | 136 (**1.65×**) | 132 (1.71×) |
| exact_nodes | 8 | 247 | 253 (0.98×) | 148 (**1.68×**) | 147 (1.69×) |
| exact_nodes | 16 | 255 | 265 (0.96×) | 166 (**1.53×**) | 162 (1.57×) |
| exact_nodes | 32 | 484 | 499 (0.97×) | 290 (**1.67×**) | 292 (1.66×) |
| pocket | 1 | 322 | 317 (1.02×) | 276 (**1.17×**) | 278 (1.16×) |
| pocket | 2 | 340 | 337 (1.01×) | 288 (**1.18×**) | 297 (1.14×) |
| pocket | 4 | 366 | 373 (0.98×) | 313 (**1.17×**) | 323 (1.13×) |
| pocket | 8 | 403 | 415 (0.97×) | 352 (**1.15×**) | 361 (1.12×) |
| pocket | 16 | 438 | 444 (0.99×) | 388 (**1.13×**) | 380 (1.15×) |
| pocket | 32 | 751 | 747 (1.01×) | 679 (**1.11×**) | 652 (1.15×) |

The four builds:

| Build | Headers | Once guard | `sizeof(once_flag)` here |
|---|---|---|--:|
| `stock` | unmodified CGAL 6.2.1 | `std::call_once` | 4 |
| `std_macro` | patched, `-DCGAL_USE_STD_CALL_ONCE` | `std::call_once` via the new header | 4 |
| `atomic_char` | patched, default | `CGAL::internal::call_once`, `unsigned char` state | 1 |
| `atomic_int` | patched, `-DCGAL_ONCE_FLAG_STATE_TYPE=int` | same, `int` state | 4 |

`std_macro` runs the same code as `stock`, so it is an A/A control: its column
shows how much of a difference is just noise (here within ±4%).

The two scenarios:

* `exact_nodes` — the bare cost of the guard. Blocks of 65 lazy numbers (a sum
  of 16 products of doubles), `exact()` on each block; the exact arithmetic is
  kept minimal.
* `pocket` — a realistic workload on `General_polygon_set_2` over
  `Gps_circle_segment_traits_2<EPECK>`: a frame minus a grid of pads, then three
  offset passes, each a union of about a thousand Minkowski "capsules" (a disc
  per vertex, a rectangle per edge) subtracted from the field.

Each of the T threads runs its own independent copy of the workload, nothing is
shared, so the ideal is the same wall time for every T up to the number of
cores (the ×32 rows above use both hardware threads of each core). All builds
must produce the same checksum of the result, otherwise the test fails.

## Answers to the review

### "You need benchmark results on linux-gcc and windows-msvc (possibly macos-arm64-clang), preferably the latest version in each case"

Agreed, and that is what this repository is for. The workflow builds and
measures seven configurations, each with the newest compiler its platform
offers:

| Label | Platform | Compiler, standard library |
|---|---|---|
| `linux-gcc` | Ubuntu 26.04, x86-64 | newest GCC on the image, libstdc++ |
| `linux-clang` | Ubuntu 26.04, x86-64 | newest Clang on the image, libstdc++ |
| `windows-msvc` | Windows Server 2025, x86-64 | MSVC (Visual Studio 2026), MSVC STL |
| `windows-gcc` | Windows Server 2025, x86-64 | MSYS2 UCRT64 GCC (mingw-w64), libstdc++ on winpthreads |
| `windows-clang` | Windows Server 2025, x86-64 | MSYS2 CLANG64 Clang, libc++ |
| `macos-clang` | macOS 26, arm64 | Apple Clang, libc++ |
| `macos-gcc` | macOS 26, arm64 | Homebrew GCC, libstdc++ |

The exact versions are printed in every result table. Results are in
[Releases](../../releases).

Two remarks on "you are changing the implementation for all platforms":

* The local Linux run above shows the change is not a mingw-only fix. With
  GCC 16 and glibc the patched build is 1.5–1.7× faster on the micro-benchmark
  and 1.11–1.18× faster on Boolean set operations, at every thread count. The
  reason is visible in libstdc++'s `<mutex>`: `std::call_once` has no inline
  fast path. Every call — also after the flag is set — stores two thread-local
  pointers, makes an out-of-line call to `pthread_once()` and clears the
  pointers again. `Lazy_rep::exact()` calls it unconditionally (the
  `if (is_lazy())` test in front of it is commented out), so each access to an
  exact value pays for that. The replacement is one atomic load and a compare.
* mingw-w64 being unsupported is a fair point, and nothing in the patch is
  specific to it. Should the CI numbers show a platform where the standard
  version wins, `CGAL_USE_STD_CALL_ONCE` selects it (see below).

### "Is your code essentially a reimplementation of `std::call_once`?"

Yes. It keeps the contract of `std::call_once` and only restricts the interface
to what `Lazy.h` needs.

**Did you consider contributing it to libstdc++ or boost?**
libstdc++ already has this implementation and cannot use it. GCC 11 switched
`std::call_once` to a futex-based flag with an inlined check for the "already
done" case, and it was reverted before the release because the new flag is not
ABI-compatible with the `pthread_once_t`-based one
([PR libstdc++/99341](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=99341): "the
new implementation is superior … and performs better in some cases due to
inlining an initial check", but "by default we need to be compatible with the
old version based on `pthread_once`"). So there is nothing to contribute there
until the next ABI break — which is your rant, exactly. Boost.Thread has
`boost::call_once`, but it is a compiled library, and CGAL only depends on
header-only Boost. For a guard that sits in every node of the lazy DAG, a
small internal header seemed the pragmatic place.

**Is it called `lazy_*` because it is for `Lazy.h`, or because it does something lazier?**
Only because it lived in `Lazy.h`. It is now `CGAL::internal::once_flag` and
`CGAL::internal::call_once`; nothing in it is specific to the lazy kernel.

**"I'd rather give it its own header in STL_extensions."**
Done: `STL_Extension/include/CGAL/STL_Extension/internal/once.h`. The change to
`Lazy.h` shrinks to one `#include`, three `std::once_flag` → `internal::once_flag`
and three `std::call_once` → `internal::call_once`.

**"A typedef with `once_flag` in its name … easier to test if char is faster than int."**
Done. `CGAL::internal::once_flag` is a class of its own (or a typedef for
`std::once_flag`, see the next point); `Lazy.h` no longer mentions `std::atomic`
for this. The integer type of its state is `CGAL_ONCE_FLAG_STATE_TYPE`, default
`unsigned char`, and the benchmark measures both: the `atomic_char` and
`atomic_int` columns. On Linux/x86-64 the two are within noise of each other, so
I kept the 1-byte default; the CI tables show whether another platform disagrees.

**"Keep both versions, with a macro allowing to switch."**
Done. `-DCGAL_USE_STD_CALL_ONCE` makes `once_flag` a typedef for
`std::once_flag` and `call_once` a forwarder to `std::call_once`. The benchmark
builds that configuration as `std_macro`, so the switch is compiled and run on
every platform, and it doubles as the A/A control.

**"If there are some small differences with the standard version, a clear comment documenting those sounds important."**
The header starts with that comment. The differences:

* Only `call_once(flag, f)` with a nullary callable; no extra arguments, no `INVOKE`.
* Re-entering `call_once` for a flag from the function running for that same
  flag deadlocks. The standard leaves this case undefined and the usual
  implementations deadlock too. Nested calls for *different* flags are fine —
  the lazy DAG depends on them (`update_exact()` of a node calls `exact()` of
  its operands).
* Without C++20 `std::atomic::wait` a thread that has to wait polls the flag
  (yields, then sleeps with exponential back-off capped at 1 ms) instead of
  blocking. With C++20 it blocks in `wait()`.
* It is not fork-aware: glibc's `pthread_once` lets the child of a `fork()`
  rerun a function that was in progress in another thread of the parent; here
  such a child would wait forever.

Everything else follows `[thread.once.callonce]`, including exceptions: the
exception reaches the caller, the flag is rearmed and one waiting call runs its
own function.

### "Which `memory_order` is needed when is always a headache"

There are four atomic operations, and none of them uses the default `seq_cst`
by accident:

| Operation | Order | Why |
|---|---|---|
| fast-path `load` | `acquire` | pairs with the release store below: a thread that sees `done` sees everything the function wrote |
| claim: CAS `not_run → running` | `acquire` / `acquire` | a lock acquisition: after an execution that threw, the next one must see its writes; on failure the reloaded value may be `done` |
| announce a waiter: CAS `running → contended` | `acquire` / `acquire` | success needs no ordering (`relaxed` would do), but a failure order stronger than the success order was not allowed before C++17 and GCC still warns about it; slow path only |
| finish: `exchange` to `done` or `not_run` | `release` | publishes the function's writes; its return value tells whether a wake-up is needed |

A wake-up is issued only if the exchanged-out value was `contended`, so an
uncontended flag never enters the kernel. No wake-up can be lost: a waiter
sleeps only while the value is still `contended`, and the finishing thread
changes the value before it notifies.

This is checked by `test_once.cpp`: all hardware threads walk the same 20 000
flags simultaneously, with and without throwing functions, and read the result
through a plain non-atomic variable. It runs in CI on every platform in both
waiting modes (C++20 and C++17). Locally it is also clean under
ThreadSanitizer (GCC 16, 32 threads, both modes). That run is why the C++17
fallback backs off to sleeping: as a pure `yield` loop, 31 spinning waiters
starved the one thread they were waiting for under TSan.

### "The CAS-based alternative was to call the function first and then atomically try to set the result"

You are right, and that sentence of the description was wrong: this patch does
not implement the alternative discussed in the `Lazy_rep` comment. It keeps the
`call_once` idea — one thread computes, the others wait — and only replaces the
primitive. I will remove the claim from the pull request text.

## Running it

Requirements: CMake ≥ 3.22, a C++20 compiler, Python 3, Boost headers, GMP and
MPFR. CGAL itself is downloaded (pinned to 6.2.1, checked by SHA-256); pass
`-DLAZY_BENCH_CGAL_ROOT=<unpacked release>` to use a local copy instead.

```
cmake -S . -B build
cmake --build build
ctest --test-dir build -V
```

| Test | What it does |
|---|---|
| `lazy_bench` | runs the four builds in rotating order, fails if their checksums differ, writes `build/results/<label>.json` and `.md` |
| `once_stress_cxx17`, `once_stress_cxx20` | `call_once` on contended flags, in both waiting modes |
| `patch_matches_headers` | applies `patches/cgal-once.patch` to the pinned release and checks that it reproduces `include/` byte for byte |

Options: `-DLAZY_BENCH_LABEL=<name>` names the result files;
`-DLAZY_BENCH_ARGS="--rounds 4 --reps 3 --threads 1,2,4 --scale 1"` sets the
size of the measurement; `-DLAZY_BENCH_CXX_STANDARD=17` builds the benchmark
itself as C++17.

Two compile-time checks in `bench_lazy.cpp` guard the comparison itself: a
build fails if the `Lazy.h` it sees is not the one its variant asks for, or if
the CGAL headers in the include path are not the pinned release. Without them a
wrong include order would silently produce four identical builds.

In CI, a manual run of the workflow (or pushing a `v*` tag) publishes the
merged report as a release; pushes and pull requests only build and measure.

## Licence

`include/CGAL/Lazy.h` is CGAL's `Filtered_kernel/include/CGAL/Lazy.h` from
release 6.2.1 with the patch applied and keeps its original licence
(LGPL-3.0-or-later OR commercial). `include/CGAL/STL_Extension/internal/once.h`
is offered to CGAL under the same terms.
# lazy_bench
