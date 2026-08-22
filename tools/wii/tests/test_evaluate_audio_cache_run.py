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
SCRIPT = REPO_ROOT / "tools" / "wii" / "evaluate_audio_cache_run.py"
MIB = 1024 * 1024


def make_a0_event(event: str, sequence: int, elapsed_ms: int, **overrides: object) -> dict[str, object]:
    row = make_event(
        event,
        sequence,
        elapsed_ms,
        profile_id="A0-audio-cache",
        build_id="8a0122cab4a2-dirty-20260809T230000Z",
        runtime_arena2_lo=0x90000000,
        runtime_arena2_hi=0x94000000,
        linker_arena2_lo=0x90001800,
        linker_arena2_hi=0x93E00000,
        generic_base=0x90000000,
        generic_end=0x90800000,
        audio_base=0x90800000,
        audio_end=0x90C00000,
        process_heap_claim_base=0x90C00000,
        process_heap_claim_end=0x90E00000,
        raw2_lo=0x90E00000,
        raw2_hi=0x91800000,
        gx_base=0x91800000,
        gx_end=0x93600000,
        generic_capacity=8 * MIB,
        generic_used=7 * MIB,
        generic_free=MIB,
        generic_largest=512 * 1024,
        generic_peak=7 * MIB,
        audio_capacity=4 * MIB,
        audio_used=3 * MIB,
        audio_free=MIB,
        audio_largest=MIB,
        audio_peak=3 * MIB,
        audio_alloc_fail_count=0,
        process_heap_arena=2 * MIB,
        process_heap_used=1536 * 1024,
        process_heap_free=512 * 1024,
        process_heap_top=256 * 1024,
        raw2_remaining=10 * MIB,
        gx_capacity=30 * MIB,
        gx_used=25 * MIB,
        gx_free=5 * MIB,
        gx_largest=2 * MIB,
        gx_texture_bytes=24 * MIB,
        gx_texture_count=1900,
    )
    row.update(overrides)
    return row


def log_line(event: dict[str, object]) -> str:
    return f"00:00:000 [WII-P0] {json.dumps(event, separators=(',', ':'))}"


