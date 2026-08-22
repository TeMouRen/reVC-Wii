from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "tools" / "wii" / "validate_p2_gx_boundary.py"
MIB = 1024 * 1024


def make_event(event: str, sequence: int, elapsed_ms: int, **overrides: object) -> dict[str, object]:
    base: dict[str, object] = {
        "schema_version": 1,
        "event": event,
        "sequence": sequence,
        "elapsed_ms": elapsed_ms,
        "profile_id": "P0-current",
        "build_id": "dol-123",
        "runtime_arena2_lo": 0x90000000,
        "runtime_arena2_hi": 0x94000000,
        "linker_arena2_lo": 0x90001800,
        "linker_arena2_hi": 0x93E00000,
        "generic_base": 0x90000000,
        "generic_end": 0x91200000,
        "audio_base": 0x91200000,
        "audio_end": 0x91200000,
        "process_heap_claim_base": 0x91200000,
        "process_heap_claim_end": 0x91200000,
        "raw2_lo": 0x91200000,
        "raw2_hi": 0x91E00000,
        "gx_base": 0x91E00000,
        "gx_end": 0x93600000,
        "shared_reserve_base": 0,
        "shared_reserve_end": 0,
        "shared_reserve_state": "disabled",
        "allocator_routing_mode": "generic-fixed+process-heap-arena2+gx-fixed",
        "texture_format_policy": "current-runtime",
        "texture_candidate_state": "blocked",
        "malloc_mem2": 1,
        "generic_capacity": 18 * MIB,
        "generic_used": 0,
        "generic_free": 18 * MIB,
        "generic_largest": 18 * MIB,
        "generic_peak": 0,
        "audio_capacity": 0,
        "audio_used": 0,
        "audio_free": 0,
        "audio_largest": 0,
        "audio_peak": 0,
        "audio_alloc_fail_count": 0,
        "process_heap_arena": 180000,
        "process_heap_used": 100000,
        "process_heap_free": 1000,
        "process_heap_top": 1000,
        "raw2_remaining": 12 * MIB,
        "gx_capacity": 24 * MIB,
        "gx_used": 0,
        "gx_free": 24 * MIB,
        "gx_largest": 24 * MIB,
        "gx_texture_bytes": 0,
        "gx_texture_count": 0,
        "gx_compaction_generation": 0,
        "gx_shrink_count": 0,
        "generic_owner_bytes": 0,
        "generic_system_bytes": None,
        "generic_unknown_bytes": 0,
        "gx_owner_bytes": 0,
        "gx_system_bytes": None,
        "gx_unknown_bytes": 0,
        "gx_alloc_fail_count": 0,
        "gx_fallback_count": 0,
        "request_pending": 0,
        "request_retry_count": 0,
        "hard_fallback_count": 0,
        "txd_failure_count": None,
        "run_id": "run-fixed",
        "source_log": "host.log",
        "source_line": sequence + 1,
    }
    base.update(overrides)
    return base


def make_p2_event(event: str, sequence: int, elapsed_ms: int, **overrides: object) -> dict[str, object]:
    event_data = make_event(
        event,
        sequence,
        elapsed_ms,
        profile_id="P2-gx-plus2m",
        build_id="dol-p2",
        raw2_hi=0x91C00000,
        gx_base=0x91C00000,
        raw2_remaining=10 * MIB,
        gx_capacity=26 * MIB,
        gx_free=26 * MIB,
        gx_largest=26 * MIB,
    )
    event_data.update(overrides)
    if "raw2_remaining" in overrides and "raw2_lo" not in overrides:
        event_data["raw2_lo"] = int(event_data["raw2_hi"]) - int(event_data["raw2_remaining"])
        event_data["process_heap_claim_end"] = event_data["raw2_lo"]
    return event_data


class ValidateP2GxBoundaryTests(unittest.TestCase):
    def run_tool(self, baseline_rows: list[dict[str, object]], candidate_rows: list[dict[str, object]]) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            baseline_path = root / "baseline.jsonl"
            candidate_path = root / "candidate.jsonl"
            baseline_path.write_text("\n".join(json.dumps(row) for row in baseline_rows) + "\n", encoding="utf-8")
            candidate_path.write_text("\n".join(json.dumps(row) for row in candidate_rows) + "\n", encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(SCRIPT), str(baseline_path), str(candidate_path)],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )

    def test_accepts_exact_isolated_p2_boundary(self) -> None:
        baseline = [make_event("run_start", 0, 0), make_event("snapshot", 1, 10)]
        candidate = [make_p2_event("run_start", 0, 0), make_p2_event("snapshot", 1, 10, raw2_remaining=3 * MIB)]
        result = self.run_tool(baseline, candidate)
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("24 MiB -> 26 MiB", result.stdout)

    def test_rejects_p2_fallback_to_p0_boundary(self) -> None:
        baseline = [make_event("run_start", 0, 0), make_event("snapshot", 1, 10)]
        candidate = [make_event("run_start", 0, 0, profile_id="P2-gx-plus2m"), make_event("snapshot", 1, 10, profile_id="P2-gx-plus2m")]
        result = self.run_tool(baseline, candidate)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate.gx_base", result.stderr)

    def test_rejects_insufficient_startup_raw2_headroom(self) -> None:
        baseline = [make_event("run_start", 0, 0), make_event("snapshot", 1, 10)]
        candidate = [
            make_p2_event("run_start", 0, 0, raw2_remaining=7 * MIB),
            make_p2_event("snapshot", 1, 10, raw2_remaining=3 * MIB),
        ]
        result = self.run_tool(baseline, candidate)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("startup raw2_remaining must be at least", result.stderr)

    def test_rejects_insufficient_runtime_raw2_headroom(self) -> None:
        baseline = [make_event("run_start", 0, 0), make_event("snapshot", 1, 10)]
        candidate = [make_p2_event("run_start", 0, 0), make_p2_event("snapshot", 1, 10, raw2_remaining=MIB)]
        result = self.run_tool(baseline, candidate)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("raw2_remaining fell below", result.stderr)


if __name__ == "__main__":
    unittest.main()
