#!/usr/bin/env python3
"""Benchmarks ./eind on a directory tree, isolated from the user's index.

    tools/bench.py ROOT [--binary ./eind] [--runs 30]

Builds an index of ROOT in a temporary directory, then reports the median wall
time of indexing and of a set of queries, plus peak RSS and, on macOS,
instructions retired (from /usr/bin/time -l). EIND_CONFIG, EIND_INDEX and
EIND_SOCKET point into the temporary directory, so neither the user's index
nor a running daemon is touched.
"""

import argparse
import os
import platform
import re
import shutil
import statistics
import subprocess
import tempfile
import time

QUERIES = {
    "relevance main": ["-n", "50", "-s", "relevance", "main"],
    "ext:json size:>10kb": ["-n", "50", "ext:json", "size:>10kb"],
    "*.go": ["-n", "50", "*.go"],
    "regex": ["-n", "50", r"regex:^[a-z]+_test\.go$"],
    "path:src/main": ["-n", "50", "path:src/main"],
    "--count e": ["--count", "e"],
}


def measure(cmd, env, runs):
    """Median wall seconds, max peak RSS bytes and median instructions (or None)."""
    walls, rss, instructions = [], [], []
    timer = ["/usr/bin/time", "-l"] if platform.system() == "Darwin" else ["/usr/bin/time", "-v"]
    for _ in range(runs):
        start = time.perf_counter()
        r = subprocess.run(timer + cmd, env=env, capture_output=True, text=True)
        walls.append(time.perf_counter() - start)
        if r.returncode != 0:
            raise SystemExit(f"{' '.join(cmd)} failed:\n{r.stderr}")
        m = re.search(r"(\d+)\s+maximum resident set size", r.stderr)  # bytes on macOS
        if m:
            rss.append(int(m.group(1)))
        m = re.search(r"Maximum resident set size \(kbytes\): (\d+)", r.stderr)  # GNU time
        if m:
            rss.append(int(m.group(1)) * 1024)
        m = re.search(r"(\d+)\s+instructions retired", r.stderr)
        if m:
            instructions.append(int(m.group(1)))
    return (statistics.median(walls), max(rss) if rss else None,
            statistics.median(instructions) if instructions else None)


def row(label, wall, rss, instructions):
    parts = [f"{label:24}", f"{wall * 1000:9.1f} ms"]
    if rss:
        parts.append(f"{rss / 2**20:7.1f} MB")
    if instructions:
        parts.append(f"{instructions / 1e6:10.1f} M instr")
    print("  ".join(parts))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--binary", default="./eind")
    ap.add_argument("--runs", type=int, default=30)
    args = ap.parse_args()
    binary = os.path.abspath(args.binary)
    tmp = tempfile.mkdtemp(prefix="eind-bench-")
    try:
        with open(os.path.join(tmp, "config"), "w") as f:
            f.write(f"root = {os.path.abspath(args.root)}\n")
        env = dict(os.environ, EIND_CONFIG=f"{tmp}/config", EIND_INDEX=f"{tmp}/index.bin",
                   EIND_SOCKET=f"{tmp}/s.sock", NO_COLOR="1")
        subprocess.run([binary, "index"], env=env, check=True, capture_output=True)
        entries = subprocess.run([binary, "--count", ""], env=env, capture_output=True, text=True).stdout.strip()
        print(f"{entries} entries under {args.root}; medians of {args.runs} runs (index: 5)\n")
        row("index", *measure([binary, "index"], env, 5))
        row("startup (--version)", *measure([binary, "--version"], env, args.runs))
        for label, q in QUERIES.items():
            row(label, *measure([binary] + q, env, args.runs))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
