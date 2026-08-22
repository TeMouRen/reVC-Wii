from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "tools" / "wii" / "extract_memory_sidecar.py"


def make_event(event: str, sequence: int, elapsed_ms: int, **overrides: object) -> dict[str, object]:
    base: dict[str, object] = {
        "schema_version": 1,
        "event": event,
        "sequence": sequence,
        "elapsed_ms": elapsed_ms,
        "profile_id": "route-a",
        "build_id": "dol-123",
        "runtime_arena2_lo": 0x1000,
        "runtime_arena2_hi": 0xA000,
        "linker_arena2_lo": 0x1800,
        "linker_arena2_hi": 0x1C00,
        "generic_base": 0x2000,
        "generic_end": 0x3000,
        "process_heap_claim_base": 0x3000,
        "process_heap_claim_end": 0x4000,
        "raw2_lo": 0x4000,
        "raw2_hi": 0x6000,
        "gx_base": 0x6000,
        "gx_end": 0x9000,
        "shared_reserve_base": 0,
        "shared_reserve_end": 0,
        "shared_reserve_state": "disabled",
        "allocator_routing_mode": "locked",
        "texture_format_policy": "phase0",
        "texture_candidate_state": "blocked",
        "malloc_mem2": 0,
        "generic_capacity": 4096,
        "generic_used": 2048,
        "generic_free": 2048,
        "generic_largest": 1024,
        "generic_peak": 2048,
        "audio_base": 0x3000,
        "audio_end": 0x3000,
        "audio_capacity": 0,
        "audio_used": 0,
        "audio_free": 0,
        "audio_largest": 0,
        "audio_peak": 0,
        "audio_alloc_fail_count": 0,
        "process_heap_arena": 8192,
        "process_heap_used": 4096,
        "process_heap_free": 2048,
        "process_heap_top": 1024,
        "raw2_remaining": 8192,
        "gx_capacity": 12288,
        "gx_used": 4096,
        "gx_free": 8192,
        "gx_largest": 4096,
        "gx_texture_bytes": 3072,
        "gx_texture_count": 3,
        "gx_compaction_generation": 1,
        "gx_shrink_count": 0,
        "generic_owner_bytes": 256,
        "generic_system_bytes": None,
        "generic_unknown_bytes": 0,
        "gx_owner_bytes": 1024,
        "gx_system_bytes": None,
        "gx_unknown_bytes": 0,
        "gx_alloc_fail_count": 0,
        "gx_fallback_count": 0,
        "request_pending": 0,
        "request_retry_count": 0,
        "hard_fallback_count": 0,
        "txd_failure_count": None,
    }
    base.update(overrides)
    return base