class EvaluateAudioCacheRunTests(unittest.TestCase):
    def run_tool(
        self,
        *,
        rows: list[dict[str, object]] | None = None,
        extra_lines: list[str] | None = None,
        expected_build: str = "8a0122cab4a2-dirty-20260809T230000Z",
    ) -> subprocess.CompletedProcess[str]:
        if rows is None:
            rows = [
                make_a0_event("run_start", 0, 0),
                make_a0_event("snapshot", 1, 120_000),
                make_a0_event("snapshot", 2, 310_000),
            ]
        lines = [log_line(row) for row in rows]
        lines.extend(
            extra_lines
            if extra_lines is not None
            else [
                "00:10:000 [WII-ISLAND] splash prepared target=1 phase=read ready=1",
                "00:20:000 [WII-ISLAND] splash prepared target=2 phase=read ready=1",
                "00:21:000 [WII-AUDIO-POOL] cap=4096KB used=3072KB peak=3584KB free=1024KB largest=1024KB denied=0",
                "00:21:010 [WII-AUDIO-CACHE] hits=120 misses=24 evict=8 queued=24 decode=24 pin=4 pinned=768KB peak=4096KB denied=0",
                "00:21:020 [WII-AUDIO-DECODE] count=24 queue=avg2/max5 service=avg3/max6 total=avg5/max11",
                "00:21:030 [WII-AUDIO-DMA] callbacks=900 underrun=0 silence=0",
                "00:21:040 [WII-STREAM] archive ceiling=28160KB->28672KB pressure=0x0",
                "00:21:050 [WII-STREAM-DIAG] win=5000ms trimEp=2 victims=4 m/t/c/a=3/1/0/0 reload5=0 reload10=0 model=n100/avg600/p95b1000/max2000 txd=n40/avg800/p95b1000/max1800 radar=n0/avg0/p95b0/max0 pending=2 evq=0 dropped=0",
                "00:21:060 [WII-RESIDENT] live=L1020/S0 ledger=g3000/u90 nl8000/u90 gx14000/u80 pool=g3500 nl21000 gx24000 residual=g+500 nl+13000 gx+10000 raw2free=5000/d+0 owner=g0/u0 gx13500/u10500 sat=0 under=0",
                "00:21:070 [WII-ISLAND] commit source=1 target=2 dt=7000ms pending=0 priority=0",
                "00:21:080 [WII-ISLAND] commit source=2 target=1 dt=7600ms pending=0 priority=0",
                "00:30:000 [reVC-WII] Game loop exited. Total frames: 9000",
            ]
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_root = Path(temp_dir)
            log_path = temp_root / "dolphin-REVC02-20260809-000000-000.log"
            dol_path = temp_root / "main.dol"
            log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            dol_path.write_bytes(b"A0-test-dol\n")
            dol_sha256 = __import__("hashlib").sha256(dol_path.read_bytes()).hexdigest().upper()
            return subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--log",
                    str(log_path),
                    "--expected-profile",
                    "A0-audio-cache",
                    "--expected-build",
                    expected_build,
                    "--dol",
                    str(dol_path),
                    "--expected-dol-sha256",
                    dol_sha256,
                ],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )

    def test_accepts_complete_audio_cache_route(self) -> None:
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("automated gate passed", result.stdout)

    def test_rejects_archive_ceiling_that_drops_after_reaching_target(self) -> None:
        result = self.run_tool(
            extra_lines=[
                "00:10:000 [WII-ISLAND] splash prepared target=1 phase=read ready=1",
                "00:20:000 [WII-ISLAND] splash prepared target=2 phase=read ready=1",
                "00:21:000 [WII-AUDIO-POOL] cap=4096KB used=3072KB peak=3584KB free=1024KB largest=1024KB denied=0",
                "00:21:010 [WII-AUDIO-CACHE] hits=120 misses=24 evict=8 queued=24 decode=24 pin=4 pinned=768KB peak=4096KB denied=0",
                "00:21:020 [WII-AUDIO-DECODE] count=24 queue=avg2/max5 service=avg3/max6 total=avg5/max11",
                "00:21:030 [WII-AUDIO-DMA] callbacks=900 underrun=0 silence=0",
                "00:21:040 [WII-STREAM] archive ceiling=28160KB->28672KB pressure=0x0 grow=1 retain=1 reason=grow soft_age=0ms generic=4096/2048KB rawnl=8192KB gx=8192/4096KB",
                "00:22:040 [WII-STREAM] archive ceiling=28672KB->27648KB pressure=0x0 grow=0 retain=0 reason=soft soft_age=1000ms generic=4096/2048KB rawnl=8192KB gx=3000/2048KB",
                "00:23:040 [WII-STREAM] archive ceiling=27648KB->26624KB pressure=0x0 grow=0 retain=0 reason=soft soft_age=2000ms generic=4096/2048KB rawnl=8192KB gx=3000/2048KB",
                "00:24:000 [WII-STREAM-DIAG] win=5000ms trimEp=2 victims=4 m/t/c/a=3/1/0/0 reload5=0 reload10=0 model=n100/avg600/p95b1000/max2000 txd=n40/avg800/p95b1000/max1800 radar=n0/avg0/p95b0/max0 pending=2 evq=0 dropped=0",
                "00:24:010 [WII-RESIDENT] live=L1020/S0 ledger=g3000/u90 nl8000/u90 gx14000/u80 pool=g3500 nl21000 gx24000 residual=g+500 nl+13000 gx+10000 raw2free=5000/d+0 owner=g0/u0 gx13500/u10500 sat=0 under=0",
                "00:24:020 [WII-ISLAND] commit source=1 target=2 dt=7000ms pending=0 priority=0",
                "00:24:030 [WII-ISLAND] commit source=2 target=1 dt=7600ms pending=0 priority=0",
                "00:30:000 [reVC-WII] Game loop exited. Total frames: 9000",
            ]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("final archive ceiling 26624KB is below 28672KB", result.stderr)

    def test_rejects_missing_audio_telemetry(self) -> None:
        result = self.run_tool(
            extra_lines=[
                "00:10:000 [WII-ISLAND] splash prepared target=1 phase=read ready=1",
                "00:20:000 [WII-ISLAND] splash prepared target=2 phase=read ready=1",
                "00:30:000 [reVC-WII] Game loop exited. Total frames: 9000",
            ]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing [WII-AUDIO-POOL] telemetry", result.stderr)

    def test_rejects_missing_archive_ceiling_telemetry(self) -> None:
        result = self.run_tool(
            extra_lines=[
                "00:10:000 [WII-ISLAND] splash prepared target=1 phase=read ready=1",
                "00:20:000 [WII-ISLAND] splash prepared target=2 phase=read ready=1",
                "00:21:000 [WII-AUDIO-POOL] cap=4096KB used=3072KB peak=3584KB free=1024KB largest=1024KB denied=0",
                "00:21:010 [WII-AUDIO-CACHE] hits=120 misses=24 evict=8 queued=24 decode=24 pin=4 pinned=768KB peak=4096KB denied=0",
                "00:21:020 [WII-AUDIO-DECODE] count=24 queue=avg2/max5 service=avg3/max6 total=avg5/max11",
                "00:21:030 [WII-AUDIO-DMA] callbacks=900 underrun=0 silence=0",
                "00:21:050 [WII-STREAM-DIAG] win=5000ms trimEp=2 victims=4 m/t/c/a=3/1/0/0 reload5=0 reload10=0 model=n100/avg600/p95b1000/max2000 txd=n40/avg800/p95b1000/max1800 radar=n0/avg0/p95b0/max0 pending=2 evq=0 dropped=0",
                "00:21:060 [WII-RESIDENT] live=L1020/S0 ledger=g3000/u90 nl8000/u90 gx14000/u80 pool=g3500 nl21000 gx24000 residual=g+500 nl+13000 gx+10000 raw2free=5000/d+0 owner=g0/u0 gx13500/u10500 sat=0 under=0",
                "00:21:070 [WII-ISLAND] commit source=1 target=2 dt=7000ms pending=0 priority=0",
                "00:21:080 [WII-ISLAND] commit source=2 target=1 dt=7600ms pending=0 priority=0",
                "00:30:000 [reVC-WII] Game loop exited. Total frames: 9000",
            ]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing [WII-STREAM] archive ceiling telemetry", result.stderr)

    def test_rejects_audio_pool_denials_and_dma_underrun(self) -> None:
        result = self.run_tool(
            extra_lines=[
                "00:10:000 [WII-ISLAND] splash prepared target=1 phase=read ready=1",
                "00:20:000 [WII-ISLAND] splash prepared target=2 phase=read ready=1",
                "00:21:000 [WII-AUDIO-POOL] cap=4096KB used=3072KB peak=3584KB free=1024KB largest=1024KB denied=2",
                "00:21:010 [WII-AUDIO-CACHE] hits=120 misses=24 evict=8 queued=24 decode=24 pin=4 pinned=768KB peak=4096KB denied=1",
                "00:21:020 [WII-AUDIO-DECODE] count=24 queue=avg2/max5 service=avg3/max6 total=avg5/max11",
                "00:21:030 [WII-AUDIO-DMA] callbacks=900 underrun=3 silence=1",
                "00:21:040 [WII-STREAM] archive ceiling=28160KB->28672KB pressure=0x0 grow=1 retain=1 generic=4096/2048KB rawnl=8192KB gx=8192/4096KB",
                "00:21:050 [WII-STREAM-DIAG] win=5000ms trimEp=2 victims=4 m/t/c/a=3/1/0/0 reload5=0 reload10=0 model=n100/avg600/p95b1000/max2000 txd=n40/avg800/p95b1000/max1800 radar=n0/avg0/p95b0/max0 pending=2 evq=0 dropped=0",
                "00:21:060 [WII-RESIDENT] live=L1020/S0 ledger=g3000/u90 nl8000/u90 gx14000/u80 pool=g3500 nl21000 gx24000 residual=g+500 nl+13000 gx+10000 raw2free=5000/d+0 owner=g0/u0 gx13500/u10500 sat=0 under=0",
                "00:21:070 [WII-ISLAND] commit source=1 target=2 dt=7000ms pending=0 priority=0",
                "00:21:080 [WII-ISLAND] commit source=2 target=1 dt=7600ms pending=0 priority=0",
                "00:30:000 [reVC-WII] Game loop exited. Total frames: 9000",
            ]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("audio pool allocation denied count must remain zero", result.stderr)
        self.assertIn("audio cache allocation denied count must remain zero", result.stderr)
        self.assertIn("audio DMA underrun count must remain zero", result.stderr)

    def test_rejects_profile_build_and_memory_regressions(self) -> None:
        rows = [
            make_a0_event("run_start", 0, 0, profile_id="P5-noaudio-global-lru"),
            make_a0_event("snapshot", 1, 120_000, profile_id="P5-noaudio-global-lru"),
            make_a0_event(
                "snapshot",
                2,
                310_000,
                profile_id="P5-noaudio-global-lru",
                generic_free=128 * 1024,
                generic_largest=64 * 1024,
                process_heap_claim_end=0x91700000,
                raw2_lo=0x91700000,
                gx_free=512 * 1024,
                gx_largest=128 * 1024,
                raw2_remaining=MIB,
                gx_fallback_count=1,
                hard_fallback_count=1,
            ),
        ]
        result = self.run_tool(rows=rows, expected_build="different-build")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("profile must be A0-audio-cache", result.stderr)
        self.assertIn("build must be different-build", result.stderr)
        self.assertIn("gx_fallback_count must remain zero", result.stderr)
        self.assertIn("hard_fallback_count must remain zero", result.stderr)
        self.assertIn("raw2_remaining fell", result.stderr)
        self.assertIn("final gx_free", result.stderr)
        self.assertIn("final generic_largest", result.stderr)

    def test_rejects_cache_without_reuse_or_valid_latency_split(self) -> None:
        result = self.run_tool(
            extra_lines=[
                "00:10:000 [WII-ISLAND] splash prepared target=1 phase=read ready=1",
                "00:20:000 [WII-ISLAND] splash prepared target=2 phase=read ready=1",
                "00:21:000 [WII-AUDIO-POOL] cap=4096KB used=3072KB peak=3584KB free=1024KB largest=1024KB denied=0",
                "00:21:010 [WII-AUDIO-CACHE] hits=0 misses=0 evict=0 queued=0 decode=0 pin=0 pinned=0KB peak=512KB denied=0",
                "00:21:020 [WII-AUDIO-DECODE] count=0 queue=avg4/max7 service=avg5/max8 total=avg8/max12",
                "00:21:030 [WII-AUDIO-DMA] callbacks=900 underrun=0 silence=0",
                "00:21:040 [WII-STREAM] archive ceiling=28160KB->28672KB pressure=0x0 grow=1 retain=1 generic=4096/2048KB rawnl=8192KB gx=8192/4096KB",
                "00:21:050 [WII-STREAM-DIAG] win=5000ms trimEp=2 victims=4 m/t/c/a=3/1/0/0 reload5=0 reload10=0 model=n100/avg600/p95b1000/max2000 txd=n40/avg800/p95b1000/max1800 radar=n0/avg0/p95b0/max0 pending=2 evq=0 dropped=0",
                "00:21:060 [WII-RESIDENT] live=L1020/S0 ledger=g3000/u90 nl8000/u90 gx14000/u80 pool=g3500 nl21000 gx24000 residual=g+500 nl+13000 gx+10000 raw2free=5000/d+0 owner=g0/u0 gx13500/u10500 sat=0 under=0",
                "00:21:070 [WII-ISLAND] commit source=1 target=2 dt=7000ms pending=0 priority=0",
                "00:21:080 [WII-ISLAND] commit source=2 target=1 dt=7600ms pending=0 priority=0",
                "00:30:000 [reVC-WII] Game loop exited. Total frames: 9000",
            ]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("audio cache never decoded a streamed sample", result.stderr)
        self.assertIn("audio cache must record at least one miss", result.stderr)
        self.assertIn("audio cache must record at least one hit", result.stderr)
        self.assertIn("audio decode total avg 8ms is below queue+service 9ms", result.stderr)


if __name__ == "__main__":
    unittest.main()
