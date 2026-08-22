from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

try:
    from .test_extract_memory_sidecar import make_event
except ImportError:
    from test_extract_memory_sidecar import make_event


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "tools" / "wii" / "evaluate_p2_texture_run.py"
MIB = 1024 * 1024


def make_p2_event(event: str, sequence: int, elapsed_ms: int, **overrides: object) -> dict[str, object]:
    row = make_event(
        event,
        sequence,
        elapsed_ms,
        profile_id="P2-gx-plus2m",
        build_id="8a0122cab4a2-dirty-20260808T180542Z",
        runtime_arena2_lo=0x90000000,
        runtime_arena2_hi=0x94000000,
        linker_arena2_lo=0x90001800,
        linker_arena2_hi=0x93E00000,
        generic_base=0x90000000,
        generic_end=0x91200000,
        audio_base=0x91200000,
        audio_end=0x91200000,
        process_heap_claim_base=0x91200000,
        process_heap_claim_end=0x91400000,
        raw2_lo=0x91400000,
        raw2_hi=0x91E00000,
        gx_base=0x91E00000,
        gx_end=0x93800000,
        generic_capacity=18 * MIB,
        generic_used=14 * MIB,
        generic_free=4 * MIB,
        generic_largest=3 * MIB,
        generic_peak=14 * MIB,
        audio_capacity=0,
        audio_used=0,
        audio_free=0,
        audio_largest=0,
        audio_peak=0,
        audio_alloc_fail_count=0,
        process_heap_arena=4 * MIB,
        process_heap_used=3 * MIB,
        process_heap_free=MIB,
        process_heap_top=256 * 1024,
        raw2_remaining=10 * MIB,
        gx_capacity=26 * MIB,
        gx_used=22 * MIB,
        gx_free=4 * MIB,
        gx_largest=2 * MIB,
        gx_texture_bytes=21 * MIB,
        gx_texture_count=1800,
    )
    row.update(overrides)
    return row


def log_line(event: dict[str, object]) -> str:
    return f"00:00:000 [WII-P0] {json.dumps(event, separators=(',', ':'))}"


class EvaluateP2TextureRunTests(unittest.TestCase):
    def run_tool(
        self,
        *,
        rows: list[dict[str, object]] | None = None,
        extra_lines: list[str] | None = None,
        min_build: str = "20260808T180542Z",
    ) -> subprocess.CompletedProcess[str]:
        if rows is None:
            rows = [
                make_p2_event("run_start", 0, 0),
                make_p2_event("snapshot", 1, 250_000, gx_shrink_count=20, gx_alloc_fail_count=10),
                make_p2_event("snapshot", 2, 310_000, gx_shrink_count=24, gx_alloc_fail_count=12),
            ]
        lines = [log_line(row) for row in rows]
        lines.extend(
            extra_lines
            if extra_lines is not None
            else [
                "[WII-ISLAND] splash prepared target=1 phase=read ready=1",
                "[WII-ISLAND] splash prepared target=2 phase=read ready=1",
                "[reVC-WII] Game loop exited. Total frames: 9000",
            ]
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            log_path = Path(temp_dir) / "dolphin-REVC02-20260808-000000-000.log"
            log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            return subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--log",
                    str(log_path),
                    "--min-build-timestamp",
                    min_build,
                ],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )

    def test_accepts_stable_complete_route(self) -> None:
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("automated gate passed", result.stdout)

    def test_rejects_stale_build(self) -> None:
        result = self.run_tool(min_build="20260808T190000Z")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("older than required", result.stderr)

    def test_rejects_texture_load_and_allocator_failures(self) -> None:
        result = self.run_tool(
            extra_lines=[
                "[WII-ISLAND] splash prepared target=1 phase=read ready=1",
                "[WII-ISLAND] splash prepared target=2 phase=read ready=1",
                "[WII-MEM] GX allocation OOM size=1048576 tag='image-raster'",
                "[TXD-FAIL] READNATIVE failed txd=frontend1",
                "[reVC-WII] Game loop exited. Total frames: 9000",
            ]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GX allocation OOM", result.stderr)
        self.assertIn("TXD load failure", result.stderr)

    def test_rejects_runaway_late_shrink_and_exhausted_headroom(self) -> None:
        rows = [
            make_p2_event("run_start", 0, 0),
            make_p2_event("snapshot", 1, 250_000, gx_shrink_count=100, gx_alloc_fail_count=50),
            make_p2_event(
                "snapshot",
                2,
                310_000,
                gx_shrink_count=300,
                gx_alloc_fail_count=200,
                gx_used=25 * MIB + 768 * 1024,
                gx_free=256 * 1024,
                gx_largest=64 * 1024,
                raw2_lo=0x91D00000,
                process_heap_claim_end=0x91D00000,
                raw2_remaining=MIB,
            ),
        ]
        result = self.run_tool(rows=rows)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("gx_shrink_count 300 exceeds", result.stderr)
        self.assertIn("last-minute shrink delta 200 exceeds", result.stderr)
        self.assertIn("final gx_largest", result.stderr)
        self.assertIn("raw2_remaining fell", result.stderr)

    def test_rejects_long_compaction_stall(self) -> None:
        result = self.run_tool(
            extra_lines=[
                "10:00:000 [WII-ISLAND] splash prepared target=1 phase=read ready=1",
                "10:01:000 [WII-ISLAND] splash prepared target=2 phase=read ready=1",
                "10:02:000 [GX-POOL] pending compaction reason=test need=64KB free=256KB largest=32KB",
                "10:02:120 [GX-POOL] compacted gen=2 reason=test moved=100 payload=1024KB",
                "10:03:000 [reVC-WII] Game loop exited. Total frames: 9000",
            ]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("compaction duration 120ms exceeds 50ms", result.stderr)


if __name__ == "__main__":
    unittest.main()
