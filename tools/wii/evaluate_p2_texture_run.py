#!/usr/bin/env python3
"""Fail-closed runtime gate for P2 texture quality and allocator stability."""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Any

import extract_memory_sidecar as sidecar


P2_PROFILE_ID = "P2-gx-plus2m"
MIN_ROUTE_DURATION_MS = 5 * 60 * 1000
MAX_SHRINK_COUNT = 256
MAX_ALLOC_FAIL_COUNT = 128
MAX_LATE_SHRINK_DELTA = 32
MAX_LATE_ALLOC_FAIL_DELTA = 16
MAX_COMPACTION_DURATION_MS = 50
MIN_FINAL_GX_FREE_BYTES = 1024 * 1024
MIN_FINAL_GX_LARGEST_BYTES = 256 * 1024
MIN_RAW2_REMAINING_BYTES = 2 * 1024 * 1024
LATE_WINDOW_MS = 60 * 1000
BUILD_TIMESTAMP_RE = re.compile(r"-(\d{8}T\d{6}Z)$")
RUN_LOG_RE = re.compile(r"^dolphin-REVC02-\d{8}-\d{6}-\d+\.log$")
LOG_TIMESTAMP_RE = re.compile(r"^(\d+):(\d{2}):(\d{3})\s")


class EvaluationError(ValueError):
    pass


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Evaluate a user-operated P2 route for texture load failures, "
            "runaway shrinking, and exhausted GX headroom."
        )
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--log", type=Path, help="Run-specific Dolphin log")
    source.add_argument(
        "--latest",
        action="store_true",
        help="Use the newest run-specific log under %%APPDATA%%/Dolphin Emulator/Logs",
    )
    parser.add_argument(
        "--min-build-timestamp",
        metavar="YYYYMMDDTHHMMSSZ",
        help="Reject older builds so stale logs cannot satisfy the gate",
    )
    return parser.parse_args(argv)


def find_latest_log() -> Path:
    appdata = os.environ.get("APPDATA")
    if not appdata:
        raise EvaluationError("APPDATA is not set; pass --log explicitly")
    log_dir = Path(appdata) / "Dolphin Emulator" / "Logs"
    if not log_dir.is_dir():
        raise EvaluationError(f"Dolphin log directory does not exist: {log_dir}")
    candidates = [path for path in log_dir.iterdir() if path.is_file() and RUN_LOG_RE.fullmatch(path.name)]
    if not candidates:
        raise EvaluationError(f"no run-specific REVC02 log found under {log_dir}")
    return max(candidates, key=lambda path: path.stat().st_mtime_ns)


def load_run(log_path: Path) -> tuple[str, list[dict[str, Any]]]:
    if not log_path.is_file():
        raise EvaluationError(f"log does not exist: {log_path}")
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
        events = sidecar.extract_events(log_path)
        rows = sidecar.validate_and_materialize(events, log_path, log_path.stem)
    except (OSError, sidecar.SidecarValidationError) as exc:
        raise EvaluationError(str(exc)) from exc
    return text, rows


def build_timestamp(build_id: str) -> str | None:
    match = BUILD_TIMESTAMP_RE.search(build_id)
    return match.group(1) if match else None


def counter_delta(rows: list[dict[str, Any]], field: str, window_ms: int) -> int:
    final = rows[-1]
    cutoff = max(0, int(final["elapsed_ms"]) - window_ms)
    start = rows[0]
    for row in rows:
        if int(row["elapsed_ms"]) >= cutoff:
            start = row
            break
    return int(final[field]) - int(start[field])


def compaction_durations(text: str) -> tuple[int, list[int]]:
    pending_ms: int | None = None
    compact_count = 0
    durations: list[int] = []
    for line in text.splitlines():
        match = LOG_TIMESTAMP_RE.match(line)
        if match is None:
            continue
        timestamp_ms = (
            int(match.group(1)) * 60_000
            + int(match.group(2)) * 1_000
            + int(match.group(3))
        )
        if "[GX-POOL] pending compaction" in line:
            pending_ms = timestamp_ms
        elif "[GX-POOL] compacted gen=" in line:
            compact_count += 1
            if pending_ms is not None and timestamp_ms >= pending_ms:
                durations.append(timestamp_ms - pending_ms)
            pending_ms = None
    return compact_count, durations


