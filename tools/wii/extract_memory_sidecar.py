#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


MARKER = "[WII-P0] "
SCHEMA_VERSION = 1
COMMON_FIELDS = (
    "schema_version",
    "event",
    "sequence",
    "elapsed_ms",
    "profile_id",
    "build_id",
)
BOUNDARY_FIELDS = (
    "runtime_arena2_lo",
    "runtime_arena2_hi",
    "linker_arena2_lo",
    "linker_arena2_hi",
    "generic_base",
    "generic_end",
    "audio_base",
    "audio_end",
    "process_heap_claim_base",
    "process_heap_claim_end",
    "raw2_lo",
    "raw2_hi",
    "gx_base",
    "gx_end",
)
MODE_FIELDS = (
    "shared_reserve_base",
    "shared_reserve_end",
    "shared_reserve_state",
    "allocator_routing_mode",
    "texture_format_policy",
    "texture_candidate_state",
)
NUMERIC_FIELDS = (
    "malloc_mem2",
    "generic_capacity",
    "generic_used",
    "generic_free",
    "generic_largest",
    "generic_peak",
    "audio_capacity",
    "audio_used",
    "audio_free",
    "audio_largest",
    "audio_peak",
    "audio_alloc_fail_count",
    "process_heap_arena",
    "process_heap_used",
    "process_heap_free",
    "process_heap_top",
    "raw2_remaining",
    "gx_capacity",
    "gx_used",
    "gx_free",
    "gx_largest",
    "gx_texture_bytes",
    "gx_texture_count",
    "gx_compaction_generation",
    "gx_shrink_count",
    "generic_owner_bytes",
    "generic_unknown_bytes",
    "gx_owner_bytes",
    "gx_unknown_bytes",
    "gx_alloc_fail_count",
    "gx_fallback_count",
    "request_pending",
    "request_retry_count",
    "hard_fallback_count",
)
NULLABLE_NUMERIC_FIELDS = (
    "generic_system_bytes",
    "gx_system_bytes",
    "txd_failure_count",
)
REQUIRED_FIELDS = COMMON_FIELDS + BOUNDARY_FIELDS + MODE_FIELDS + NUMERIC_FIELDS + NULLABLE_NUMERIC_FIELDS
FIXED_COMPARE_FIELDS = (
    "runtime_arena2_lo",
    "runtime_arena2_hi",
    "linker_arena2_lo",
    "linker_arena2_hi",
    "generic_base",
    "generic_end",
    "audio_base",
    "audio_end",
    "raw2_hi",
    "gx_base",
    "gx_end",
)
IDENTIFIER_RE = re.compile(r"^[A-Za-z0-9._-]+$")
class SidecarValidationError(ValueError):
    pass


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Extract [WII-P0] JSON events from a Dolphin host log into a validated JSONL sidecar."
    )
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--run-id", dest="run_id")
    return parser.parse_args(argv)


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def require_object(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise SidecarValidationError(f"{path} must be an object")
    return value


def require_string(value: Any, path: str) -> str:
    if not isinstance(value, str):
        raise SidecarValidationError(f"{path} must be a string")
    if not value:
        raise SidecarValidationError(f"{path} must be non-empty")
    return value


def require_identifier(value: Any, path: str) -> str:
    value = require_string(value, path)
    if IDENTIFIER_RE.fullmatch(value) is None:
        raise SidecarValidationError(f"{path} contains unsupported characters")
    return value


def require_int(value: Any, path: str, *, minimum: int | None = None) -> int:
    if type(value) is not int:
        raise SidecarValidationError(f"{path} must be an integer")
    if minimum is not None and value < minimum:
        raise SidecarValidationError(f"{path} must be >= {minimum}")
    return value


def require_non_negative_or_null(value: Any, path: str) -> int | None:
    if value is None:
        return None
    return require_int(value, path, minimum=0)


def validate_common_fields(event: dict[str, Any], index: int) -> None:
    unexpected = sorted(set(event) - set(REQUIRED_FIELDS))
    if unexpected:
        raise SidecarValidationError(f"event #{index} has unexpected fields: {', '.join(unexpected)}")
    for field in REQUIRED_FIELDS:
        if field not in event:
            raise SidecarValidationError(f"event #{index} missing {field}")
    if require_int(event["schema_version"], f"event #{index}.schema_version") != SCHEMA_VERSION:
        raise SidecarValidationError(f"event #{index}.schema_version must be {SCHEMA_VERSION}")
    require_string(event["event"], f"event #{index}.event")
    require_int(event["sequence"], f"event #{index}.sequence", minimum=0)
    require_int(event["elapsed_ms"], f"event #{index}.elapsed_ms", minimum=0)
    require_identifier(event["profile_id"], f"event #{index}.profile_id")
    require_identifier(event["build_id"], f"event #{index}.build_id")

    for field in BOUNDARY_FIELDS:
        require_int(event[field], f"event #{index}.{field}", minimum=0)
    for field in NUMERIC_FIELDS:
        require_int(event[field], f"event #{index}.{field}", minimum=0)
    for field in NULLABLE_NUMERIC_FIELDS:
        require_non_negative_or_null(event[field], f"event #{index}.{field}")
    for field in ("shared_reserve_base", "shared_reserve_end"):
        require_int(event[field], f"event #{index}.{field}", minimum=0)
    for field in ("shared_reserve_state", "allocator_routing_mode", "texture_format_policy", "texture_candidate_state"):
        require_string(event[field], f"event #{index}.{field}")
    if event["malloc_mem2"] not in (0, 1):
        raise SidecarValidationError(f"event #{index}.malloc_mem2 must be 0 or 1")


def get_segments(event: dict[str, Any], index: int) -> dict[str, tuple[int, int]]:
    runtime = (
        require_int(event["runtime_arena2_lo"], f"event #{index}.runtime_arena2_lo", minimum=0),
        require_int(event["runtime_arena2_hi"], f"event #{index}.runtime_arena2_hi", minimum=0),
    )
    linker = (
        require_int(event["linker_arena2_lo"], f"event #{index}.linker_arena2_lo", minimum=0),
        require_int(event["linker_arena2_hi"], f"event #{index}.linker_arena2_hi", minimum=0),
    )
    generic = (
        require_int(event["generic_base"], f"event #{index}.generic_base", minimum=0),
        require_int(event["generic_end"], f"event #{index}.generic_end", minimum=0),
    )
    audio = (
        require_int(event["audio_base"], f"event #{index}.audio_base", minimum=0),
        require_int(event["audio_end"], f"event #{index}.audio_end", minimum=0),
    )
    process_heap_claim = (
        require_int(event["process_heap_claim_base"], f"event #{index}.process_heap_claim_base", minimum=0),
        require_int(event["process_heap_claim_end"], f"event #{index}.process_heap_claim_end", minimum=0),
    )
    raw2 = (
        require_int(event["raw2_lo"], f"event #{index}.raw2_lo", minimum=0),
        require_int(event["raw2_hi"], f"event #{index}.raw2_hi", minimum=0),
    )
    gx = (
        require_int(event["gx_base"], f"event #{index}.gx_base", minimum=0),
        require_int(event["gx_end"], f"event #{index}.gx_end", minimum=0),
    )
    segments = {
        "runtime_arena2": runtime,
        "linker_arena2": linker,
        "generic": generic,
        "audio": audio,
        "process_heap_claim": process_heap_claim,
        "raw2": raw2,
        "gx": gx,
    }
    for name, (start, end) in segments.items():
        if name in ("audio", "process_heap_claim", "raw2"):
            valid_range = start <= end
        else:
            valid_range = start < end
        if not valid_range:
            raise SidecarValidationError(f"event #{index}.{name} must have lo/base < hi/end")
    return segments


def validate_reserve_and_modes(event: dict[str, Any], index: int) -> None:
    if event["shared_reserve_base"] != 0 or event["shared_reserve_end"] != 0:
        raise SidecarValidationError(f"event #{index}.shared_reserve_base/end must both be 0")
    if event["shared_reserve_state"] != "disabled":
        raise SidecarValidationError(f"event #{index}.shared_reserve_state must be disabled")
    if event["texture_candidate_state"] != "blocked":
        raise SidecarValidationError(f"event #{index}.texture_candidate_state must be blocked")


def validate_usage_invariants(event: dict[str, Any], index: int) -> None:
    if event["generic_used"] > event["generic_capacity"]:
        raise SidecarValidationError(f"event #{index}.generic_used exceeds generic_capacity")
    if event["generic_free"] > event["generic_capacity"]:
        raise SidecarValidationError(f"event #{index}.generic_free exceeds generic_capacity")
    if event["generic_largest"] > event["generic_free"]:
        raise SidecarValidationError(f"event #{index}.generic_largest exceeds generic_free")
    if event["generic_peak"] > event["generic_capacity"]:
        raise SidecarValidationError(f"event #{index}.generic_peak exceeds generic_capacity")
    if event["audio_capacity"] != event["audio_end"] - event["audio_base"]:
        raise SidecarValidationError(f"event #{index}.audio_capacity disagrees with audio bounds")
    if event["audio_used"] > event["audio_capacity"]:
        raise SidecarValidationError(f"event #{index}.audio_used exceeds audio_capacity")
    if event["audio_free"] > event["audio_capacity"]:
        raise SidecarValidationError(f"event #{index}.audio_free exceeds audio_capacity")
    if event["audio_largest"] > event["audio_free"]:
        raise SidecarValidationError(f"event #{index}.audio_largest exceeds audio_free")
    if event["audio_peak"] > event["audio_capacity"]:
        raise SidecarValidationError(f"event #{index}.audio_peak exceeds audio_capacity")
    if event["gx_used"] > event["gx_capacity"]:
        raise SidecarValidationError(f"event #{index}.gx_used exceeds gx_capacity")
    if event["gx_free"] > event["gx_capacity"]:
        raise SidecarValidationError(f"event #{index}.gx_free exceeds gx_capacity")
    if event["gx_largest"] > event["gx_free"]:
        raise SidecarValidationError(f"event #{index}.gx_largest exceeds gx_free")
    if event["process_heap_used"] > event["process_heap_arena"]:
        raise SidecarValidationError(f"event #{index}.process_heap_used exceeds process_heap_arena")
    if event["process_heap_free"] > event["process_heap_arena"]:
        raise SidecarValidationError(f"event #{index}.process_heap_free exceeds process_heap_arena")
    if event["process_heap_top"] > event["process_heap_arena"]:
        raise SidecarValidationError(f"event #{index}.process_heap_top exceeds process_heap_arena")


def validate_boundary_invariants(
    event: dict[str, Any],
    index: int,
    *,
    run_start_boundary_modes: dict[str, Any] | None,
    snapshot_bounds: dict[str, tuple[int, int]] | None,
) -> tuple[dict[str, Any], dict[str, tuple[int, int]]]:
    segments = get_segments(event, index)
    validate_reserve_and_modes(event, index)
    validate_usage_invariants(event, index)

    runtime_start, runtime_end = segments["runtime_arena2"]
    audio_disabled_sentinel = (
        event["audio_capacity"] == 0
        and segments["audio"] == (0, 0)
    )
    for name in ("generic", "audio", "process_heap_claim", "raw2", "gx"):
        if name == "audio" and audio_disabled_sentinel:
            continue
        start, end = segments[name]
        if start < runtime_start or end > runtime_end:
            raise SidecarValidationError(f"event #{index}.{name} must stay within runtime_arena2")

    if event["audio_capacity"] == 0:
        empty_audio = segments["audio"]
        if empty_audio not in ((0, 0), (event["generic_end"], event["generic_end"])):
            raise SidecarValidationError(
                f"event #{index}.disabled audio bounds must be 0/0 or generic_end/generic_end"
            )
        if event["process_heap_claim_base"] != event["generic_end"]:
            raise SidecarValidationError(
                f"event #{index}.process_heap_claim_base must equal generic_end when audio is disabled"
            )
    else:
        if event["generic_end"] != event["audio_base"]:
            raise SidecarValidationError(f"event #{index}.audio_base must equal generic_end")
        if event["audio_end"] != event["process_heap_claim_base"]:
            raise SidecarValidationError(f"event #{index}.process_heap_claim_base must equal audio_end")
    if event["process_heap_claim_end"] != event["raw2_lo"]:
        raise SidecarValidationError(f"event #{index}.process_heap_claim_end must equal raw2_lo")
    if event["raw2_hi"] != event["gx_base"]:
        raise SidecarValidationError(f"event #{index}.raw2_hi must equal gx_base")
    if not (event["audio_end"] <= event["raw2_lo"] <= event["gx_base"]):
        raise SidecarValidationError(f"event #{index}.raw2_lo must stay within audio_end..gx_base")
    if event["raw2_remaining"] != event["raw2_hi"] - event["raw2_lo"]:
        raise SidecarValidationError(f"event #{index}.raw2_remaining disagrees with raw2 bounds")
    pools = [
        ("generic", *segments["generic"]),
        ("audio", *segments["audio"]),
        ("raw2", *segments["raw2"]),
        ("gx", *segments["gx"]),
    ]
    pools.sort(key=lambda item: item[1])
    for i in range(len(pools) - 1):
        left_name, _, left_end = pools[i]
        right_name, right_start, _ = pools[i + 1]
        if left_end > right_start:
            raise SidecarValidationError(f"event #{index} has overlapping {left_name} and {right_name}")

    boundary_modes = {field: event[field] for field in FIXED_COMPARE_FIELDS + MODE_FIELDS}
    if run_start_boundary_modes is not None:
        for field, expected in run_start_boundary_modes.items():
            if boundary_modes[field] != expected:
                raise SidecarValidationError(f"event #{index}.{field} must match run_start")

    if snapshot_bounds is not None:
        for name, field_name in (
            ("runtime_arena2", "runtime_arena2_lo/hi"),
            ("linker_arena2", "linker_arena2_lo/hi"),
            ("generic", "generic_base/end"),
            ("audio", "audio_base/end"),
            ("gx", "gx_base/end"),
        ):
            if snapshot_bounds[name] != segments[name]:
                raise SidecarValidationError(f"event #{index}.{field_name} moved from the first snapshot")
        if snapshot_bounds["raw2"][1] != segments["raw2"][1]:
            raise SidecarValidationError(f"event #{index}.raw2_hi moved from the first snapshot")

    return boundary_modes, segments


def extract_events(input_path: Path) -> list[tuple[int, dict[str, Any]]]:
    if not input_path.is_file():
        raise SidecarValidationError(f"input log does not exist: {input_path}")

    events: list[tuple[int, dict[str, Any]]] = []
    with input_path.open("r", encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, start=1):
            marker_index = line.find(MARKER)
            if marker_index < 0:
                continue
            payload = line[marker_index + len(MARKER) :].strip()
            if not payload:
                raise SidecarValidationError(f"line {line_number} has an empty {MARKER.strip()} payload")
            try:
                parsed = json.loads(payload)
            except json.JSONDecodeError as exc:
                raise SidecarValidationError(f"line {line_number} contains invalid JSON: {exc.msg}") from exc
            events.append((line_number, require_object(parsed, f"line {line_number}")))

    if not events:
        raise SidecarValidationError(f"no {MARKER.strip()} events found in {input_path}")
    return events


def validate_and_materialize(events: list[tuple[int, dict[str, Any]]], source_log: Path, run_id: str) -> list[dict[str, Any]]:
    run_start_count = 0
    profile_id: str | None = None
    build_id: str | None = None
    previous_sequence: int | None = None
    previous_elapsed: int | None = None
    run_start_boundary_modes: dict[str, Any] | None = None
    first_snapshot_bounds: dict[str, tuple[int, int]] | None = None
    output_rows: list[dict[str, Any]] = []

    for index, (line_number, event) in enumerate(events, start=1):
        validate_common_fields(event, index)

        event_name = event["event"]
        if index == 1 and event_name != "run_start":
            raise SidecarValidationError("run_start must be the first event")
        if index == 1 and (event["sequence"] != 0 or event["elapsed_ms"] != 0):
            raise SidecarValidationError("run_start must start at sequence 0 and elapsed_ms 0")
        if event_name == "run_start":
            run_start_count += 1
            if run_start_count > 1:
                raise SidecarValidationError("run_start must be unique")
        elif event_name != "snapshot":
            raise SidecarValidationError(f"unsupported event type: {event_name}")

        sequence = event["sequence"]
        elapsed_ms = event["elapsed_ms"]
        if previous_sequence is not None and sequence <= previous_sequence:
            raise SidecarValidationError("sequence must be strictly increasing")
        if previous_elapsed is not None and elapsed_ms < previous_elapsed:
            raise SidecarValidationError("elapsed_ms must be non-decreasing")
        previous_sequence = sequence
        previous_elapsed = elapsed_ms

        if profile_id is None:
            profile_id = event["profile_id"]
            build_id = event["build_id"]
        else:
            if event["profile_id"] != profile_id:
                raise SidecarValidationError("profile_id changed within one run")
            if event["build_id"] != build_id:
                raise SidecarValidationError("build_id changed within one run")

        boundary_modes, segments = validate_boundary_invariants(
            event,
            index,
            run_start_boundary_modes=run_start_boundary_modes,
            snapshot_bounds=first_snapshot_bounds if event_name == "snapshot" else None,
        )
        if event_name == "run_start":
            run_start_boundary_modes = boundary_modes
        elif first_snapshot_bounds is None:
            first_snapshot_bounds = segments

        output_event = dict(event)
        output_event["run_id"] = run_id
        output_event["source_log"] = str(source_log)
        output_event["source_line"] = line_number
        output_rows.append(output_event)

    if run_start_count != 1:
        raise SidecarValidationError("exactly one run_start event is required")
    if first_snapshot_bounds is None:
        raise SidecarValidationError("at least one snapshot event is required")
    return output_rows


def write_jsonl(output_path: Path, rows: list[dict[str, Any]]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    created = False
    try:
        with output_path.open("x", encoding="utf-8", newline="\n") as handle:
            created = True
            for row in rows:
                handle.write(json.dumps(row, sort_keys=True, separators=(",", ":")))
                handle.write("\n")
    except OSError:
        if created:
            output_path.unlink(missing_ok=True)
        raise


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    input_path = args.input.resolve()
    output_path = args.output.resolve()

    if output_path.exists():
        return fail(f"refusing to overwrite existing output: {output_path}")

    run_id = args.run_id if args.run_id is not None else input_path.stem
    try:
        require_identifier(run_id, "run_id")
    except SidecarValidationError as exc:
        return fail(str(exc))

    try:
        events = extract_events(input_path)
        rows = validate_and_materialize(events, input_path, run_id)
        write_jsonl(output_path, rows)
    except (OSError, SidecarValidationError) as exc:
        return fail(str(exc))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
