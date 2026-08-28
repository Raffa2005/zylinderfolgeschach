#!/usr/bin/env python3

"""Run and record one bounded ZFS engine experiment.

The script deliberately does not edit source or decide whether a result is good.
It provides a reproducible execution boundary for an agent or human researcher:
build, correctness tests, interleaved fixed-depth benchmarks, paired self-play,
and an append-only JSONL ledger.
"""

from __future__ import annotations

import argparse
import datetime as dt
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import statistics
import subprocess
import sys
from typing import Any, Sequence


BENCH_RE = re.compile(
    r"total nodes (?P<nodes>\d+) time (?P<time>\d+)ms nps (?P<nps>\d+) "
    r"signature 0x(?P<signature>[0-9a-f]{16})"
)


class ExperimentError(RuntimeError):
    pass


def command_text(command: Sequence[str]) -> str:
    return shlex.join(command)


def run(command: Sequence[str], *, cwd: Path, quiet: bool = False) -> str:
    if not quiet:
        print(f"$ {command_text(command)}", flush=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout, end="", file=sys.stderr)
        raise ExperimentError(
            f"command exited {completed.returncode}: {command_text(command)}"
        )
    if not quiet and completed.stdout:
        print(completed.stdout, end="")
    return completed.stdout


def executable(build: Path, name: str) -> Path:
    path = (build / name).resolve()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ExperimentError(f"missing executable: {path}")
    return path


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_frozen_assets(
    manifest_path: Path, baseline_build: Path, openings: Path, root: Path
) -> dict[str, Any]:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ExperimentError(f"cannot read baseline manifest: {error}") from error

    for name, expected in manifest["baseline_binaries"].items():
        path = executable(baseline_build, name)
        actual = sha256_file(path)
        if actual != expected:
            raise ExperimentError(f"frozen baseline asset changed: {path}")

    try:
        suite_name = str(openings.relative_to(root))
    except ValueError as error:
        raise ExperimentError("opening suite must be inside the repository") from error
    expected_suite = manifest["opening_suites"].get(suite_name)
    if expected_suite is None:
        raise ExperimentError(f"opening suite is not frozen in the manifest: {suite_name}")
    if sha256_file(openings) != expected_suite:
        raise ExperimentError(f"frozen opening suite changed: {openings}")
    return manifest


def affinity(command: list[str], core: int) -> list[str]:
    if core < 0:
        return command
    taskset = shutil.which("taskset")
    if taskset is None:
        raise ExperimentError("--core requires taskset")
    return [taskset, "-c", str(core), *command]


def bench_once(binary: Path, depth: int, core: int, root: Path) -> dict[str, Any]:
    output = run(
        affinity([str(binary), "--depth", str(depth)], core),
        cwd=root,
        quiet=True,
    )
    match = BENCH_RE.search(output)
    if match is None:
        raise ExperimentError(f"could not parse benchmark output from {binary}")
    return {
        "nodes": int(match.group("nodes")),
        "time_ms": int(match.group("time")),
        "nps": int(match.group("nps")),
        "signature": match.group("signature"),
    }


def bench_summary(samples: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "samples": samples,
        "median_time_ms": statistics.median(sample["time_ms"] for sample in samples),
        "median_nps": statistics.median(sample["nps"] for sample in samples),
        "nodes": sorted({sample["nodes"] for sample in samples}),
        "signatures": sorted({sample["signature"] for sample in samples}),
    }


def git_metadata(root: Path) -> dict[str, Any]:
    head = run(["git", "rev-parse", "HEAD"], cwd=root, quiet=True).strip()
    status = run(["git", "status", "--short"], cwd=root, quiet=True)
    diff = run(
        [
            "git", "diff", "--binary", "HEAD", "--", ".",
            ":(exclude)autoresearch/ledger.jsonl",
        ],
        cwd=root,
        quiet=True,
    )
    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout.split(b"\0")
    digest = hashlib.sha256(diff.encode())
    for raw_path in sorted(path for path in untracked if path):
        if raw_path == b"autoresearch/ledger.jsonl":
            continue
        path = root / os.fsdecode(raw_path)
        digest.update(b"untracked\0")
        digest.update(raw_path)
        digest.update(b"\0")
        digest.update(path.read_bytes())
    return {
        "head": head,
        "dirty": bool(status.strip()),
        "status": status.splitlines(),
        "source_change_sha256": digest.hexdigest(),
    }


