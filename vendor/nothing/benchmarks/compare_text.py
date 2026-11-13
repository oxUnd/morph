#!/usr/bin/env python3
"""Compare two non-LTO builds of bench_text.c using identical workloads."""
import argparse
import csv
import io
from pathlib import Path
import statistics
import subprocess


def run(binary, size):
    result = subprocess.run([str(binary), str(size)], check=True,
                            capture_output=True, text=True)
    return list(csv.DictReader(io.StringIO(result.stdout)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--sizes", nargs="+", type=int,
                        default=[16, 64, 1024, 65536, 4194304])
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--output", type=Path, default=Path("text-results.csv"))
    args = parser.parse_args()
    if args.rounds < 3 or any(size < 1 or size > 67108864 for size in args.sizes):
        parser.error("use at least three rounds and sizes in 1..67108864")
    binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
    raw = []
    samples = {}
    for size in args.sizes:
        expected = None
        for trial in range(-1, args.rounds):
            order = ("before", "after") if trial % 2 else ("after", "before")
            for phase in order:
                rows = run(binaries[phase], size)
                signature = [(row["scenario"], row["input_bytes"],
                              row["iterations"], row["checksum"]) for row in rows]
                if expected is None:
                    expected = signature
                if signature != expected:
                    raise RuntimeError(f"workload/output mismatch: {size}, {trial}, {phase}")
                for row in rows:
                    raw.append(dict(phase=phase, trial=trial, raw_bytes=size, **row))
                    if trial >= 0:
                        key = (size, row["scenario"], int(row["input_bytes"]), phase)
                        samples.setdefault(key, []).append(float(row["ns_per_call"]))
        print(f"completed {size} bytes ({args.rounds} measured rounds)", flush=True)
    raw_path = args.output.with_name(args.output.stem + "-raw.csv")
    with raw_path.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(raw[0]))
        writer.writeheader()
        writer.writerows(raw)
    summary = []
    for (size, scenario, input_bytes, phase), timings in samples.items():
        if phase != "before":
            continue
        after = samples[(size, scenario, input_bytes, "after")]
        before_ns = statistics.median(timings)
        after_ns = statistics.median(after)
        summary.append(dict(
            raw_bytes=size, scenario=scenario, input_bytes=input_bytes,
            before_ns=f"{before_ns:.3f}", after_ns=f"{after_ns:.3f}",
            before_GB_per_s=f"{input_bytes / before_ns:.6f}",
            after_GB_per_s=f"{input_bytes / after_ns:.6f}",
            speedup=f"{before_ns / after_ns:.3f}",
            before_cv_percent=f"{100 * statistics.pstdev(timings) / statistics.mean(timings):.2f}",
            after_cv_percent=f"{100 * statistics.pstdev(after) / statistics.mean(after):.2f}",
        ))
    with args.output.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)


if __name__ == "__main__":
    main()
