#!/usr/bin/env python3
"""Compare existing Release fayasm_run builds using one identical WASM fixture.

Run from the repository root. Timings include process startup and parsing;
variants are rotated between repetitions and stdout must agree exactly.
No build directories are removed or configured by this script.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time


WORKLOADS = [
    ("mean(500k)", "sample_stats_mean", ["i32:500000"]),
    ("variance(500k)", "sample_stats_variance", ["i32:500000"]),
    ("linreg(500k)", "sample_linreg_slope", ["i32:500000"]),
    ("pearson(500k)", "sample_pearson_r", ["i32:500000"]),
    ("pi(500k)", "sample_monte_carlo_pi", ["i32:500000", "i32:42"]),
    ("sieve(100k)", "sample_prime_count", ["i32:100000"]),
    ("matrix(40)", "sample_matrix_checksum", ["i32:40"]),
    ("pipeline(100k)", "sample_stats_pipeline", ["i32:100000"]),
    ("startup", "sample_prime_count", ["i32:2"]),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", action="append", required=True, metavar="LABEL=BINARY")
    parser.add_argument("--fixture", type=Path, default=Path("wasm_samples/build/stats_suite.wasm"))
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--microcode", type=int, nargs="+", choices=[0, 1], default=[0, 1])
    parser.add_argument("--sequence-mode", action="append", default=[], metavar="LABEL=0|1",
                        help="Override serial execution for a named build (default: enabled)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    fixture = args.fixture.resolve(strict=True)
    variants = []
    sequence_modes = {}
    overrides = dict(spec.split("=", 1) for spec in args.sequence_mode)
    if any(value not in ("0", "1") for value in overrides.values()):
        parser.error("--sequence-mode must be LABEL=0 or LABEL=1")
    known = set()
    for spec in args.variant:
        label, binary = spec.split("=", 1)
        known.add(label)
        for mode in args.microcode:
            variants.append((f"{label}/mc{mode}", str(Path(binary).resolve(strict=True)), mode))
            sequence_modes[f"{label}/mc{mode}"] = int(overrides.get(label, "1"))
    if overrides.keys() - known:
        parser.error("--sequence-mode names an unknown variant")
    if len({v[0] for v in variants}) != len(variants):
        parser.error("variant labels and microcode modes must be unique")
    report = {"platform": platform.platform(), "fixture_sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
              "repeats": args.repeats, "timer": "perf_counter, whole process, seconds",
              "variants": {label: {"binary": binary, "microcode": mode,
                                   "sequences": sequence_modes[label]} for label, binary, mode in variants},
              "workloads": {}}
    for name, export, call_args in WORKLOADS:
        expected = None
        samples = {label: [] for label, _, _ in variants}
        # A warmup per variant is excluded; each measurement is a fresh process.
        for repeat in range(-1, args.repeats):
            shift = max(0, repeat) % len(variants)
            for label, binary, mode in variants[shift:] + variants[:shift]:
                env = dict(os.environ, FAYASM_MICROCODE=str(mode),
                           FAYASM_JIT_SEQUENCES=str(sequence_modes[label]),
                           FAYASM_JIT_PRESCAN="0", FAYASM_JIT_PRESCAN_FORCE="0")
                start = time.perf_counter()
                result = subprocess.run([binary, str(fixture), export, *call_args],
                                        env=env, capture_output=True, text=True, check=True)
                elapsed = time.perf_counter() - start
                if expected is None:
                    expected = result.stdout
                if result.stdout != expected:
                    raise RuntimeError(f"result mismatch: {name}, {label}: {result.stdout!r} != {expected!r}")
                if repeat >= 0:
                    samples[label].append(elapsed)
        report["workloads"][name] = {"export": export, "args": call_args, "result": expected.strip(),
            "samples": samples, "median": {k: statistics.median(v) for k, v in samples.items()},
            "best": {k: min(v) for k, v in samples.items()}}
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(name + ": " + ", ".join(f"{k}={statistics.median(v):.4f}s" for k, v in samples.items()), flush=True)
    print("\n| Workload | " + " | ".join(v[0] for v in variants) + " |")
    print("| --- | " + " | ".join("---:" for _ in variants) + " |")
    for name, data in report["workloads"].items():
        print("| " + name + " | " + " | ".join(f"{data['median'][v[0]] * 1000:.2f} ms" for v in variants) + " |")


if __name__ == "__main__":
    main()
