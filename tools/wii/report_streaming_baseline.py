#!/usr/bin/env python3
"""Create reproducible single-profile Wii streaming baseline reports."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

import extract_memory_sidecar as sidecar


SCHEMA_VERSION = 1
LOG_TIMESTAMP_RE = re.compile(r"^(\d+):(\d{2}):(\d{3})\s")
STREAM_DIAG_RE = re.compile(
    r"\[WII-STREAM-DIAG\] win=(\d+)ms trimEp=(\d+) victims=(\d+) "
    r"m/t/c/a=(\d+)/(\d+)/(\d+)/(\d+) reload5=(\d+) reload10=(\d+) "
    r"model=n(\d+)/avg(\d+)/p95b(\d+)/max(\d+) "
    r"txd=n(\d+)/avg(\d+)/p95b(\d+)/max(\d+) "
    r"radar=n(\d+)/avg(\d+)/p95b(\d+)/max(\d+) "
    r"pending=(\d+) evq=(\d+) dropped=(\d+)"
)
ARCHIVE_CEILING_RE = re.compile(
    r"\[WII-STREAM\] archive ceiling=(\d+)KB->(\d+)KB"
)
FRAME_WORK_RE = re.compile(r"\[WII-FRAME\].*?\bwork=([0-9]+(?:\.[0-9]+)?)ms")
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9._-]+$")


class ReportError(ValueError):
    pass


@dataclass(frozen=True)
class LatencySummary:
    count: int
    average_ms: int
    p95_bucket_ms: int
    maximum_ms: int


@dataclass(frozen=True)
class StreamWindow:
    end_ms: int
    duration_ms: int
    trim_epochs: int
    victims: int
    model_victims: int
    txd_victims: int
    col_victims: int
    anim_victims: int
    reload5: int
    reload10: int
    model: LatencySummary
    txd: LatencySummary
    radar: LatencySummary
    pending: int
    event_queue: int
    dropped: int

    @property
    def start_ms(self) -> int:
        return max(0, self.end_ms - self.duration_ms)


@dataclass(frozen=True)
class TimedValue:
    elapsed_ms: int
    value: float


@dataclass(frozen=True)
class CeilingChange:
    elapsed_ms: int
    before_kib: int
    after_kib: int


@dataclass
class ParsedRun:
    log_path: Path
    text: str
    rows: list[dict[str, Any]]
    windows: list[StreamWindow]
    frame_samples: list[TimedValue]
    ceiling_changes: list[CeilingChange]
    log_size_bytes: int
    log_mtime_utc: str


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build a normalized Wii streaming baseline from one or more "
            "user-operated run-specific Dolphin logs."
        )
    )
    parser.add_argument("--log", action="append", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--route-id", required=True)
    parser.add_argument("--checkpoint", action="append", required=True)
    parser.add_argument("--expected-profile")
    parser.add_argument("--expected-build")
    parser.add_argument("--active-duration-ms", type=int)
    parser.add_argument("--notes")
    dol = parser.add_mutually_exclusive_group(required=True)
    dol.add_argument("--dol", type=Path)
    dol.add_argument("--dol-sha256")
    return parser.parse_args(argv)


def require_identifier(value: str, label: str) -> str:
    if IDENTIFIER_RE.fullmatch(value) is None:
        raise ReportError(
            f"{label} may contain only A-Z, a-z, 0-9, dot, underscore, or hyphen"
        )
    return value


def log_timestamp_ms(line: str, line_number: int) -> int:
    match = LOG_TIMESTAMP_RE.match(line)
    if match is None:
        raise ReportError(f"line {line_number} has no Dolphin elapsed timestamp")
    return int(match.group(1)) * 60_000 + int(match.group(2)) * 1_000 + int(match.group(3))


def latency(values: tuple[str, ...], offset: int) -> LatencySummary:
    return LatencySummary(*(int(values[offset + index]) for index in range(4)))


def parse_stream_window(line: str, end_ms: int) -> StreamWindow | None:
    match = STREAM_DIAG_RE.search(line)
    if match is None:
        return None
    values = match.groups()
    return StreamWindow(
        end_ms=end_ms,
        duration_ms=int(values[0]),
        trim_epochs=int(values[1]),
        victims=int(values[2]),
        model_victims=int(values[3]),
        txd_victims=int(values[4]),
        col_victims=int(values[5]),
        anim_victims=int(values[6]),
        reload5=int(values[7]),
        reload10=int(values[8]),
        model=latency(values, 9),
        txd=latency(values, 13),
        radar=latency(values, 17),
        pending=int(values[21]),
        event_queue=int(values[22]),
        dropped=int(values[23]),
    )


def parse_run(log_path: Path) -> ParsedRun:
    log_path = log_path.resolve()
    if not log_path.is_file():
        raise ReportError(f"log does not exist: {log_path}")
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
        events = sidecar.extract_events(log_path)
        rows = sidecar.validate_and_materialize(events, log_path, log_path.stem)
    except (OSError, sidecar.SidecarValidationError) as exc:
        raise ReportError(str(exc)) from exc

    lines = text.splitlines()
    start_line = int(rows[0]["source_line"])
    if start_line < 1 or start_line > len(lines):
        raise ReportError("run_start source line is outside the log")
    run_start_ms = log_timestamp_ms(lines[start_line - 1], start_line)

    windows: list[StreamWindow] = []
    frame_samples: list[TimedValue] = []
    ceiling_changes: list[CeilingChange] = []
    for line_number, line in enumerate(lines, start=1):
        if "[WII-STREAM-DIAG] win=" not in line and "[WII-FRAME]" not in line and "archive ceiling=" not in line:
            continue
        absolute_ms = log_timestamp_ms(line, line_number)
        elapsed_ms = absolute_ms - run_start_ms
        if elapsed_ms < 0:
            continue
        window = parse_stream_window(line, elapsed_ms)
        if window is not None:
            windows.append(window)
            continue
        frame_match = FRAME_WORK_RE.search(line)
        if frame_match is not None:
            frame_samples.append(TimedValue(elapsed_ms, float(frame_match.group(1))))
            continue
        ceiling_match = ARCHIVE_CEILING_RE.search(line)
        if ceiling_match is not None:
            ceiling_changes.append(
                CeilingChange(elapsed_ms, int(ceiling_match.group(1)), int(ceiling_match.group(2)))
            )

    if not windows:
        raise ReportError(f"no [WII-STREAM-DIAG] summary windows found in {log_path}")
    if not any(window.victims > 0 for window in windows):
        raise ReportError(f"no nonzero trim window found in {log_path}")

    stat = log_path.stat()
    return ParsedRun(
        log_path=log_path,
        text=text,
        rows=rows,
        windows=windows,
        frame_samples=frame_samples,
        ceiling_changes=ceiling_changes,
        log_size_bytes=stat.st_size,
        log_mtime_utc=datetime.fromtimestamp(stat.st_mtime, timezone.utc).isoformat(),
    )


def active_windows(run: ParsedRun) -> list[StreamWindow]:
    first = next(index for index, window in enumerate(run.windows) if window.victims > 0)
    return run.windows[first:]


def select_duration(windows: list[StreamWindow], limit_ms: int) -> list[StreamWindow]:
    selected: list[StreamWindow] = []
    accumulated = 0
    for window in windows:
        if accumulated + window.duration_ms > limit_ms:
            break
        selected.append(window)
        accumulated += window.duration_ms
    if not selected:
        raise ReportError(
            f"active duration {limit_ms}ms is shorter than the first complete summary window"
        )
    return selected


def weighted_average(windows: Iterable[StreamWindow], field: str) -> float:
    total_count = 0
    weighted_total = 0
    for window in windows:
        summary = getattr(window, field)
        total_count += summary.count
        weighted_total += summary.count * summary.average_ms
    return weighted_total / total_count if total_count else 0.0


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(len(ordered) * fraction) - 1)
    return ordered[index]


def memory_summary(rows: list[dict[str, Any]]) -> dict[str, int]:
    return {
        "generic_free_min_bytes": min(int(row["generic_free"]) for row in rows),
        "generic_largest_min_bytes": min(int(row["generic_largest"]) for row in rows),
        "raw2_remaining_min_bytes": min(int(row["raw2_remaining"]) for row in rows),
        "gx_free_min_bytes": min(int(row["gx_free"]) for row in rows),
        "gx_largest_min_bytes": min(int(row["gx_largest"]) for row in rows),
        "gx_shrink_max": max(int(row["gx_shrink_count"]) for row in rows),
        "gx_alloc_fail_max": max(int(row["gx_alloc_fail_count"]) for row in rows),
        "gx_fallback_max": max(int(row["gx_fallback_count"]) for row in rows),
        "hard_fallback_max": max(int(row["hard_fallback_count"]) for row in rows),
    }


def frame_summary(samples: list[TimedValue]) -> dict[str, Any]:
    values = [sample.value for sample in samples]
    return {
        "scope": "diagnostic-log-samples-not-all-frames",
        "sample_count": len(values),
        "work_p95_ms": percentile(values, 0.95),
        "work_max_ms": max(values) if values else None,
        "work_over_40ms": sum(value > 40.0 for value in values),
        "work_over_50ms": sum(value > 50.0 for value in values),
        "histogram": {
            "le_33_37ms": sum(value <= 33.37 for value in values),
            "gt_33_37_le_40ms": sum(33.37 < value <= 40.0 for value in values),
            "gt_40_le_50ms": sum(40.0 < value <= 50.0 for value in values),
            "gt_50ms": sum(value > 50.0 for value in values),
        },
    }


def ceiling_summary(changes: list[CeilingChange]) -> dict[str, Any]:
    if not changes:
        return {
            "change_count": 0,
            "grow_count": 0,
            "retreat_count": 0,
            "first_kib": None,
            "final_kib": None,
            "minimum_kib": None,
            "maximum_kib": None,
        }
    values = [changes[0].before_kib] + [change.after_kib for change in changes]
    return {
        "change_count": len(changes),
        "grow_count": sum(change.after_kib > change.before_kib for change in changes),
        "retreat_count": sum(change.after_kib < change.before_kib for change in changes),
        "first_kib": changes[0].before_kib,
        "final_kib": changes[-1].after_kib,
        "minimum_kib": min(values),
        "maximum_kib": max(values),
    }


def summarize_run(run: ParsedRun, selected: list[StreamWindow]) -> dict[str, Any]:
    active_start_ms = selected[0].start_ms
    active_end_ms = selected[-1].end_ms
    duration_ms = sum(window.duration_ms for window in selected)
    interval_rows = [
        row for row in run.rows if active_start_ms <= int(row["elapsed_ms"]) <= active_end_ms
    ]
    if not interval_rows:
        raise ReportError(f"no memory snapshots in active interval for {run.log_path}")
    frames = [
        sample for sample in run.frame_samples if active_start_ms <= sample.elapsed_ms <= active_end_ms
    ]
    ceilings = [
        change for change in run.ceiling_changes if active_start_ms <= change.elapsed_ms <= active_end_ms
    ]
    seconds = duration_ms / 1000.0
    victims = sum(window.victims for window in selected)
    reload5 = sum(window.reload5 for window in selected)
    reload10 = sum(window.reload10 for window in selected)
    start = run.rows[0]
    return {
        "source_log": str(run.log_path),
        "source_log_size_bytes": run.log_size_bytes,
        "source_log_mtime_utc": run.log_mtime_utc,
        "profile_id": start["profile_id"],
        "build_id": start["build_id"],
        "run_duration_ms": int(run.rows[-1]["elapsed_ms"]),
        "active_start_ms": active_start_ms,
        "active_end_ms": active_end_ms,
        "active_duration_ms": duration_ms,
        "summary_window_count": len(selected),
        "splash_ready_count": len(
            re.findall(r"\[WII-ISLAND\] splash prepared[^\r\n]*ready=1", run.text)
        ),
        "game_loop_exited": "Game loop exited." in run.text,
        "apploader_error_count": run.text.count("APPLOADER ERROR"),
        "failure_markers": {
            "apploader_error": run.text.count("APPLOADER ERROR"),
            "gx_allocation_oom": run.text.count("[WII-MEM] GX allocation OOM"),
            "txd_fail": run.text.count("[TXD-FAIL]"),
            "gx_shrink_fault": run.text.count("[GX-SHRINK-FAULT]"),
        },
        "streaming": {
            "victims": victims,
            "victims_per_second": victims / seconds,
            "victim_types": {
                "model": sum(window.model_victims for window in selected),
                "txd": sum(window.txd_victims for window in selected),
                "col": sum(window.col_victims for window in selected),
                "anim": sum(window.anim_victims for window in selected),
            },
            "reload5": reload5,
            "reload5_per_second": reload5 / seconds,
            "reload10": reload10,
            "reload10_per_second": reload10 / seconds,
            "model_load_count": sum(window.model.count for window in selected),
            "model_weighted_average_ms": weighted_average(selected, "model"),
            "model_max_window_p95_bucket_ms": max(window.model.p95_bucket_ms for window in selected),
            "model_max_ms": max(window.model.maximum_ms for window in selected),
            "txd_load_count": sum(window.txd.count for window in selected),
            "txd_weighted_average_ms": weighted_average(selected, "txd"),
            "txd_max_window_p95_bucket_ms": max(window.txd.p95_bucket_ms for window in selected),
            "txd_max_ms": max(window.txd.maximum_ms for window in selected),
            "pending_max": max(window.pending for window in selected),
            "event_queue_max": max(window.event_queue for window in selected),
            "dropped_total": sum(window.dropped for window in selected),
        },
        "memory": memory_summary(run.rows),
        "memory_active_interval": memory_summary(interval_rows),
        "archive_ceiling": ceiling_summary(run.ceiling_changes),
        "archive_ceiling_active_interval": ceiling_summary(ceilings),
        "frame_samples": frame_summary(frames),
        "frame_samples_full_run": frame_summary(run.frame_samples),
    }


def nested_number(value: dict[str, Any], path: str) -> float:
    current: Any = value
    for key in path.split("."):
        current = current[key]
    if not isinstance(current, (int, float)):
        raise ReportError(f"aggregate metric {path} is not numeric")
    return float(current)


def aggregate_metric(runs: list[dict[str, Any]], path: str, worst: str) -> dict[str, float]:
    values = [nested_number(run, path) for run in runs]
    return {
        "median": float(statistics.median(values)),
        "minimum": min(values),
        "maximum": max(values),
        "worst": min(values) if worst == "min" else max(values),
    }


def aggregate_runs(runs: list[dict[str, Any]]) -> dict[str, Any]:
    higher_is_worse = (
        "streaming.victims_per_second",
        "streaming.reload5_per_second",
        "streaming.reload10_per_second",
        "streaming.model_weighted_average_ms",
        "streaming.model_max_ms",
        "streaming.txd_weighted_average_ms",
        "streaming.txd_max_ms",
        "streaming.pending_max",
        "memory.gx_shrink_max",
        "memory.gx_alloc_fail_max",
        "memory.gx_fallback_max",
        "memory.hard_fallback_max",
    )
    lower_is_worse = (
        "memory.generic_free_min_bytes",
        "memory.generic_largest_min_bytes",
        "memory.raw2_remaining_min_bytes",
        "memory.gx_free_min_bytes",
        "memory.gx_largest_min_bytes",
    )
    return {
        path: aggregate_metric(runs, path, "max") for path in higher_is_worse
    } | {
        path: aggregate_metric(runs, path, "min") for path in lower_is_worse
    }


def sha256_file(path: Path) -> str:
    path = path.resolve()
    if not path.is_file():
        raise ReportError(f"DOL does not exist: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def write_report(output: Path, report: dict[str, Any]) -> None:
    output = output.resolve()
    if output.exists():
        raise ReportError(f"refusing to overwrite existing output: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.open("x", encoding="utf-8", newline="\n") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)
            handle.write("\n")
    except OSError as exc:
        output.unlink(missing_ok=True)
        raise ReportError(str(exc)) from exc


def build_report(args: argparse.Namespace) -> dict[str, Any]:
    route_id = require_identifier(args.route_id, "route_id")
    checkpoints = [require_identifier(value, "checkpoint") for value in args.checkpoint]
    if len(set(checkpoints)) != len(checkpoints):
        raise ReportError("route checkpoints must be unique")
    if args.active_duration_ms is not None and args.active_duration_ms <= 0:
        raise ReportError("--active-duration-ms must be positive")

    log_paths = [path.resolve() for path in args.log]
    if len(set(log_paths)) != len(log_paths):
        raise ReportError("duplicate --log paths are not allowed")
    parsed = [parse_run(path) for path in log_paths]
    profiles = {str(run.rows[0]["profile_id"]) for run in parsed}
    builds = {str(run.rows[0]["build_id"]) for run in parsed}
    if len(profiles) != 1:
        raise ReportError(f"logs contain multiple profiles: {sorted(profiles)}")
    if len(builds) != 1:
        raise ReportError(f"logs contain multiple builds: {sorted(builds)}")
    profile_id = next(iter(profiles))
    build_id = next(iter(builds))
    if args.expected_profile is not None and profile_id != args.expected_profile:
        raise ReportError(f"profile must be {args.expected_profile}, got {profile_id}")
    if args.expected_build is not None and build_id != args.expected_build:
        raise ReportError(f"build must be {args.expected_build}, got {build_id}")

    active_sets = [active_windows(run) for run in parsed]
    available = [sum(window.duration_ms for window in windows) for windows in active_sets]
    if args.active_duration_ms is not None and any(
        duration < args.active_duration_ms for duration in available
    ):
        raise ReportError(
            f"requested active duration {args.active_duration_ms}ms exceeds available durations {available}"
        )
    if args.active_duration_ms is not None:
        selected = [select_duration(windows, args.active_duration_ms) for windows in active_sets]
    else:
        common_window_count = min(len(windows) for windows in active_sets)
        selected = [windows[:common_window_count] for windows in active_sets]
    summaries = [summarize_run(run, windows) for run, windows in zip(parsed, selected)]
    active_durations = [int(run["active_duration_ms"]) for run in summaries]
    selected_window_count = min(int(run["summary_window_count"]) for run in summaries)

    if args.dol is not None:
        dol_sha256 = sha256_file(args.dol)
        dol_path: str | None = str(args.dol.resolve())
    else:
        if SHA256_RE.fullmatch(args.dol_sha256 or "") is None:
            raise ReportError("--dol-sha256 must contain exactly 64 hexadecimal digits")
        dol_sha256 = str(args.dol_sha256).upper()
        dol_path = None

    return {
        "schema_version": SCHEMA_VERSION,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "route_id": route_id,
        "route_checkpoints": checkpoints,
        "notes": args.notes,
        "profile_id": profile_id,
        "build_id": build_id,
        "dol_path": dol_path,
        "dol_sha256": dol_sha256,
        "run_count": len(summaries),
        "comparison_readiness": {
            "required_matched_runs": 3,
            "ready": len(summaries) >= 3,
            "manual_visual_confirmation_required": True,
        },
        "normalization": {
            "start_rule": "first-complete-summary-window-with-nonzero-victims",
            "selection_rule": (
                "complete-windows-within-explicit-duration"
                if args.active_duration_ms is not None
                else "equal-complete-window-count"
            ),
            "target_active_duration_ms": args.active_duration_ms,
            "summary_window_count": selected_window_count,
            "active_duration_ms_min": min(active_durations),
            "active_duration_ms_max": max(active_durations),
            "available_active_duration_ms": available,
        },
        "runs": summaries,
        "aggregate": aggregate_runs(summaries),
    }


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        report = build_report(args)
        write_report(args.output, report)
    except ReportError as exc:
        print(f"streaming baseline report failed: {exc}", file=sys.stderr)
        return 1

    aggregate = report["aggregate"]
    print(
        f"wrote {args.output.resolve()} profile={report['profile_id']} "
        f"build={report['build_id']} runs={report['run_count']} "
        f"active={report['normalization']['active_duration_ms_min']}.."
        f"{report['normalization']['active_duration_ms_max']}ms"
    )
    print(
        "median "
        f"victims/s={aggregate['streaming.victims_per_second']['median']:.3f} "
        f"reload5/s={aggregate['streaming.reload5_per_second']['median']:.3f} "
        f"reload10/s={aggregate['streaming.reload10_per_second']['median']:.3f} "
        f"model={aggregate['streaming.model_weighted_average_ms']['median']:.1f}ms "
        f"txd={aggregate['streaming.txd_weighted_average_ms']['median']:.1f}ms"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
