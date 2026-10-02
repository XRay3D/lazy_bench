// Benchmark for the once guard in CGAL's Lazy_rep::exact().
//
// This one source file is built several times against the same CGAL release;
// the builds differ only in which Lazy.h they see and in two macros:
//
//   stock        unmodified CGAL: std::once_flag / std::call_once
//   std_macro    patched headers with CGAL_USE_STD_CALL_ONCE: must behave like
//                `stock`, so it doubles as an A/A control for the noise level
//   atomic_char  patched headers, default: CGAL::internal::once_flag, 1 byte
//   atomic_int   patched headers with CGAL_ONCE_FLAG_STATE_TYPE=int
//
// Scenarios:
//
//   exact_nodes  the bare cost of the guard: blocks of lazy sums of products,
//                exact() on each block.  Every node completes its flag once
//                and is asked for its exact value a few more times; the exact
//                arithmetic itself is kept minimal.
//   pocket       a realistic workload: Boolean set operations on
//                circle-segment polygons (EPECK).  A frame minus a grid of
//                pads, then offset passes built as unions of Minkowski
//                "capsules" (a disc per vertex, a rectangle per edge).  The
//                rectangle corners lie on the disc circles up to double
//                rounding, so the interval filter fails there and the sweep
//                falls back to exact arithmetic, as it does on real data.
//
// Every configuration runs T threads, and EACH thread runs its own
// independent copy of the workload: nothing is shared, so the ideal is the
// same wall time for any T up to the number of cores.  Whatever comes on top
// is serialization inside the standard library.
//
// Fully deterministic: the checksum of the result must be the same for all
// threads, all repetitions and all builds (the latter is checked by
// lazy_bench.py).
//
// Arguments: [--threads 1,2,4] [--reps 5] [--scale 1] [--json file].

#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/General_polygon_set_2.h>
#include <CGAL/Gps_circle_segment_traits_2.h>
#include <CGAL/version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

// Which Lazy.h did this build REALLY get?  The variants differ only in the
// order of -I directories, and if that order were wrong, all builds would
// silently use the same header and the report would honestly say "no
// difference".  The patched Lazy.h includes once.h, the stock one does not.
#ifdef CGAL_STL_EXTENSION_INTERNAL_ONCE_H
constexpr bool patchedHeaders = true;
using OnceFlag = CGAL::internal::once_flag;
#else
constexpr bool patchedHeaders = false;
using OnceFlag = std::once_flag;
#endif
static_assert(patchedHeaders == bool(LAZY_BENCH_PATCHED),
    "bench_lazy: Lazy.h in the include path does not match the requested variant");

// And is the flag the one this variant is supposed to measure?
#if defined(CGAL_USE_STD_CALL_ONCE) || !LAZY_BENCH_PATCHED
static_assert(std::is_same_v<OnceFlag, std::once_flag>, "bench_lazy: expected std::once_flag");
#else
static_assert(sizeof(OnceFlag) == sizeof(CGAL_ONCE_FLAG_STATE_TYPE), "bench_lazy: unexpected once_flag layout");
#endif

// The same check from the other side: another CGAL (vcpkg, system package)
// may sit in the include path next to the pinned one.
static_assert(std::string_view(CGAL_VERSION_STR) == LAZY_BENCH_CGAL_VERSION,
    "bench_lazy: CGAL headers in the include path are not the pinned version");

using K = CGAL::Exact_predicates_exact_constructions_kernel;
using Traits = CGAL::Gps_circle_segment_traits_2<K>;
using PolySet = CGAL::General_polygon_set_2<Traits>;
using GPoly = Traits::Polygon_2;
using GPolyWH = Traits::Polygon_with_holes_2;
using Curve = Traits::Curve_2;
using XCurve = Traits::X_monotone_curve_2;

// FNV-1a over the result.  Doubles are snapped to a 1e-6 grid: the checksum
// verifies the geometry, not the last bit of an exact-to-double conversion.
struct Checksum {
    std::uint64_t value{0xcbf29ce484222325ull};

    void add(std::uint64_t x) {
        for(int i = 0; i < 8; ++i, x >>= 8) {
            value ^= x & 0xff;
            value *= 0x100000001b3ull;
        }
    }
    void add(double x) { add(static_cast<std::uint64_t>(std::llround(x * 1e6))); }
};

// --- exact_nodes -------------------------------------------------------------

