#!/usr/bin/env python3
"""Fail-closed acceptance gate for the isolated P2 GX Arena2 boundary run."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import extract_memory_sidecar as sidecar


P0_PROFILE_ID = "P0-current"
P2_PROFILE_ID = "P2-gx-plus2m"
GX_EXPANSION_BYTES = 2 * 1024 * 1024
P2_STARTUP_RAW2_MIN_BYTES = 8 * 1024 * 1024
P2_RUNTIME_RAW2_MIN_BYTES = 2 * 1024 * 1024
PROVENANCE_FIELDS = ("run_id", "source_log", "source_line")


class P2ValidationError(ValueError):
    pass


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate that a P2 GX +2 MiB run preserved the P0 Arena2 safety boundaries."
    )
    parser.add_argument("baseline", type=Path, help="Validated P0-current JSONL sidecar")
    parser.add_argument("candidate", type=Path, help="Validated P2-gx-plus2m JSONL sidecar")
    return parser.parse_args(argv)


def fail(message: str) -> int:
    print(f"P2 GX boundary validation failed: {message}", file=sys.stderr)
    return 1


def load_validated_sidecar(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        raise P2ValidationError(f"sidecar does not exist: {path}")

    rows: list[dict[str, Any]] = []
    expected_fields = set(sidecar.REQUIRED_FIELDS) | set(PROVENANCE_FIELDS)
    try:
        with path.open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, start=1):
                if not line.strip():
                    raise P2ValidationError(f"{path}:{line_number} must not be empty")
                parsed = json.loads(line)
                if not isinstance(parsed, dict):
                    raise P2ValidationError(f"{path}:{line_number} must be a JSON object")
                if set(parsed) != expected_fields:
                    raise P2ValidationError(f"{path}:{line_number} has an unexpected sidecar schema")
                rows.append(parsed)
    except OSError as exc:
        raise P2ValidationError(f"cannot read {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise P2ValidationError(f"{path}:{exc.lineno} contains invalid JSON") from exc

    if not rows:
        raise P2ValidationError(f"{path} contains no events")

    run_ids = {row["run_id"] for row in rows}
    source_logs = {row["source_log"] for row in rows}
    if len(run_ids) != 1 or not all(isinstance(value, str) and value for value in run_ids):
        raise P2ValidationError(f"{path} must contain one non-empty run_id")
    if len(source_logs) != 1 or not all(isinstance(value, str) and value for value in source_logs):
        raise P2ValidationError(f"{path} must contain one non-empty source_log")
    if any(type(row["source_line"]) is not int or row["source_line"] < 1 for row in rows):
        raise P2ValidationError(f"{path} has an invalid source_line")

    events = [(row["source_line"], {field: row[field] for field in sidecar.REQUIRED_FIELDS}) for row in rows]
    try:
        return sidecar.validate_and_materialize(events, path, next(iter(run_ids)))
    except sidecar.SidecarValidationError as exc:
        raise P2ValidationError(f"{path} is not a valid extracted sidecar: {exc}") from exc


def require_equal(label: str, actual: Any, expected: Any) -> None:
    if actual != expected:
        raise P2ValidationError(f"{label} expected {expected!r}, got {actual!r}")


def validate_profile(rows: list[dict[str, Any]], expected_profile: str, label: str) -> dict[str, Any]:
    start = rows[0]
    require_equal(f"{label}.event", start["event"], "run_start")
    require_equal(f"{label}.profile_id", start["profile_id"], expected_profile)
    for index, row in enumerate(rows, start=1):
        require_equal(f"{label} event #{index}.profile_id", row["profile_id"], expected_profile)
        require_equal(f"{label} event #{index}.texture_candidate_state", row["texture_candidate_state"], "blocked")
        require_equal(f"{label} event #{index}.shared_reserve_state", row["shared_reserve_state"], "disabled")
    return start


def validate_boundary(baseline_rows: list[dict[str, Any]], candidate_rows: list[dict[str, Any]]) -> str:
    baseline = validate_profile(baseline_rows, P0_PROFILE_ID, "baseline")
    candidate = validate_profile(candidate_rows, P2_PROFILE_ID, "candidate")

    for field in (
        "runtime_arena2_lo",
        "runtime_arena2_hi",
        "linker_arena2_lo",
        "linker_arena2_hi",
        "generic_base",
        "generic_end",
        "gx_end",
        "allocator_routing_mode",
        "texture_format_policy",
    ):
        require_equal(f"candidate.{field}", candidate[field], baseline[field])

    require_equal("candidate.gx_base", candidate["gx_base"], baseline["gx_base"] - GX_EXPANSION_BYTES)
    require_equal("candidate.raw2_hi", candidate["raw2_hi"], candidate["gx_base"])
    require_equal("candidate.gx_capacity", candidate["gx_capacity"], baseline["gx_capacity"] + GX_EXPANSION_BYTES)
    require_equal("candidate.gx_capacity geometry", candidate["gx_capacity"], candidate["gx_end"] - candidate["gx_base"])
    require_equal("baseline.gx_capacity geometry", baseline["gx_capacity"], baseline["gx_end"] - baseline["gx_base"])
    if candidate["raw2_remaining"] < P2_STARTUP_RAW2_MIN_BYTES:
        raise P2ValidationError(
            f"candidate startup raw2_remaining must be at least {P2_STARTUP_RAW2_MIN_BYTES}, got {candidate['raw2_remaining']}"
        )

    min_raw2_remaining = min(row["raw2_remaining"] for row in candidate_rows)
    if min_raw2_remaining < P2_RUNTIME_RAW2_MIN_BYTES:
        raise P2ValidationError(
            f"candidate raw2_remaining fell below {P2_RUNTIME_RAW2_MIN_BYTES}: {min_raw2_remaining}"
        )

    return (
        "P2 GX boundary validation passed: "
        f"GX {baseline['gx_capacity'] // (1024 * 1024)} MiB -> {candidate['gx_capacity'] // (1024 * 1024)} MiB; "
        f"candidate minimum raw2/newlib headroom {min_raw2_remaining // 1024} KiB."
    )


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        baseline_rows = load_validated_sidecar(args.baseline.resolve())
        candidate_rows = load_validated_sidecar(args.candidate.resolve())
        print(validate_boundary(baseline_rows, candidate_rows))
        return 0
    except P2ValidationError as exc:
        return fail(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())
