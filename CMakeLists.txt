# Benchmark for the once guard in CGAL's Lazy_rep::exact():
# std::call_once versus CGAL::internal::call_once (include/, patches/).
#
#   cmake -S . -B build && cmake --build build
#   ctest --test-dir build -V
#
# One source file is built four times against the same pinned CGAL release:
#
#   bench_lazy_stock        unmodified CGAL
#   bench_lazy_std_macro    patched headers, CGAL_USE_STD_CALL_ONCE (A/A control)
#   bench_lazy_atomic_char  patched headers, default 1-byte flag
#   bench_lazy_atomic_int   patched headers, CGAL_ONCE_FLAG_STATE_TYPE=int
#
# The test `lazy_bench` (lazy_bench.py run) runs the builds in rotating order,
# checks that all of them compute the same thing and writes <label>.json and
# <label>.md to <build>/results.
cmake_minimum_required(VERSION 3.22)
project(CGALLazyOnceBench LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# C++20 gives the patched flag std::atomic::wait; with C++17 it falls back to
# a yield loop (set LAZY_BENCH_CXX_STANDARD=17 to measure that).
set(LAZY_BENCH_CXX_STANDARD 20 CACHE STRING "C++ standard to build the benchmark with")
set(CMAKE_CXX_STANDARD ${LAZY_BENCH_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CGAL is PINNED: include/CGAL/Lazy.h is the Lazy.h of one particular release
# plus the patch, so it can only be compared with the stock Lazy.h of that
# same release.  A system or package-manager CGAL is therefore never used;
# the release headers are downloaded instead.  Boost, GMP and MPFR come from
# the system and are located by CGAL's own CMake configuration.
#
# LAZY_BENCH_CGAL_ROOT - an already unpacked release (the directory holding
# CGALConfig.cmake), to skip the download.
set(LAZY_BENCH_CGAL_VERSION "6.2.1" CACHE STRING "CGAL release that include/CGAL/Lazy.h is based on")
set(LAZY_BENCH_CGAL_SHA256 "8e7ce1e30e8ffcb3e69d153c3a561bb1f38bbb4becd8e6544cbaa0fb2889af62"
    CACHE STRING "SHA-256 of CGAL-<version>-library.tar.xz")
set(LAZY_BENCH_CGAL_ROOT "" CACHE PATH "Unpacked CGAL release to use instead of downloading one")

if(NOT LAZY_BENCH_CGAL_ROOT)
  set(cgal_name "CGAL-${LAZY_BENCH_CGAL_VERSION}")
  set(cgal_archive "${CMAKE_BINARY_DIR}/_deps/${cgal_name}-library.tar.xz")
  set(cgal_root "${CMAKE_BINARY_DIR}/_deps/${cgal_name}")
  if(NOT EXISTS "${cgal_root}/CGALConfig.cmake")
    message(STATUS "Downloading ${cgal_name}")
    file(
      DOWNLOAD
      "https://github.com/CGAL/cgal/releases/download/v${LAZY_BENCH_CGAL_VERSION}/${cgal_name}-library.tar.xz"
      "${cgal_archive}"
      EXPECTED_HASH SHA256=${LAZY_BENCH_CGAL_SHA256}
      TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT "${cgal_archive}" DESTINATION "${CMAKE_BINARY_DIR}/_deps")
  endif()
else()
  set(cgal_root "${LAZY_BENCH_CGAL_ROOT}")
endif()

# NO_DEFAULT_PATH: another CGAL (system, vcpkg) may be installed, and only the
# pinned one must be found.
find_package(CGAL REQUIRED CONFIG PATHS "${cgal_root}" NO_DEFAULT_PATH)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(patched_include_dir "${CMAKE_CURRENT_SOURCE_DIR}/include")

# add_lazy_bench(<variant> <patched: 0|1> [<compile definition>...])
function(add_lazy_bench variant patched)
  set(target bench_lazy_${variant})
  add_executable(${target} bench_lazy.cpp)
  target_link_libraries(${target} PRIVATE CGAL::CGAL Threads::Threads)
  # CGAL_HAS_THREADS: without it one of the three Lazy_rep templates has no
  # once guard at all.  CGAL_NDEBUG: benchmark without CGAL's assertions.
  target_compile_definitions(
    ${target}
    PRIVATE CGAL_HAS_THREADS
            CGAL_NDEBUG
            LAZY_BENCH_VARIANT="${variant}"
            LAZY_BENCH_PATCHED=${patched}
            LAZY_BENCH_CGAL_VERSION="${CGAL_VERSION}"
            ${ARGN})
  # The order of include directories is the whole difference between stock
  # and patched builds, so it is made explicit and uses plain -I only: the
  # target's own directories precede those of CGAL::CGAL, and -isystem (whose
  # ordering relative to -I is compiler-specific) is switched off.
  set_target_properties(${target} PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
  if(patched)
    target_include_directories(${target} BEFORE PRIVATE "${patched_include_dir}")
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8)
  endif()
endfunction()

add_lazy_bench(stock 0)
add_lazy_bench(std_macro 1 CGAL_USE_STD_CALL_ONCE)
add_lazy_bench(atomic_char 1)
add_lazy_bench(atomic_int 1 CGAL_ONCE_FLAG_STATE_TYPE=int)

# The platform label names the result files and the row in the summary.
string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_CXX_COMPILER_ID}" default_label)
set(LAZY_BENCH_LABEL "${default_label}" CACHE STRING "Platform label used in the report")
set(LAZY_BENCH_ARGS "" CACHE STRING "Extra arguments for lazy_bench.py run (--rounds, --reps, --threads, --scale)")
separate_arguments(lazy_bench_args NATIVE_COMMAND "${LAZY_BENCH_ARGS}")

enable_testing()

# The first variant is the baseline; --primary is the one the summary quotes.
add_test(
  NAME lazy_bench
  COMMAND
    Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/lazy_bench.py" run
    --variant stock=$<TARGET_FILE:bench_lazy_stock>
    --variant std_macro=$<TARGET_FILE:bench_lazy_std_macro>
    --variant atomic_char=$<TARGET_FILE:bench_lazy_atomic_char>
    --variant atomic_int=$<TARGET_FILE:bench_lazy_atomic_int>
    --primary atomic_char
    --out-dir "${CMAKE_BINARY_DIR}/results" --label "${LAZY_BENCH_LABEL}" ${lazy_bench_args})
# The measurement is only fair on an otherwise idle machine.  The timeout is
# generous because the stock build can be several times slower with threads.
set_tests_properties(lazy_bench PROPERTIES RUN_SERIAL ON TIMEOUT 3600)

# The benchmark never has two threads on the same flag; this test does, in
# both waiting modes: C++20 (std::atomic::wait) and C++17 (yield loop).
foreach(standard 17 20)
  set(target test_once_cxx${standard})
  add_executable(${target} test_once.cpp)
  target_link_libraries(${target} PRIVATE CGAL::CGAL Threads::Threads)
  target_include_directories(${target} BEFORE PRIVATE "${patched_include_dir}")
  set_target_properties(${target} PROPERTIES CXX_STANDARD ${standard} NO_SYSTEM_FROM_IMPORTED ON)
  add_test(NAME once_stress_cxx${standard} COMMAND ${target})
  set_tests_properties(once_stress_cxx${standard} PROPERTIES RUN_SERIAL ON TIMEOUT 600)
endforeach()

# patches/cgal-once.patch is what is proposed upstream; include/ is what is
# measured.  This test keeps the two from drifting apart: the patch applied
# to the pinned release must reproduce include/ byte for byte.
find_program(PATCH_EXECUTABLE patch)
if(PATCH_EXECUTABLE AND NOT WIN32)
  add_test(
    NAME patch_matches_headers
    COMMAND
      ${CMAKE_COMMAND} -DPATCH_EXECUTABLE=${PATCH_EXECUTABLE} -DCGAL_ROOT=${cgal_root}
      -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -DWORK_DIR=${CMAKE_BINARY_DIR}/patch_check -P
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_patch.cmake")
endif()