// A block is a sum of 16 products: 32 constant leaves, 16 multiplications and
// 16 additions, 65 lazy nodes with the initial zero.  exact() on the root
// evaluates all of them.  The tree depth is bounded by the block because
// exactification is recursive; one long chain would overflow the stack.
std::uint64_t exactNodes(int scale) {
    using FT = K::FT;
    Checksum sum;
    const int blocks = 12000 * scale;
    for(int b = 0; b < blocks; ++b) {
        FT acc(0);
        for(int i = 0; i < 16; ++i) {
            const double x = 0.37 * (b % 97 + 1) + 0.011 * i;
            const double y = 1.0 / (i + 3) + 1e-3 * (b % 89);
            acc += FT(x) * FT(y);
        }
        acc.exact();
        sum.add(CGAL::to_double(acc));
    }
    return sum.value;
}

// --- pocket ------------------------------------------------------------------

struct Pt {
    double x, y;
};
using Outline = std::vector<Pt>; // closed, counterclockwise

void appendCurve(GPoly& pgn, const Curve& curve) {
    const Traits traits;
    std::vector<std::variant<Traits::Point_2, XCurve>> pieces;
    traits.make_x_monotone_2_object()(curve, std::back_inserter(pieces));
    for(const auto& piece: pieces)
        if(const XCurve* xc = std::get_if<XCurve>(&piece)) pgn.push_back(*xc);
}

GPoly toGPoly(const Outline& outline) {
    GPoly pgn;
    for(std::size_t i = 0; i < outline.size(); ++i) {
        const Pt& a = outline[i];
        const Pt& b = outline[(i + 1) % outline.size()];
        pgn.push_back(XCurve(K::Point_2(a.x, a.y), K::Point_2(b.x, b.y)));
    }
    return pgn;
}

// A full disc from one exact Circle_2 (center and squared radius): the ends
// of its x-monotone arcs carry a square root, and comparing such points is
// the main consumer of lazy exact arithmetic in the sweep.
GPoly disc(Pt center, double d) {
    GPoly pgn;
    const K::FT r2 = CGAL::square(K::FT(d));
    appendCurve(pgn, Curve(K::Circle_2(K::Point_2(center.x, center.y), r2, CGAL::COUNTERCLOCKWISE)));
    return pgn;
}

Outline rectangle(double x0, double y0, double x1, double y1) {
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

constexpr double pi = 3.141592653589793238462643383279502884;

// A toothed ring: short edges at arbitrary angles rather than axis-aligned
// ones only.
Outline toothedRing(double cx, double cy, double radius, double tooth, int teeth = 24) {
    Outline ring;
    for(int k = 0; k < teeth; ++k) {
        const double a = 2.0 * pi * k / teeth;
        const double r = radius + (k % 2 ? tooth : -tooth);
        ring.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return ring;
}

// Pieces of the Minkowski sum of an outline with a disc of radius d: a disc
// around every vertex and a rectangle along every edge.
void appendCapsules(std::vector<GPoly>& out, const Outline& outline, double d) {
    for(std::size_t i = 0; i < outline.size(); ++i) {
        const Pt& from = outline[i];
        const Pt& to = outline[(i + 1) % outline.size()];
        out.push_back(disc(from, d));
        const double dx = to.x - from.x, dy = to.y - from.y;
        const double chord = std::hypot(dx, dy);
        const Pt n{dy / chord * d, -dx / chord * d}; // right normal * d
        out.push_back(toGPoly({{from.x + n.x, from.y + n.y}, {to.x + n.x, to.y + n.y},
            {to.x - n.x, to.y - n.y}, {from.x - n.x, from.y - n.y}}));
    }
}

void addToChecksum(Checksum& sum, const GPoly& pgn) {
    sum.add(std::uint64_t(pgn.size()));
    for(auto it = pgn.curves_begin(); it != pgn.curves_end(); ++it) {
        sum.add(CGAL::to_double(it->source().x()));
        sum.add(CGAL::to_double(it->source().y()));
    }
}

// The field "frame minus pads" and three offset passes: a band of width d
// along the copper is united from capsules and subtracted from the field.
std::uint64_t pocket(int scale) {
    const int padsPerSide = 6 * scale;
    const double pitch = 4.0;
    const double size = padsPerSide * pitch + 8.0;

    std::vector<Outline> copper;
    const double origin = -(padsPerSide - 1) * pitch / 2;
    for(int j = 0; j < padsPerSide; ++j)
        for(int i = 0; i < padsPerSide; ++i) {
            const double cx = origin + i * pitch, cy = origin + j * pitch;
            if((i + j) % 2)
                copper.push_back(toothedRing(cx, cy, 1.3, 0.12));
            else
                copper.push_back(rectangle(cx - 1.1, cy - 1.1, cx + 1.1, cy + 1.1));
            // A bridge to the right neighbor stitches pads into clusters.
            if(i + 1 < padsPerSide && j % 3 == 0)
                copper.push_back(rectangle(cx, cy - 0.25, cx + pitch, cy + 0.25));
        }

    std::vector<GPoly> pads;
    for(const Outline& outline: copper) pads.push_back(toGPoly(outline));
    PolySet metal;
    metal.join(pads.begin(), pads.end());
    PolySet field(toGPoly(rectangle(-size / 2, -size / 2, size / 2, size / 2)));
    field.difference(metal);

    Checksum sum;
    for(const double d: {0.5, 0.95, 1.4}) {
        std::vector<GPoly> capsules;
        for(const Outline& outline: copper) appendCapsules(capsules, outline, d);
        PolySet band;
        band.join(capsules.begin(), capsules.end());

        PolySet pass = field;
        pass.difference(band);

        std::vector<GPolyWH> bodies;
        pass.polygons_with_holes(std::back_inserter(bodies));
        sum.add(std::uint64_t(bodies.size()));
        for(const GPolyWH& body: bodies) {
            addToChecksum(sum, body.outer_boundary());
            sum.add(std::uint64_t(body.number_of_holes()));
            for(auto hole = body.holes_begin(); hole != body.holes_end(); ++hole) addToChecksum(sum, *hole);
        }
    }
    return sum.value;
}

// --- running -----------------------------------------------------------------

struct Scenario {
    const char* name;
    std::uint64_t (*job)(int scale);
};

constexpr Scenario scenarios[]{{"exact_nodes", exactNodes}, {"pocket", pocket}};

struct Row {
    const char* scenario;
    unsigned threads;
    std::uint64_t checksum;
    std::vector<double> ms;
};

// One measurement: T threads, each with its own copy of the workload.  Yields
// the wall time from starting the first thread to joining the last one;
// returns false if a thread failed or the checksums of the threads differ.
bool measure(const Scenario& scenario, unsigned threads, int scale, double& ms, std::uint64_t& checksum) {
    std::vector<std::uint64_t> sums(threads);
    std::atomic<bool> failed{false};

    const auto t0 = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> pool;
        for(unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                try {
                    sums[t] = scenario.job(scale);
                } catch(const std::exception& e) {
                    std::fprintf(stderr, "%s: thread %u: %s\n", scenario.name, t, e.what());
                    failed = true;
                } catch(...) {
                    std::fprintf(stderr, "%s: thread %u: unknown exception\n", scenario.name, t);
                    failed = true;
                }
            });
        for(std::thread& thread: pool) thread.join();
    }
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    if(failed) return false;
    checksum = sums.front();
    if(std::any_of(sums.begin(), sums.end(), [&](std::uint64_t s) { return s != checksum; })) {
        std::fprintf(stderr, "%s: checksums differ between threads\n", scenario.name);
        return false;
    }
    return true;
}