def append_ledger(path: Path, record: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n"
    descriptor = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        remaining = memoryview(encoded.encode())
        while remaining:
            written = os.write(descriptor, remaining)
            if written == 0:
                raise ExperimentError(f"short write to ledger: {path}")
            remaining = remaining[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def parse_match(path: Path) -> dict[str, Any]:
    pairs: list[dict[str, Any]] = []
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            record = json.loads(line)
            if record.get("type") == "pair":
                pairs.append(record)
    if not pairs:
        raise ExperimentError("match produced no complete pairs")
    last = pairs[-1]
    points = sum(int(pair["candidate_half_points"]) for pair in pairs)
    score = points / (4.0 * len(pairs))
    if score <= 0.0 or score >= 1.0:
        elo = None
    else:
        elo = 400.0 * math.log10(score / (1.0 - score))
    return {
        "pairs": len(pairs),
        "pentanomial": last["pentanomial"],
        "candidate_score": score,
        "logistic_elo_point": elo,
        "llr": last["llr"],
        "decision": last["decision"],
    }


def validate_run_arguments(args: argparse.Namespace) -> None:
    if not re.fullmatch(r"[a-z0-9][a-z0-9_.-]{0,63}", args.name):
        raise ExperimentError("experiment name must be a short lowercase slug")
    if not 1 <= args.jobs <= max(1, os.cpu_count() or 1):
        raise ExperimentError("--jobs is outside the host CPU range")
    if not 1 <= args.bench_runs <= 20 or not 1 <= args.bench_depth <= 20:
        raise ExperimentError("benchmark limits are outside the safety range")
    if not 1 <= args.pairs <= 4096 or not 1 <= args.nodes <= 10_000_000:
        raise ExperimentError("match limits are outside the safety range")
    if not 0 <= args.movetime_ms <= 60_000:
        raise ExperimentError("--movetime-ms is outside the safety range")
    if not 1 <= args.hash_mb <= 1024 or not 1 <= args.max_plies <= 10_000:
        raise ExperimentError("hash or ply limit is outside the safety range")
    if args.core >= (os.cpu_count() or 1):
        raise ExperimentError("--core is outside the host CPU range")


def run_experiment(args: argparse.Namespace) -> int:
    validate_run_arguments(args)
    root = Path(__file__).resolve().parents[1]
    candidate_build = (root / args.candidate_build).resolve()
    baseline_build = (root / args.baseline_build).resolve()
    reference_build = (
        (root / args.reference_build).resolve()
        if args.reference_build is not None else baseline_build
    )
    openings = (root / args.openings).resolve()
    manifest_path = (root / args.manifest).resolve()
    ledger = (root / args.ledger).resolve()
    raw_directory = (root / args.raw_dir).resolve()
    if not openings.is_file():
        raise ExperimentError(f"missing opening suite: {openings}")
    manifest = verify_frozen_assets(manifest_path, baseline_build, openings, root)

    timestamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    record: dict[str, Any] = {
        "event": "experiment",
        "name": args.name,
        "started_utc": timestamp,
        "status": "running",
        "git": git_metadata(root),
        "configuration": {
            "core": args.core,
            "jobs": args.jobs,
            "bench_depth": args.bench_depth,
            "bench_runs": args.bench_runs,
            "openings": str(openings.relative_to(root)),
            "nodes": args.nodes,
            "movetime_ms": args.movetime_ms,
            "pairs": args.pairs,
            "hash_mb": args.hash_mb,
            "max_plies": args.max_plies,
            "frozen_baseline": manifest["id"],
            "reference_build": str(reference_build.relative_to(root)),
        },
    }

    try:
        run(
            [
                "cmake",
                "-S",
                str(root),
                "-B",
                str(candidate_build),
                "-DCMAKE_BUILD_TYPE=Release",
                "-DZFS_NATIVE=ON",
                "-DZFS_BUILD_TESTS=ON",
            ],
            cwd=root,
        )
        run(
            ["cmake", "--build", str(candidate_build), "-j", str(args.jobs)],
            cwd=root,
        )
        test_output = run(
            ["ctest", "--test-dir", str(candidate_build), "--output-on-failure"],
            cwd=root,
        )
        record["tests"] = {
            "status": "passed",
            "output_sha256": hashlib.sha256(test_output.encode()).hexdigest(),
        }

        baseline_bench = executable(reference_build, "zfs_bench")
        candidate_bench = executable(candidate_build, "zfs_bench")
        baseline_samples: list[dict[str, Any]] = []
        candidate_samples: list[dict[str, Any]] = []
        for index in range(args.bench_runs):
            order = (
                ((baseline_bench, baseline_samples),
                 (candidate_bench, candidate_samples))
                if index % 2 == 0
                else ((candidate_bench, candidate_samples),
                      (baseline_bench, baseline_samples))
            )
            for binary, samples in order:
                samples.append(bench_once(binary, args.bench_depth, args.core, root))
        record["benchmark"] = {
            "baseline": bench_summary(baseline_samples),
            "candidate": bench_summary(candidate_samples),
        }

        raw_directory.mkdir(parents=True, exist_ok=True)
        match_path = raw_directory / f"{timestamp}-{args.name}.jsonl"
        runner = executable(baseline_build, "zfs_match")
        baseline_engine = executable(reference_build, "zfs_engine")
        candidate_engine = executable(candidate_build, "zfs_engine")
        record["artifacts"] = {
            "openings_sha256": sha256_file(openings),
            "referee_sha256": sha256_file(runner),
            "reference_engine_sha256": sha256_file(baseline_engine),
            "candidate_engine_sha256": sha256_file(candidate_engine),
        }
        search_limit = (
            ["--movetime-ms", str(args.movetime_ms)]
            if args.movetime_ms > 0 else ["--nodes", str(args.nodes)]
        )
        match_command = affinity(
            [
                str(runner),
                "--candidate",
                str(candidate_engine),
                "--candidate-id",
                f"{record['git']['head']}:{record['git']['source_change_sha256']}",
                "--baseline",
                str(baseline_engine),
                "--baseline-id",
                args.baseline_id,
                "--openings",
                str(openings),
                "--output",
                str(match_path),
                *search_limit,
                "--pairs",
                str(args.pairs),
                "--hash-mb",
                str(args.hash_mb),
                "--max-plies",
                str(args.max_plies),
            ],
            args.core,
        )
        run(match_command, cwd=root)
        record["match"] = parse_match(match_path)
        record["match"]["raw_log"] = str(match_path.relative_to(root))
        record["match"]["raw_log_sha256"] = sha256_file(match_path)
        stats = record["match"]["pentanomial"]
        record["match"]["stats_output"] = run(
            [str(executable(baseline_build, "zfs_stats")), *map(str, stats)],
            cwd=root,
            quiet=True,
        ).strip()
        record["status"] = "measured"
    except Exception as error:
        record["status"] = "failed"
        record["error"] = str(error)
        record["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        append_ledger(ledger, record)
        raise

    record["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
    append_ledger(ledger, record)
    print(json.dumps(record, indent=2, sort_keys=True))
    return 0


def record_decision(args: argparse.Namespace) -> int:
    root = Path(__file__).resolve().parents[1]
    append_ledger(
        (root / args.ledger).resolve(),
        {
            "event": "decision",
            "experiment": args.name,
            "decision": args.decision,
            "reason": args.reason,
            "utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        },
    )
    return 0


def recover_match(args: argparse.Namespace) -> int:
    root = Path(__file__).resolve().parents[1]
    raw_path = (root / args.raw_log).resolve()
    try:
        relative = raw_path.relative_to(root)
    except ValueError as error:
        raise ExperimentError("raw match log must be inside the repository") from error
    with raw_path.open(encoding="utf-8") as stream:
        manifest = json.loads(stream.readline())
    if manifest.get("type") != "manifest":
        raise ExperimentError("raw match log has no manifest header")
    match = parse_match(raw_path)
    match["raw_log"] = str(relative)
    match["raw_log_sha256"] = sha256_file(raw_path)
    append_ledger(
        (root / args.ledger).resolve(),
        {
            "event": "recovered_match",
            "name": args.name,
            "candidate_id": manifest["candidate"]["id"],
            "reference_id": manifest["baseline"]["id"],
            "opening_fingerprint": manifest["openings"]["content_fnv1a64"],
            "limits": manifest["limit"],
            "match": match,
            "note": args.note,
            "recovered_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        },
    )
    print(json.dumps(match, indent=2, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    subparsers = result.add_subparsers(dest="command", required=True)
    run_parser = subparsers.add_parser("run", help="build, test, measure, and log")
    run_parser.add_argument("--name", required=True)
    run_parser.add_argument("--candidate-build", default="build-autoresearch-candidate")
    run_parser.add_argument("--baseline-build", default="build-autoresearch-baseline")
    run_parser.add_argument("--reference-build")
    run_parser.add_argument("--baseline-id", default="ddbcfad-zfs0")
    run_parser.add_argument("--manifest", default="autoresearch/baseline.json")
    run_parser.add_argument("--openings", default="openings/screen-v2.txt")
    run_parser.add_argument("--ledger", default="autoresearch/ledger.jsonl")
    run_parser.add_argument("--raw-dir", default="build-autoresearch-results")
    run_parser.add_argument("--core", type=int, default=-1)
    run_parser.add_argument("--jobs", type=int, default=4)
    run_parser.add_argument("--bench-depth", type=int, default=9)
    run_parser.add_argument("--bench-runs", type=int, default=4)
    run_parser.add_argument("--nodes", type=int, default=10_000)
    run_parser.add_argument("--movetime-ms", type=int, default=0)
    run_parser.add_argument("--pairs", type=int, default=32)
    run_parser.add_argument("--hash-mb", type=int, default=16)
    run_parser.add_argument("--max-plies", type=int, default=256)
    run_parser.set_defaults(function=run_experiment)

    decision = subparsers.add_parser("decide", help="append an accept/reject event")
    decision.add_argument("--name", required=True)
    decision.add_argument("--decision", choices=("accept", "reject", "defer"),
                          required=True)
    decision.add_argument("--reason", required=True)
    decision.add_argument("--ledger", default="autoresearch/ledger.jsonl")
    decision.set_defaults(function=record_decision)

    recover = subparsers.add_parser(
        "recover", help="record a separately validated completed match log"
    )
    recover.add_argument("--name", required=True)
    recover.add_argument("--raw-log", required=True)
    recover.add_argument("--note", required=True)
    recover.add_argument("--ledger", default="autoresearch/ledger.jsonl")
    recover.set_defaults(function=recover_match)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        return args.function(args)
    except ExperimentError as error:
        print(f"autoresearch: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
