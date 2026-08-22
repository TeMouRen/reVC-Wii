#!/usr/bin/env python3
"""Summarize one detailed Wii streaming lifecycle diagnostic run."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import report_streaming_baseline as baseline


SCHEMA_VERSION = 1
DETAIL_RE = re.compile(
    r"\[WII-STREAM-DIAG\] enabled state=(\d+)KB events=(\d+) "
    r"detail=([^ ]+) summary=(\d+)ms schema=([^ ]+)"
)
CHURN_RE = re.compile(
    r"\[WII-CHURN\] rank=(\d+) id=(-?\d+) type=([^ ]+) name='([^']*)' "
    r"trim=(\d+) r5=(\d+) r10=(\d+) load=(\d+) "
    r"gap=(\d+)/(\d+) queue=(\d+)/(\d+) service=(\d+)/(\d+) "
    r"reason=([^ ]+) req=([^ ]+) age=req(\d+)/load(\d+)/vis(\d+) "
    r"cf=(-?\d+)/(\d+) bypass=(\d+) loc=(-?\d+)/(-?\d+) dropped=(\d+)"
)
FRAME_HIST_RE = re.compile(
    r"\[WII-FRAME-HIST\] win=(\d+)ms frames=(\d+) risky=(\d+) "
    r"over40=(\d+) over50=(\d+) work=avg(\d+)/p95b(\d+)/max(\d+) "
    r"hist=(\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+) "
    r"stream=frames(\d+)/calls(\d+)/remove(\d+)/a(\d+)/p(\d+) "
    r"make=total(\d+)/max(\d+) removeUs=(\d+) "
    r"corr=stream40(\d+)/nostream40(\d+)"
)
LOAD_LAT_RE = re.compile(
    r"\[WII-LOAD-LAT\].* total=(\d+) queue=(\d+) service=(\d+)"
)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--route-id", required=True)
    parser.add_argument("--checkpoint", action="append", default=[])
    parser.add_argument("--expected-profile")
    parser.add_argument("--expected-build")
    parser.add_argument("--dol", required=True, type=Path)
    parser.add_argument("--notes")
    return parser.parse_args(argv)


def add_u32(current: int, value: int) -> int:
    return min(0xFFFFFFFF, current + value)


def parse_churn(lines: list[str]) -> tuple[list[dict[str, Any]], int]:
    resources: dict[int, dict[str, Any]] = {}
    dropped_max = 0
    for line in lines:
        match = CHURN_RE.search(line)
        if match is None:
            continue
        values = match.groups()
        stream_id = int(values[1])
        trim = int(values[4])
        reload5 = int(values[5])
        reload10 = int(values[6])
        loads = int(values[7])
        gap_average = int(values[8])
        queue_average = int(values[10])
        service_average = int(values[12])
        entry = resources.setdefault(
            stream_id,
            {
                "stream_id": stream_id,
                "type": values[2],
                "name": values[3],
                "trims": 0,
                "reload5": 0,
                "reload10": 0,
                "loads": 0,
                "reload_total_ms": 0,
                "reload_max_ms": 0,
                "queue_total_ms": 0,
                "queue_max_ms": 0,
                "service_total_ms": 0,
                "service_max_ms": 0,
                "class_bias_bypasses": 0,
                "last_trim_reason": "none",
                "last_request_class": "normal",
                "last_request_age_ms": 0xFFFFFFFF,
                "last_load_age_ms": 0xFFFFFFFF,
                "last_visible_age_ms": 0xFFFFFFFF,
                "last_global_oldest_id": -1,
                "last_global_oldest_age_ms": 0xFFFFFFFF,
                "last_level": -1,
                "last_area": -1,
            },
        )
        entry["trims"] += trim
        entry["reload5"] += reload5
        entry["reload10"] += reload10
        entry["loads"] += loads
        entry["reload_total_ms"] = add_u32(
            entry["reload_total_ms"], gap_average * reload10
        )
        entry["reload_max_ms"] = max(entry["reload_max_ms"], int(values[9]))
        entry["queue_total_ms"] = add_u32(
            entry["queue_total_ms"], queue_average * loads
        )
        entry["queue_max_ms"] = max(entry["queue_max_ms"], int(values[11]))
        entry["service_total_ms"] = add_u32(
            entry["service_total_ms"], service_average * loads
        )
        entry["service_max_ms"] = max(entry["service_max_ms"], int(values[13]))
        entry["class_bias_bypasses"] += int(values[21])
        entry["last_trim_reason"] = values[14]
        entry["last_request_class"] = values[15]
        entry["last_request_age_ms"] = int(values[16])
        entry["last_load_age_ms"] = int(values[17])
        entry["last_visible_age_ms"] = int(values[18])
        entry["last_global_oldest_id"] = int(values[19])
        entry["last_global_oldest_age_ms"] = int(values[20])
        entry["last_level"] = int(values[22])
        entry["last_area"] = int(values[23])
        dropped_max = max(dropped_max, int(values[24]))

    result = []
    for entry in resources.values():
        entry["reload_average_ms"] = (
            entry.pop("reload_total_ms") / entry["reload10"] if entry["reload10"] else 0
        )
        entry["queue_average_ms"] = (
            entry.pop("queue_total_ms") / entry["loads"] if entry["loads"] else 0
        )
        entry["service_average_ms"] = (
            entry.pop("service_total_ms") / entry["loads"] if entry["loads"] else 0
        )
        result.append(entry)
    result.sort(
        key=lambda entry: (
            entry["reload10"],
            entry["class_bias_bypasses"],
            entry["trims"],
            entry["service_max_ms"],
        ),
        reverse=True,
    )
    return result, dropped_max


def parse_frame_histograms(lines: list[str]) -> dict[str, Any]:
    totals: defaultdict[str, int] = defaultdict(int)
    maxima: defaultdict[str, int] = defaultdict(int)
    windows = 0
    for line in lines:
        match = FRAME_HIST_RE.search(line)
        if match is None:
            continue
        values = [int(value) for value in match.groups()]
        windows += 1
        totals["duration_ms"] += values[0]
        for key, index in {
            "frames": 1,
            "risky": 2,
            "over40": 3,
            "over50": 4,
            "stream_frames": 14,
            "stream_calls": 15,
            "removals": 16,
            "archive_removals": 17,
            "pressure_removals": 18,
            "make_total_us": 19,
            "removal_total_us": 21,
            "stream_over40": 22,
            "no_stream_over40": 23,
        }.items():
            totals[key] += values[index]
        maxima["work_p95_bucket_us"] = max(maxima["work_p95_bucket_us"], values[6])
        maxima["work_max_us"] = max(maxima["work_max_us"], values[7])
        maxima["make_frame_max_us"] = max(maxima["make_frame_max_us"], values[20])
        for index, value in enumerate(values[8:14]):
            totals[f"hist_{index}"] += value
    frames = totals["frames"]
    return {
        "window_count": windows,
        "duration_ms": totals["duration_ms"],
        "frames": frames,
        "risky_frames": totals["risky"],
        "over_40ms": totals["over40"],
        "over_50ms": totals["over50"],
        "work_p95_bucket_us_max": maxima["work_p95_bucket_us"],
        "work_max_us": maxima["work_max_us"],
        "work_histogram": [totals[f"hist_{index}"] for index in range(6)],
        "stream_frames": totals["stream_frames"],
        "stream_calls": totals["stream_calls"],
        "removals": totals["removals"],
        "archive_removals": totals["archive_removals"],
        "pressure_removals": totals["pressure_removals"],
        "make_total_us": totals["make_total_us"],
        "make_frame_max_us": maxima["make_frame_max_us"],
        "removal_total_us": totals["removal_total_us"],
        "stream_over_40ms": totals["stream_over40"],
        "no_stream_over_40ms": totals["no_stream_over40"],
        "stream_over_40_rate": (
            totals["stream_over40"] / totals["stream_frames"]
            if totals["stream_frames"]
            else 0
        ),
        "no_stream_over_40_rate": (
            totals["no_stream_over40"] / (frames - totals["stream_frames"])
            if frames > totals["stream_frames"]
            else 0
        ),
    }


def parse_latency_split(lines: list[str]) -> dict[str, Any]:
    checked = 0
    consistent = 0
    maximum_error_ms = 0
    for line in lines:
        match = LOAD_LAT_RE.search(line)
        if match is None:
            continue
        total_ms, queue_ms, service_ms = (int(value) for value in match.groups())
        if queue_ms == 0:
            continue
        checked += 1
        error_ms = abs(total_ms - queue_ms - service_ms)
        maximum_error_ms = max(maximum_error_ms, error_ms)
        if error_ms <= 4:
            consistent += 1
    return {
        "checked_events": checked,
        "consistent_events": consistent,
        "invalid_events": checked - consistent,
        "maximum_error_ms": maximum_error_ms,
        "valid": checked > 0 and checked == consistent,
    }


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    route_id = baseline.require_identifier(args.route_id, "route_id")
    checkpoints = [
        baseline.require_identifier(value, "checkpoint") for value in args.checkpoint
    ]
    parsed = baseline.parse_run(args.log)
    start = parsed.rows[0]
    profile_id = str(start["profile_id"])
    build_id = str(start["build_id"])
    if args.expected_profile and profile_id != args.expected_profile:
        raise baseline.ReportError(
            f"profile must be {args.expected_profile}, got {profile_id}"
        )
    if args.expected_build and build_id != args.expected_build:
        raise baseline.ReportError(f"build must be {args.expected_build}, got {build_id}")
    lines = parsed.text.splitlines()
    detail_matches = [match for line in lines if (match := DETAIL_RE.search(line))]
    if not detail_matches or detail_matches[-1].group(3) != "per-resource":
        raise baseline.ReportError("log is not a per-resource lifecycle diagnostic build")
    detail = detail_matches[-1]
    churn, churn_dropped = parse_churn(lines)
    frame_histograms = parse_frame_histograms(lines)
    latency_split = parse_latency_split(lines)
    selected = baseline.active_windows(parsed)
    base = baseline.summarize_run(parsed, selected)
    failure_free = all(value == 0 for value in base["failure_markers"].values())
    ready = (
        bool(churn)
        and frame_histograms["window_count"] > 0
        and base["game_loop_exited"]
        and base["splash_ready_count"] >= 2
        and failure_free
        and latency_split["valid"]
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "route_id": route_id,
        "route_checkpoints": checkpoints,
        "notes": args.notes,
        "profile_id": profile_id,
        "build_id": build_id,
        "dol_path": str(args.dol.resolve()),
        "dol_sha256": baseline.sha256_file(args.dol),
        "source_log": str(parsed.log_path),
        "source_log_size_bytes": parsed.log_size_bytes,
        "source_log_mtime_utc": parsed.log_mtime_utc,
        "diagnostic_contract": {
            "state_kib": int(detail.group(1)),
            "event_capacity": int(detail.group(2)),
            "detail": detail.group(3),
            "summary_ms": int(detail.group(4)),
            "schema": detail.group(5),
            "raw_trim_events": parsed.text.count("[WII-TRIM-VICTIM]"),
            "raw_reload_events": parsed.text.count("[WII-TRIM-RELOAD]"),
            "raw_load_events": parsed.text.count("[WII-LOAD-LAT]"),
            "churn_resource_count": len(churn),
            "churn_replacement_count_max": churn_dropped,
            "frame_histogram_windows": frame_histograms["window_count"],
            "latency_split": latency_split,
        },
        "readiness": {
            "ready": ready,
            "requires_fixed_route_completion": True,
            "performance_acceptance_allowed": False,
            "latency_split_valid": latency_split["valid"],
            "reason": (
                "detailed latency split is invalid; total must equal queue plus service"
                if not latency_split["valid"]
                else "detailed event logging is for root-cause attribution, not frame acceptance"
            ),
        },
        "baseline_interval": base,
        "top_churn": churn,
        "frame_histograms": frame_histograms,
    }


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        report = build_report(args)
        baseline.write_report(args.output, report)
    except baseline.ReportError as exc:
        print(f"streaming diagnostic report failed: {exc}", file=sys.stderr)
        return 1
    print(
        f"wrote {args.output}: churn={len(report['top_churn'])} "
        f"frame_windows={report['frame_histograms']['window_count']} "
        f"ready={report['readiness']['ready']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