// --- build description -------------------------------------------------------

std::string compilerName() {
    char buf[96];
#if defined(__clang__) && defined(__apple_build_version__)
    std::snprintf(buf, sizeof buf, "Apple Clang %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__clang__) && defined(_MSC_VER)
    std::snprintf(buf, sizeof buf, "clang-cl %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__clang__)
    std::snprintf(buf, sizeof buf, "Clang %d.%d.%d", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    std::snprintf(buf, sizeof buf, "GCC %d.%d.%d", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    std::snprintf(buf, sizeof buf, "MSVC %d.%d.%d", _MSC_VER / 100, _MSC_VER % 100, _MSC_FULL_VER % 100000);
#else
    std::snprintf(buf, sizeof buf, "unknown");
#endif
    return buf;
}

// The standard library and what its std::call_once is built on: the main
// explanatory variable of the whole measurement.  libstdc++ uses
// __gthread_once, plus a global mutex around every call if there is no TLS
// (see its <mutex>).
std::string stdlibName() {
    char buf[96];
#if defined(_LIBCPP_VERSION)
    std::snprintf(buf, sizeof buf, "libc++ %d", int(_LIBCPP_VERSION / 10000));
#elif defined(__GLIBCXX__)
    #if !defined(_GLIBCXX_HAS_GTHREADS)
    const char* once = "no gthreads";
    #elif defined(_GLIBCXX_HAVE_TLS)
    const char* once = "gthread_once";
    #else
    const char* once = "gthread_once, global functor mutex";
    #endif
    std::snprintf(buf, sizeof buf, "libstdc++ %d (%s)", int(_GLIBCXX_RELEASE), once);
#elif defined(_MSVC_STL_UPDATE)
    std::snprintf(buf, sizeof buf, "MSVC STL %ld", long(_MSVC_STL_UPDATE));
#else
    std::snprintf(buf, sizeof buf, "unknown");
#endif
    return buf;
}

const char* osName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

const char* archName() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "unknown";
#endif
}

// Default thread counts: powers of two up to the number of hardware threads,
// plus that number itself.
std::vector<unsigned> defaultThreads() {
    const unsigned hardware = std::max(std::thread::hardware_concurrency(), 1u);
    std::vector<unsigned> out;
    for(unsigned t = 1; t < hardware; t *= 2) out.push_back(t);
    out.push_back(hardware);
    return out;
}

std::vector<unsigned> parseThreads(const char* text) {
    std::vector<unsigned> out;
    for(char* end{}; *text; text = *end ? end + 1 : end) {
        const unsigned long t = std::strtoul(text, &end, 10);
        if(end == text || (*end && *end != ',') || !t) return {};
        out.push_back(unsigned(t));
    }
    return out;
}

void writeJson(std::FILE* file, const std::vector<Row>& rows, int scale) {
    std::fprintf(file, "{\n");
    std::fprintf(file, "  \"variant\": \"%s\",\n", LAZY_BENCH_VARIANT);
    std::fprintf(file, "  \"once_flag_bytes\": %u,\n", unsigned(sizeof(OnceFlag)));
    std::fprintf(file, "  \"cgal\": \"%s\",\n", CGAL_VERSION_STR);
    std::fprintf(file, "  \"compiler\": \"%s\",\n", compilerName().c_str());
    std::fprintf(file, "  \"stdlib\": \"%s\",\n", stdlibName().c_str());
    std::fprintf(file, "  \"os\": \"%s\",\n", osName());
    std::fprintf(file, "  \"arch\": \"%s\",\n", archName());
    std::fprintf(file, "  \"hardware_threads\": %u,\n", std::thread::hardware_concurrency());
    std::fprintf(file, "  \"scale\": %d,\n", scale);
    std::fprintf(file, "  \"results\": [\n");
    for(std::size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        std::fprintf(file, "    {\"scenario\": \"%s\", \"threads\": %u, \"checksum\": \"%016llx\", \"ms\": [",
            row.scenario, row.threads, static_cast<unsigned long long>(row.checksum));
        for(std::size_t k = 0; k < row.ms.size(); ++k) std::fprintf(file, "%s%.3f", k ? ", " : "", row.ms[k]);
        std::fprintf(file, "]}%s\n", i + 1 < rows.size() ? "," : "");
    }
    std::fprintf(file, "  ]\n}\n");
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<unsigned> threads = defaultThreads();
    int reps = 5, scale = 1;
    const char* jsonPath{};

    for(int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if(arg == "--threads" && value)
            threads = parseThreads(argv[++i]);
        else if(arg == "--reps" && value)
            reps = std::atoi(argv[++i]);
        else if(arg == "--scale" && value)
            scale = std::atoi(argv[++i]);
        else if(arg == "--json" && value)
            jsonPath = argv[++i];
        else {
            std::fprintf(stderr, "usage: %s [--threads 1,2,4] [--reps 5] [--scale 1] [--json file]\n", argv[0]);
            return 2;
        }
    }
    if(threads.empty() || reps < 1 || scale < 1) {
        std::fprintf(stderr, "bench_lazy: bad --threads, --reps or --scale\n");
        return 2;
    }

    std::fprintf(stderr, "bench_lazy [%s, once_flag %u bytes]: CGAL %s, %s, %s, %s/%s, %u hardware threads\n",
        LAZY_BENCH_VARIANT, unsigned(sizeof(OnceFlag)), CGAL_VERSION_STR, compilerName().c_str(),
        stdlibName().c_str(), osName(), archName(), std::thread::hardware_concurrency());

    std::vector<Row> rows;
    for(const Scenario& scenario: scenarios) {
        // An unmeasured warm-up: allocator arenas, pages, CGAL thread-locals.
        double ms{};
        std::uint64_t reference{};
        if(!measure(scenario, 1, scale, ms, reference)) return 1;

        for(const unsigned t: threads) {
            Row row{scenario.name, t, reference, {}};
            for(int r = 0; r < reps; ++r) {
                std::uint64_t checksum{};
                if(!measure(scenario, t, scale, ms, checksum)) return 1;
                if(checksum != reference) {
                    std::fprintf(stderr, "%s: checksum changed between runs\n", scenario.name);
                    return 1;
                }
                row.ms.push_back(ms);
            }
            std::vector<double> sorted = row.ms;
            std::sort(sorted.begin(), sorted.end());
            std::fprintf(stderr, "  %-12s x%-3u median %9.2f ms, min %9.2f ms  [%016llx]\n", scenario.name, t,
                sorted[sorted.size() / 2], sorted.front(), static_cast<unsigned long long>(reference));
            rows.push_back(std::move(row));
        }
    }

    if(jsonPath) {
        std::FILE* file = std::fopen(jsonPath, "w");
        if(!file) {
            std::perror(jsonPath);
            return 1;
        }
        writeJson(file, rows, scale);
        std::fclose(file);
    } else
        writeJson(stdout, rows, scale);
    return 0;
}
