#!/usr/bin/env python3
"""Driver for bench_lazy: compares builds that differ only in the once guard.

  lazy_bench.py run --variant NAME=EXE [--variant NAME=EXE ...]
                    --out-dir DIR --label NAME [--primary NAME]
      Runs the builds in rotating order, checks that all of them compute the
      same thing, writes DIR/LABEL.json and DIR/LABEL.md.  The first variant
      is the baseline that speedups refer to.

  lazy_bench.py summary --out FILE [--expect A,B,...] RESULT.json...
      Merges per-platform results into one Markdown report.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile

# Build description: must be identical for all variants, otherwise the
# comparison is between different builds rather than different once guards.
META = ("cgal", "compiler", "stdlib", "os", "arch", "hardware_threads", "scale")


def fail(message):
    print("lazy_bench: " + message, file=sys.stderr)
    sys.exit(1)


def run_bench(exe, variant, bench_args):
    """One launch of bench_lazy; returns its parsed JSON."""
    handle, path = tempfile.mkstemp(suffix=".json", prefix="bench_lazy_")
    os.close(handle)
    try:
        code = subprocess.call([exe, "--json", path] + bench_args)
        if code:
            fail("%s exited with code %d" % (exe, code))
        with open(path, encoding="utf-8") as file:
            data = json.load(file)
    finally:
        os.unlink(path)
    if data["variant"] != variant:
        fail("%s reports variant '%s', expected '%s'" % (exe, data["variant"], variant))
    return data


def stats(samples):
    return {"median": statistics.median(samples), "min": min(samples), "samples": samples}


def cmd_run(args):
    bench_args = ["--reps", str(args.reps)]
    if args.threads:
        bench_args += ["--threads", args.threads]
    if args.scale:
        bench_args += ["--scale", str(args.scale)]

    names, exes = [], {}
    for item in args.variant:
        name, sep, exe = item.partition("=")
        if not sep or not name or not exe or name in exes:
            fail("bad --variant '%s', expected NAME=EXE with unique names" % item)
        names.append(name)
        exes[name] = exe
    if len(names) < 2:
        fail("at least two --variant are needed")
    primary = args.primary or names[1]
    if primary not in exes or primary == names[0]:
        fail("--primary must name a variant other than the baseline")

    meta = None
    flag_bytes = {}
    samples = {}  # (scenario, threads) -> {"checksum": ..., "ms": {variant: [...]}}
    order = []

    # The launch order rotates from round to round, so that warm-up, thermal
    # throttling and noisy neighbors on a shared runner hit all variants alike.
    for round_index in range(args.rounds):
        shift = round_index % len(names)
        for variant in names[shift:] + names[:shift]:
            print("round %d/%d: %s" % (round_index + 1, args.rounds, variant), file=sys.stderr, flush=True)
            data = run_bench(exes[variant], variant, bench_args)
            flag_bytes[variant] = data["once_flag_bytes"]

            this_meta = {key: data[key] for key in META}
            if meta is None:
                meta = this_meta
            elif this_meta != meta:
                fail("build metadata differs between runs: %s vs %s" % (meta, this_meta))

            for result in data["results"]:
                key = (result["scenario"], result["threads"])
                if key not in samples:
                    samples[key] = {"checksum": result["checksum"], "ms": {name: [] for name in names}}
                    order.append(key)
                # The actual test: every build must compute exactly the same.
                if result["checksum"] != samples[key]["checksum"]:
                    fail("checksum mismatch in %s x%d (%s): %s vs %s" % (
                        key[0], key[1], variant, result["checksum"], samples[key]["checksum"]))
                samples[key]["ms"][variant] += result["ms"]

    rows = []
    for key in order:
        entry = samples[key]
        if not all(entry["ms"][name] for name in names):
            fail("%s x%d was not measured by all variants" % key)
        rows.append({
            "scenario": key[0], "threads": key[1], "checksum": entry["checksum"],
            "variants": {name: stats(entry["ms"][name]) for name in names},
        })

    report = dict(meta, label=args.label, rounds=args.rounds, reps=args.reps, variants=names,
                  primary=primary, once_flag_bytes=flag_bytes, rows=rows)
    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, args.label + ".json"), "w", encoding="utf-8") as file:
        json.dump(report, file, indent=1)
        file.write("\n")

    markdown = "\n".join(platform_section(report, heading="###")) + "\n"
    with open(os.path.join(args.out_dir, args.label + ".md"), "w", encoding="utf-8") as file:
        file.write(markdown)
    print(markdown)

    # On GitHub the same table goes to the run's summary page.
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as file:
            file.write(markdown + "\n")


def ms(value):
    return "%.1f" % value if value < 100 else "%.0f" % value


def speedup(row, variant, baseline):
    return row["variants"][baseline]["median"] / row["variants"][variant]["median"]


def platform_section(report, heading):
    """Detailed table of one platform."""
    names = report["variants"]
    baseline = names[0]
    lines = [
        "%s %s" % (heading, report["label"]),
        "",
        "%s, %s, CGAL %s, %s/%s, %d hardware threads." % (
            report["compiler"], report["stdlib"], report["cgal"], report["os"], report["arch"],
            report["hardware_threads"]),
        "Median wall time of %d samples per build (%d rounds × %d reps, launch order rotated);" % (
            report["rounds"] * report["reps"], report["rounds"], report["reps"]),
        "in parentheses the speedup over `%s`. `sizeof(once_flag)`: %s." % (
            baseline, ", ".join("`%s` %d" % (name, report["once_flag_bytes"][name]) for name in names)),
        "",
        "| Scenario | Threads | " + " | ".join("%s, ms" % name for name in names) + " |",
        "|---|--:|" + "--:|" * len(names),
    ]
    for row in report["rows"]:
        cells = [ms(row["variants"][baseline]["median"])]
        for name in names[1:]:
            text = "%.2f×" % speedup(row, name, baseline)
            if name == report["primary"]:
                text = "**%s**" % text
            cells.append("%s (%s)" % (ms(row["variants"][name]["median"]), text))
        lines.append("| %s | %d | %s |" % (row["scenario"], row["threads"], " | ".join(cells)))
    return lines


def overview_cell(report, row, with_threads):
    baseline, primary = report["variants"][0], report["primary"]
    prefix = "×%d: " % row["threads"] if with_threads else ""
    return "%s**%.2f×** (%s → %s)" % (
        prefix, speedup(row, primary, baseline), ms(row["variants"][baseline]["median"]),
        ms(row["variants"][primary]["median"]))


def cmd_summary(args):
    reports = []
    for path in args.results:
        with open(path, encoding="utf-8") as file:
            reports.append(json.load(file))
    reports.sort(key=lambda report: report["label"])

    scenarios = []
    for report in reports:
        for row in report["rows"]:
            if row["scenario"] not in scenarios:
                scenarios.append(row["scenario"])

    lines = [
        "# CGAL `Lazy_rep::exact()`: `std::call_once` vs `CGAL::internal::call_once`",
        "",
        "One benchmark source is built several times against the same CGAL release:",
        "",
        "* `stock` — unmodified CGAL (`std::once_flag` / `std::call_once`);",
        "* `std_macro` — patched headers with `CGAL_USE_STD_CALL_ONCE`: the same code path as",
        "  `stock`, i.e. an A/A control that shows the noise level of the runner;",
        "* `atomic_char` — patched headers, default (`once_flag` is one atomic byte);",
        "* `atomic_int` — patched headers with `CGAL_ONCE_FLAG_STATE_TYPE=int`.",
        "",
        "All builds are verified to produce identical checksums. Every thread runs its own",
        "independent copy of the workload, so the ideal is the same time at any thread count",
        "up to the number of cores.",
        "",
        "* `exact_nodes` — the bare cost of the guard: blocks of lazy exact numbers, `exact()`",
        "  on each block.",
        "* `pocket` — Boolean set operations on circle-segment polygons (EPECK): a frame minus",
        "  pads, then offset passes built as unions of Minkowski capsules.",
        "",
    ]

    if args.expect:
        present = {report["label"] for report in reports}
        missing = [label for label in args.expect.split(",") if label and label not in present]
        if missing:
            lines += ["> **No results for:** %s — the job failed or was skipped." % ", ".join(missing), ""]

    if not reports:
        lines += ["No results were produced.", ""]
    else:
        # Overview: per scenario, one thread and the platform's maximum.
        header = "| Platform | Compiler | Standard library |"
        rule = "|---|---|---|"
        for scenario in scenarios:
            header += " %s ×1 | %s ×max |" % (scenario, scenario)
            rule += "--:|--:|"
        lines += ["## Overview", "", header, rule]
        for report in reports:
            line = "| %s | %s | %s |" % (report["label"], report["compiler"], report["stdlib"])
            for scenario in scenarios:
                rows = [row for row in report["rows"] if row["scenario"] == scenario]
                one = [row for row in rows if row["threads"] == 1]
                most = max(rows, key=lambda row: row["threads"]) if rows else None
                line += " %s |" % (overview_cell(report, one[0], False) if one else "—")
                line += " %s |" % (overview_cell(report, most, True) if most and most["threads"] > 1 else "—")
            lines.append(line)
        lines += ["", "Cells: speedup of the default patched build over `stock` (stock → patched, ms).",
                  "", "## Details", ""]
        for report in reports:
            lines += platform_section(report, heading="###") + [""]

    text = "\n".join(lines)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as file:
            file.write(text)
    else:
        print(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser("run", help="run the builds and compare them")
    run.add_argument("--variant", action="append", default=[], metavar="NAME=EXE",
                     help="a build of bench_lazy; the first one is the baseline")
    run.add_argument("--primary", help="variant quoted in the summary (default: the second one)")
    run.add_argument("--out-dir", required=True)
    run.add_argument("--label", required=True, help="platform label: file name and report row")
    run.add_argument("--rounds", type=int, default=4, help="launches of each build")
    run.add_argument("--reps", type=int, default=3, help="measurements per configuration per launch")
    run.add_argument("--threads", help="thread counts, e.g. 1,2,4 (default: powers of two up to all)")
    run.add_argument("--scale", type=int, help="workload size multiplier")
    run.set_defaults(handler=cmd_run)

    summary = commands.add_parser("summary", help="merge per-platform results into one report")
    summary.add_argument("results", nargs="*", help="JSON files written by 'run'")
    summary.add_argument("--out", help="output Markdown file (default: stdout)")
    summary.add_argument("--expect", help="comma-separated labels that should be present")
    summary.set_defaults(handler=cmd_summary)

    # The tables contain '×' and '→'.  Under ctest on Windows stdout is a pipe
    # in the locale's code page, and printing would fail on the first of them.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")

    args = parser.parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
