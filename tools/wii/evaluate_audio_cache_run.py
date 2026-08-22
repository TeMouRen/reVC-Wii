#!/usr/bin/env python3
"""Fail-closed runtime gate for the bounded A0 audio cache profile."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
from pathlib import Path
from typing import Any

import extract_memory_sidecar as sidecar


RUN_LOG_RE = re.compile(r"^dolphin-REVC02-\d{8}-\d{6}-\d+\.log$")
POOL_RE = re.compile(
    r"\[WII-AUDIO-POOL\]\s+cap=(\d+)KB used=(\d+)KB peak=(\d+)KB "
    r"free=(\d+)KB largest=(\d+)KB denied=(\d+)"
)
CACHE_RE = re.compile(
    r"\[WII-AUDIO-CACHE\]\s+hits=(\d+) misses=(\d+) evict=(\d+) queued=(\d+) "
    r"decode=(\d+) pin=(\d+) pinned=(\d+)KB peak=(\d+)KB denied=(\d+)"
)
DECODE_RE = re.compile(
    r"\[WII-AUDIO-DECODE\]\s+count=(\d+) queue=avg(\d+)/max(\d+) "
    r"service=avg(\d+)/max(\d+) total=avg(\d+)/max(\d+)"
)
DMA_RE = re.compile(
    r"\[WII-AUDIO-DMA\]\s+callbacks=(\d+) underrun=(\d+) silence=(\d+)"
)
ARCHIVE_CEILING_RE = re.compile(
    r"\[WII-STREAM\]\s+archive ceiling=\d+KB->(\d+)KB"
)
STREAM_DIAG_RE = re.compile(
    r"\[WII-STREAM-DIAG\][^\r\n]*model=n(\d+)/avg(\d+)/p95b\d+/max\d+ "
    r"txd=n(\d+)/avg(\d+)/p95b\d+/max\d+"
)
RESIDENT_RE = re.compile(r"\[WII-RESIDENT\]\s+live=L(\d+)")
ISLAND_COMMIT_RE = re.compile(r"\[WII-ISLAND\]\s+commit[^\r\n]*dt=(\d+)ms")

MIN_ROUTE_DURATION_MS = 5 * 60 * 1000
EXPECTED_AUDIO_POOL_CAPACITY_KIB = 4096
MIN_FINAL_AUDIO_FREE_KIB = 512
MIN_AUDIO_CACHE_DECODES = 1
MIN_RAW2_REMAINING_BYTES = 4 * 1024 * 1024
MIN_FINAL_GX_FREE_BYTES = 1024 * 1024
MIN_FINAL_GX_LARGEST_BYTES = 256 * 1024
MIN_FINAL_GENERIC_FREE_BYTES = 256 * 1024
MIN_FINAL_GENERIC_LARGEST_BYTES = 128 * 1024
MIN_ARCHIVE_CEILING_KIB = 28 * 1024
MIN_FINAL_RESIDENT_LIVE = 980
MAX_MODEL_AVG_MS = 800
MAX_TXD_AVG_MS = 1050
MAX_ISLAND_COMMIT_MS = 8500


class EvaluationError(ValueError):
    pass


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--log", type=Path, help="Run-specific Dolphin log")
    source.add_argument(
        "--latest",
        action="store_true",
        help="Use the newest run-specific log under %%APPDATA%%/Dolphin Emulator/Logs",
    )
    parser.add_argument("--expected-profile", required=True)
    parser.add_argument("--expected-build")
    parser.add_argument("--dol", type=Path)
    parser.add_argument("--expected-dol-sha256")
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


def parse_last_match(pattern: re.Pattern[str], text: str, label: str) -> tuple[int, ...]:
    matches = [match for line in text.splitlines() if (match := pattern.search(line))]
    if not matches:
        raise EvaluationError(f"missing {label} telemetry")
    return tuple(int(value) for value in matches[-1].groups())


def sha256_file(path: Path) -> str:
    path = path.resolve()
    if not path.is_file():
        raise EvaluationError(f"DOL does not exist: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def evaluate(
    log_path: Path,
    text: str,
    rows: list[dict[str, Any]],
    *,
    expected_profile: str,
    expected_build: str | None,
    dol: Path | None,
    expected_dol_sha256: str | None,
) -> tuple[list[str], str]:
    start = rows[0]
    final = rows[-1]
    failures: list[str] = []

    if str(start["profile_id"]) != expected_profile:
        failures.append(f"profile must be {expected_profile}, got {start['profile_id']}")
    if expected_build is not None and str(start["build_id"]) != expected_build:
        failures.append(f"build must be {expected_build}, got {start['build_id']}")

    if expected_dol_sha256 is not None:
        if dol is None:
            raise EvaluationError("--expected-dol-sha256 requires --dol")
        if not re.fullmatch(r"[0-9A-Fa-f]{64}", expected_dol_sha256):
            raise EvaluationError("--expected-dol-sha256 must contain 64 hexadecimal digits")
        actual_dol_sha256 = sha256_file(dol)
        if actual_dol_sha256 != expected_dol_sha256.upper():
            failures.append(
                f"DOL sha256 must be {expected_dol_sha256.upper()}, got {actual_dol_sha256}"
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

    pool_cap_kib, pool_used_kib, pool_peak_kib, pool_free_kib, pool_largest_kib, pool_denied = parse_last_match(
        POOL_RE, text, "[WII-AUDIO-POOL]"
    )
    hits, misses, evict, queued, decode_count, pin_count, pinned_kib, cache_peak_kib, cache_denied = parse_last_match(
        CACHE_RE, text, "[WII-AUDIO-CACHE]"
    )
    (
        decode_events,
        queue_avg_ms,
        queue_max_ms,
        service_avg_ms,
        service_max_ms,
        total_avg_ms,
        total_max_ms,
    ) = parse_last_match(DECODE_RE, text, "[WII-AUDIO-DECODE]")
    dma_callbacks, dma_underrun, dma_silence = parse_last_match(
        DMA_RE, text, "[WII-AUDIO-DMA]"
    )
    archive_ceilings = [int(match.group(1)) for match in ARCHIVE_CEILING_RE.finditer(text)]
    stream_windows = [tuple(int(value) for value in match.groups()) for match in STREAM_DIAG_RE.finditer(text)]
    resident_live = [int(match.group(1)) for match in RESIDENT_RE.finditer(text)]
    island_commit_ms = [int(match.group(1)) for match in ISLAND_COMMIT_RE.finditer(text)]

    max_archive_ceiling_kib = max(archive_ceilings, default=0)
    final_archive_ceiling_kib = archive_ceilings[-1] if archive_ceilings else 0
    model_events = sum(window[0] for window in stream_windows)
    txd_events = sum(window[2] for window in stream_windows)
    model_avg_ms = (
        sum(window[0] * window[1] for window in stream_windows) // model_events
        if model_events
        else 0
    )
    txd_avg_ms = (
        sum(window[2] * window[3] for window in stream_windows) // txd_events
        if txd_events
        else 0
    )
    final_resident_live = resident_live[-1] if resident_live else 0
    max_island_commit_ms = max(island_commit_ms, default=0)

    if pool_cap_kib != EXPECTED_AUDIO_POOL_CAPACITY_KIB:
        failures.append(
            f"audio pool capacity must be {EXPECTED_AUDIO_POOL_CAPACITY_KIB}KB, got {pool_cap_kib}KB"
        )
    if pool_used_kib > pool_cap_kib:
        failures.append(f"audio pool used {pool_used_kib}KB exceeds capacity {pool_cap_kib}KB")
    if pool_peak_kib > pool_cap_kib:
        failures.append(f"audio pool peak {pool_peak_kib}KB exceeds capacity {pool_cap_kib}KB")
    if pool_largest_kib > pool_cap_kib:
        failures.append(
            f"audio pool largest free block {pool_largest_kib}KB exceeds capacity {pool_cap_kib}KB"
        )
    if pool_free_kib < MIN_FINAL_AUDIO_FREE_KIB:
        failures.append(
            f"audio pool final free {pool_free_kib}KB is below {MIN_FINAL_AUDIO_FREE_KIB}KB"
        )
    if pool_denied != 0:
        failures.append(f"audio pool allocation denied count must remain zero, got {pool_denied}")
    if cache_denied != 0:
        failures.append(f"audio cache allocation denied count must remain zero, got {cache_denied}")
    if decode_count < MIN_AUDIO_CACHE_DECODES or decode_events < MIN_AUDIO_CACHE_DECODES:
        failures.append("audio cache never decoded a streamed sample")
    if misses < 1:
        failures.append("audio cache must record at least one miss to prove cold-path coverage")
    if hits < 1:
        failures.append("audio cache must record at least one hit to prove reuse")
    if decode_events != decode_count:
        failures.append(
            f"audio decode event count {decode_events} must match cache decode count {decode_count}"
        )
    if total_avg_ms < queue_avg_ms + service_avg_ms:
        failures.append(
            f"audio decode total avg {total_avg_ms}ms is below queue+service {queue_avg_ms + service_avg_ms}ms"
        )
    if total_max_ms < max(queue_max_ms, service_max_ms):
        failures.append(
            f"audio decode total max {total_max_ms}ms is below a component max "
            f"{max(queue_max_ms, service_max_ms)}ms"
        )
    if dma_callbacks < 1:
        failures.append("audio DMA callback count must be nonzero")
    if dma_underrun != 0:
        failures.append(f"audio DMA underrun count must remain zero, got {dma_underrun}")
    if not archive_ceilings:
        failures.append("missing [WII-STREAM] archive ceiling telemetry")
    elif final_archive_ceiling_kib < MIN_ARCHIVE_CEILING_KIB:
        failures.append(
            f"final archive ceiling {final_archive_ceiling_kib}KB is below {MIN_ARCHIVE_CEILING_KIB}KB "
            f"(maximum {max_archive_ceiling_kib}KB)"
        )
    if final_resident_live < MIN_FINAL_RESIDENT_LIVE:
        failures.append(
            f"final resident live count {final_resident_live} is below {MIN_FINAL_RESIDENT_LIVE}"
        )
    if model_events == 0 or model_avg_ms > MAX_MODEL_AVG_MS:
        failures.append(
            f"model load average {model_avg_ms}ms across {model_events} events exceeds {MAX_MODEL_AVG_MS}ms"
        )
    if txd_events == 0 or txd_avg_ms > MAX_TXD_AVG_MS:
        failures.append(
            f"TXD load average {txd_avg_ms}ms across {txd_events} events exceeds {MAX_TXD_AVG_MS}ms"
        )
    if len(island_commit_ms) < 2:
        failures.append(f"expected two island commits, found {len(island_commit_ms)}")
    elif max_island_commit_ms > MAX_ISLAND_COMMIT_MS:
        failures.append(
            f"island commit maximum {max_island_commit_ms}ms exceeds {MAX_ISLAND_COMMIT_MS}ms"
        )
    max_fallbacks = max(int(row["gx_fallback_count"]) for row in rows)
    max_hard_fallbacks = max(int(row["hard_fallback_count"]) for row in rows)
    min_raw2 = min(int(row["raw2_remaining"]) for row in rows)

    if max_fallbacks != 0:
        failures.append(f"gx_fallback_count must remain zero, got {max_fallbacks}")
    if max_hard_fallbacks != 0:
        failures.append(f"hard_fallback_count must remain zero, got {max_hard_fallbacks}")
    if min_raw2 < MIN_RAW2_REMAINING_BYTES:
        failures.append(
            f"raw2_remaining fell to {min_raw2} bytes, below {MIN_RAW2_REMAINING_BYTES}"
        )
    if int(final["gx_free"]) < MIN_FINAL_GX_FREE_BYTES:
        failures.append(f"final gx_free {final['gx_free']} is below {MIN_FINAL_GX_FREE_BYTES}")
    if int(final["gx_largest"]) < MIN_FINAL_GX_LARGEST_BYTES:
        failures.append(
            f"final gx_largest {final['gx_largest']} is below {MIN_FINAL_GX_LARGEST_BYTES}"
        )
    if int(final["generic_free"]) < MIN_FINAL_GENERIC_FREE_BYTES:
        failures.append(
            f"final generic_free {final['generic_free']} is below {MIN_FINAL_GENERIC_FREE_BYTES}"
        )
    if int(final["generic_largest"]) < MIN_FINAL_GENERIC_LARGEST_BYTES:
        failures.append(
            f"final generic_largest {final['generic_largest']} is below {MIN_FINAL_GENERIC_LARGEST_BYTES}"
        )

    summary = (
        f"log={log_path} build={start['build_id']} duration={duration_ms}ms "
        f"audio_pool={pool_used_kib}/{pool_cap_kib}KB peak={pool_peak_kib}KB denied={pool_denied} "
        f"cache=hits{hits}/misses{misses}/evict{evict}/queued{queued}/decode{decode_count} "
        f"pin={pin_count} pinned={pinned_kib}KB cache_peak={cache_peak_kib}KB cache_denied={cache_denied} "
        f"decode=avg{total_avg_ms}ms/max{total_max_ms}ms "
        f"dma=callbacks{dma_callbacks}/underrun{dma_underrun}/silence{dma_silence} "
        f"archive=max{max_archive_ceiling_kib}KB/final{final_archive_ceiling_kib}KB "
        f"resident={final_resident_live} "
        f"loads=model{model_avg_ms}ms/txd{txd_avg_ms}ms commit_max={max_island_commit_ms}ms "
        f"final_gx_free={final['gx_free']} final_generic_free={final['generic_free']} min_raw2={min_raw2}"
    )
    return failures, summary


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        log_path = find_latest_log() if args.latest else args.log.resolve()
        text, rows = load_run(log_path)
        failures, summary = evaluate(
            log_path,
            text,
            rows,
            expected_profile=args.expected_profile,
            expected_build=args.expected_build,
            dol=args.dol.resolve() if args.dol is not None else None,
            expected_dol_sha256=args.expected_dol_sha256,
        )
    except EvaluationError as exc:
        print(f"A0 audio cache run evaluation failed: {exc}", file=sys.stderr)
        return 1

    print(summary)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print(
        "A0 audio cache automated gate passed; manual route review is still required "
        "for texture quality, mid-distance LOD behavior, hitching, and subjective audio quality."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