def evaluate(
    log_path: Path,
    text: str,
    rows: list[dict[str, Any]],
    min_build_timestamp: str | None,
) -> tuple[list[str], str]:
    failures: list[str] = []
    start = rows[0]
    final = rows[-1]

    if start["profile_id"] != P2_PROFILE_ID:
        failures.append(f"profile must be {P2_PROFILE_ID}, got {start['profile_id']}")

    timestamp = build_timestamp(str(start["build_id"]))
    if min_build_timestamp is not None:
        if not re.fullmatch(r"\d{8}T\d{6}Z", min_build_timestamp):
            raise EvaluationError("--min-build-timestamp must use YYYYMMDDTHHMMSSZ")
        if timestamp is None:
            failures.append(f"build_id has no comparable timestamp: {start['build_id']}")
        elif timestamp < min_build_timestamp:
            failures.append(
                f"build {timestamp} is older than required {min_build_timestamp}"
            )

    duration_ms = int(final["elapsed_ms"])
    if duration_ms < MIN_ROUTE_DURATION_MS:
        failures.append(
            f"route duration {duration_ms}ms is below required {MIN_ROUTE_DURATION_MS}ms"
        )

    splash_ready_count = len(
        re.findall(r"\[WII-ISLAND\] splash prepared[^\r\n]*ready=1", text)
    )
    if splash_ready_count < 2:
        failures.append(f"expected two ready splash transitions, found {splash_ready_count}")
    if "Game loop exited." not in text:
        failures.append("game loop exit marker is missing")

    log_failure_markers = (
        ("APPLOADER ERROR", "Apploader boundary error"),
        ("[WII-MEM] GX allocation OOM", "GX allocation OOM"),
        ("[TXD-FAIL]", "TXD load failure"),
        ("[GX-SHRINK-FAULT]", "GX shrink integrity fault"),
    )
    for marker, label in log_failure_markers:
        count = text.count(marker)
        if count:
            failures.append(f"{label}: {count} occurrence(s)")

    max_shrinks = max(int(row["gx_shrink_count"]) for row in rows)
    max_alloc_fails = max(int(row["gx_alloc_fail_count"]) for row in rows)
    max_hard_fallbacks = max(int(row["hard_fallback_count"]) for row in rows)
    max_fallbacks = max(int(row["gx_fallback_count"]) for row in rows)
    min_raw2 = min(int(row["raw2_remaining"]) for row in rows)
    late_shrinks = counter_delta(rows, "gx_shrink_count", LATE_WINDOW_MS)
    late_alloc_fails = counter_delta(rows, "gx_alloc_fail_count", LATE_WINDOW_MS)
    compact_count, compact_durations_ms = compaction_durations(text)
    max_compact_ms = max(compact_durations_ms, default=0)

    if max_shrinks > MAX_SHRINK_COUNT:
        failures.append(f"gx_shrink_count {max_shrinks} exceeds {MAX_SHRINK_COUNT}")
    if max_alloc_fails > MAX_ALLOC_FAIL_COUNT:
        failures.append(
            f"gx_alloc_fail_count {max_alloc_fails} exceeds {MAX_ALLOC_FAIL_COUNT}"
        )
    if late_shrinks > MAX_LATE_SHRINK_DELTA:
        failures.append(
            f"last-minute shrink delta {late_shrinks} exceeds {MAX_LATE_SHRINK_DELTA}"
        )
    if late_alloc_fails > MAX_LATE_ALLOC_FAIL_DELTA:
        failures.append(
            f"last-minute alloc-fail delta {late_alloc_fails} exceeds {MAX_LATE_ALLOC_FAIL_DELTA}"
        )
    if max_compact_ms > MAX_COMPACTION_DURATION_MS:
        failures.append(
            f"max paired compaction duration {max_compact_ms}ms exceeds "
            f"{MAX_COMPACTION_DURATION_MS}ms"
        )
    if max_hard_fallbacks != 0:
        failures.append(f"hard_fallback_count must remain zero, got {max_hard_fallbacks}")
    if max_fallbacks != 0:
        failures.append(f"gx_fallback_count must remain zero, got {max_fallbacks}")
    if min_raw2 < MIN_RAW2_REMAINING_BYTES:
        failures.append(
            f"raw2_remaining fell to {min_raw2} bytes, below {MIN_RAW2_REMAINING_BYTES}"
        )
    if int(final["gx_free"]) < MIN_FINAL_GX_FREE_BYTES:
        failures.append(
            f"final gx_free {final['gx_free']} is below {MIN_FINAL_GX_FREE_BYTES}"
        )
    if int(final["gx_largest"]) < MIN_FINAL_GX_LARGEST_BYTES:
        failures.append(
            f"final gx_largest {final['gx_largest']} is below {MIN_FINAL_GX_LARGEST_BYTES}"
        )

    summary = (
        f"log={log_path} build={start['build_id']} duration={duration_ms}ms "
        f"splash_ready={splash_ready_count} shrink={max_shrinks} "
        f"alloc_fail={max_alloc_fails} late_shrink={late_shrinks} "
        f"late_alloc_fail={late_alloc_fails} final_gx_free={final['gx_free']} "
        f"final_gx_largest={final['gx_largest']} min_raw2={min_raw2} "
        f"compact_count={compact_count} max_compact_ms={max_compact_ms}"
    )
    return failures, summary


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        log_path = find_latest_log() if args.latest else args.log.resolve()
        text, rows = load_run(log_path)
        failures, summary = evaluate(log_path, text, rows, args.min_build_timestamp)
    except EvaluationError as exc:
        print(f"P2 texture run evaluation failed: {exc}", file=sys.stderr)
        return 1

    print(summary)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print(
        "P2 texture run automated gate passed; manual visual confirmation is still "
        "required for player, weapon, HUD, frontend, and world texture quality."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