class ExtractMemorySidecarTests(unittest.TestCase):
    maxDiff = None

    def run_tool(self, lines: list[str], output_name: str = "sidecar.jsonl", *extra_args: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            input_path = temp_root / "host.log"
            output_path = temp_root / output_name
            input_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(input_path), str(output_path), *extra_args],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )
            result.output_exists = output_path.exists()  # type: ignore[attr-defined]
            result.output_text = output_path.read_text(encoding="utf-8") if output_path.exists() else ""  # type: ignore[attr-defined]
            return result

    def test_extracts_valid_sidecar(self) -> None:
        lines = [
            "noise before marker",
            f"prefix [WII-P0] {json.dumps(make_event('run_start', 0, 0), separators=(',', ':'))}",
            f"dolphin: [WII-P0] {json.dumps(make_event('snapshot', 1, 10, raw2_lo=0x4200, process_heap_claim_end=0x4200, raw2_remaining=0x1E00), separators=(',', ':'))}",
            f"dolphin: [WII-P0] {json.dumps(make_event('snapshot', 2, 20, raw2_lo=0x4300, process_heap_claim_end=0x4300, raw2_remaining=0x1D00, gx_used=6144, gx_free=6144), separators=(',', ':'))}",
        ]
        result = self.run_tool(lines, "sidecar.jsonl", "--run-id", "run-fixed")
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertTrue(result.output_exists)

        rows = [json.loads(line) for line in result.output_text.splitlines()]
        self.assertEqual([row["event"] for row in rows], ["run_start", "snapshot", "snapshot"])
        self.assertEqual({row["run_id"] for row in rows}, {"run-fixed"})
        self.assertEqual(rows[0]["source_line"], 2)
        self.assertEqual(rows[1]["source_line"], 3)
        self.assertEqual(rows[2]["source_line"], 4)
        self.assertIsNone(rows[0]["generic_system_bytes"])
        self.assertEqual(rows[2]["gx_used"], 6144)
        self.assertEqual(rows[1]["raw2_lo"], 0x4200)
        self.assertEqual(rows[2]["process_heap_claim_end"], 0x4300)

    def test_accepts_nonzero_audio_pool(self) -> None:
        start = make_event(
            "run_start",
            0,
            0,
            audio_end=0x3800,
            process_heap_claim_base=0x3800,
            audio_capacity=0x800,
            audio_used=0x200,
            audio_free=0x600,
            audio_largest=0x400,
            audio_peak=0x200,
        )
        snapshot = dict(start)
        snapshot.update(event="snapshot", sequence=1, elapsed_ms=10)
        result = self.run_tool(
            [
                f"[WII-P0] {json.dumps(start, separators=(',', ':'))}",
                f"[WII-P0] {json.dumps(snapshot, separators=(',', ':'))}",
            ]
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_accepts_disabled_audio_zero_bounds_sentinel(self) -> None:
        start = make_event(
            "run_start",
            0,
            0,
            audio_base=0,
            audio_end=0,
            audio_capacity=0,
        )
        snapshot = dict(start)
        snapshot.update(event="snapshot", sequence=1, elapsed_ms=10)
        result = self.run_tool(
            [
                f"[WII-P0] {json.dumps(start, separators=(',', ':'))}",
                f"[WII-P0] {json.dumps(snapshot, separators=(',', ':'))}",
            ]
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_rejects_disabled_audio_noncanonical_bounds(self) -> None:
        start = make_event(
            "run_start",
            0,
            0,
            audio_base=0x2800,
            audio_end=0x2800,
            audio_capacity=0,
        )
        result = self.run_tool(
            [f"[WII-P0] {json.dumps(start, separators=(',', ':'))}"]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("disabled audio bounds", result.stderr)

    def test_rejects_invalid_json_and_writes_nothing(self) -> None:
        lines = [
            f"[WII-P0] {json.dumps(make_event('run_start', 0, 0), separators=(',', ':'))}",
            "[WII-P0] {not-json}",
        ]
        result = self.run_tool(lines)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid JSON", result.stderr)
        self.assertFalse(result.output_exists)

    def test_rejects_non_first_run_start(self) -> None:
        lines = [
            f"[WII-P0] {json.dumps(make_event('snapshot', 0, 0), separators=(',', ':'))}",
            f"[WII-P0] {json.dumps(make_event('run_start', 1, 10), separators=(',', ':'))}",
        ]
        result = self.run_tool(lines)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("run_start must be the first event", result.stderr)
        self.assertFalse(result.output_exists)

    def test_rejects_snapshot_boundary_change_from_run_start(self) -> None:
        lines = [
            f"[WII-P0] {json.dumps(make_event('run_start', 0, 0), separators=(',', ':'))}",
            f"[WII-P0] {json.dumps(make_event('snapshot', 1, 10, generic_base=0x2100), separators=(',', ':'))}",
        ]
        result = self.run_tool(lines)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must match run_start", result.stderr)
        self.assertFalse(result.output_exists)

    def test_rejects_process_heap_claim_mismatch(self) -> None:
        lines = [
            f"[WII-P0] {json.dumps(make_event('run_start', 0, 0), separators=(',', ':'))}",
            f"[WII-P0] {json.dumps(make_event('snapshot', 1, 10, raw2_lo=0x4100, process_heap_claim_end=0x4200, raw2_remaining=0x1F00), separators=(',', ':'))}",
        ]
        result = self.run_tool(lines)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("process_heap_claim_end must equal raw2_lo", result.stderr)
        self.assertFalse(result.output_exists)

    def test_rejects_invalid_process_heap_claim_range(self) -> None:
        lines = [
            f"[WII-P0] {json.dumps(make_event('run_start', 0, 0), separators=(',', ':'))}",
            f"[WII-P0] {json.dumps(make_event('snapshot', 1, 10, raw2_lo=0x2F00, process_heap_claim_end=0x2F00, raw2_remaining=0x3100), separators=(',', ':'))}",
        ]
        result = self.run_tool(lines)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("process_heap_claim must have lo/base < hi/end", result.stderr)
        self.assertFalse(result.output_exists)

    def test_rejects_invalid_shared_reserve_state(self) -> None:
        lines = [
            f"[WII-P0] {json.dumps(make_event('run_start', 0, 0, shared_reserve_state='enabled'), separators=(',', ':'))}",
            f"[WII-P0] {json.dumps(make_event('snapshot', 1, 10, shared_reserve_state='enabled'), separators=(',', ':'))}",
        ]
        result = self.run_tool(lines)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("shared_reserve_state must be disabled", result.stderr)
        self.assertFalse(result.output_exists)

    def test_rejects_existing_output_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            input_path = temp_root / "host.log"
            output_path = temp_root / "sidecar.jsonl"
            input_path.write_text(
                "\n".join(
                    [
                        f"[WII-P0] {json.dumps(make_event('run_start', 0, 0), separators=(',', ':'))}",
                        f"[WII-P0] {json.dumps(make_event('snapshot', 1, 10), separators=(',', ':'))}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            output_path.write_text("existing\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(input_path), str(output_path)],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusing to overwrite", result.stderr)
            self.assertEqual(output_path.read_text(encoding="utf-8"), "existing\n")

    def test_accepts_zero_length_process_heap_claim_and_external_linker_fallback(self) -> None:
        start = make_event(
            "run_start",
            0,
            0,
            runtime_arena2_lo=0x2000,
            linker_arena2_lo=0x1000,
            linker_arena2_hi=0x1C00,
            generic_base=0x2000,
            process_heap_claim_end=0x3000,
            raw2_lo=0x3000,
            raw2_remaining=0x3000,
        )
        snapshot = dict(start)
        snapshot.update(event="snapshot", sequence=1, elapsed_ms=10)
        result = self.run_tool(
            [
                f"[WII-P0] {json.dumps(start, separators=(',', ':'))}",
                f"[WII-P0] {json.dumps(snapshot, separators=(',', ':'))}",
            ]
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_accepts_zero_length_raw2_interval(self) -> None:
        start = make_event(
            "run_start",
            0,
            0,
            process_heap_claim_end=0x6000,
            raw2_lo=0x6000,
            raw2_remaining=0,
        )
        snapshot = dict(start)
        snapshot.update(event="snapshot", sequence=1, elapsed_ms=10)
        result = self.run_tool(
            [
                f"[WII-P0] {json.dumps(start, separators=(',', ':'))}",
                f"[WII-P0] {json.dumps(snapshot, separators=(',', ':'))}",
            ]
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)

    def test_rejects_sequence_regression(self) -> None:
        lines = [
            f"[WII-P0] {json.dumps(make_event('run_start', 0, 0), separators=(',', ':'))}",
            f"[WII-P0] {json.dumps(make_event('snapshot', 2, 10), separators=(',', ':'))}",
            f"[WII-P0] {json.dumps(make_event('snapshot', 1, 20), separators=(',', ':'))}",
        ]
        result = self.run_tool(lines)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sequence must be strictly increasing", result.stderr)
        self.assertFalse(result.output_exists)

    def test_rejects_profile_or_build_change(self) -> None:
        for field, value in (("profile_id", "route-b"), ("build_id", "dol-456")):
            with self.subTest(field=field):
                lines = [
                    f"[WII-P0] {json.dumps(make_event('run_start', 0, 0), separators=(',', ':'))}",
                    f"[WII-P0] {json.dumps(make_event('snapshot', 1, 10, **{field: value}), separators=(',', ':'))}",
                ]
                result = self.run_tool(lines)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"{field} changed", result.stderr)
                self.assertFalse(result.output_exists)

    def test_rejects_missing_or_unexpected_field(self) -> None:
        for mutate, expected in (
            (lambda event: event.pop("gx_largest"), "missing gx_largest"),
            (lambda event: event.update(gx_larget=1), "unexpected fields"),
        ):
            with self.subTest(expected=expected):
                snapshot = make_event("snapshot", 1, 10)
                mutate(snapshot)
                lines = [
                    f"[WII-P0] {json.dumps(make_event('run_start', 0, 0), separators=(',', ':'))}",
                    f"[WII-P0] {json.dumps(snapshot, separators=(',', ':'))}",
                ]
                result = self.run_tool(lines)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertFalse(result.output_exists)


if __name__ == "__main__":
    unittest.main()
