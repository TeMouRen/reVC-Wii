from __future__ import annotations

import hashlib
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
SCRIPT = REPO_ROOT / "tools" / "wii" / "report_streaming_baseline.py"
PROFILE = "P4-noaudio-balanced"
BUILD = "8a0122cab4a2-dirty-20260809T091551Z"


def timestamp(elapsed_ms: int) -> str:
    minutes, remainder = divmod(elapsed_ms, 60_000)
    seconds, milliseconds = divmod(remainder, 1_000)
    return f"{minutes:02d}:{seconds:02d}:{milliseconds:03d}"


def p0_line(event: dict[str, object]) -> str:
    return (
        f"{timestamp(int(event['elapsed_ms']))} host [WII-P0] "
        f"{json.dumps(event, separators=(',', ':'))}"
    )


def diag_line(
    end_ms: int,
    *,
    duration_ms: int = 5_000,
    victims: int,
    reload5: int = 0,
    reload10: int = 0,
    model_count: int = 2,
    model_average: int = 200,
    model_max: int = 400,
    txd_count: int = 1,
    txd_average: int = 300,
    txd_max: int = 500,
    pending: int = 0,
) -> str:
    return (
        f"{timestamp(end_ms)} host [WII-STREAM-DIAG] win={duration_ms}ms "
        f"trimEp={victims} victims={victims} m/t/c/a={victims}/0/0/0 "
        f"reload5={reload5} reload10={reload10} "
        f"model=n{model_count}/avg{model_average}/p95b1000/max{model_max} "
        f"txd=n{txd_count}/avg{txd_average}/p95b1000/max{txd_max} "
        "radar=n0/avg0/p95b0/max0 "
        f"pending={pending} evq=0 dropped=0"
    )


def make_log(
    path: Path,
    *,
    build_id: str = BUILD,
    active_windows: list[dict[str, int]] | None = None,
) -> None:
    windows = active_windows or [
        {"duration_ms": 5_000, "victims": 2, "reload5": 1, "reload10": 1},
        {"duration_ms": 5_000, "victims": 3, "reload5": 0, "reload10": 2},
    ]
    lines = [
        p0_line(
            make_event(
                "run_start",
                0,
                0,
                profile_id=PROFILE,
                build_id=build_id,
            )
        ),
        diag_line(
            5_000,
            victims=0,
            model_count=100,
            model_average=9_000,
            txd_count=100,
            txd_average=9_000,
        ),
    ]
    elapsed_ms = 5_000
    sequence = 1
    lines.append(
        p0_line(
            make_event(
                "snapshot",
                sequence,
                elapsed_ms,
                profile_id=PROFILE,
                build_id=build_id,
                generic_free=2_000,
                generic_largest=1_000,
                gx_free=8_000,
                gx_largest=4_000,
            )
        )
    )
    for index, values in enumerate(windows):
        elapsed_ms += values["duration_ms"]
        lines.append(
            diag_line(
                elapsed_ms,
                duration_ms=values["duration_ms"],
                victims=values["victims"],
                reload5=values.get("reload5", 0),
                reload10=values.get("reload10", 0),
                model_average=200 + index * 100,
                txd_average=300 + index * 100,
                pending=4 + index,
            )
        )
        lines.append(
            f"{timestamp(elapsed_ms - 100)} host [WII-FRAME] sample seq={index} "
            f"work={35 + index * 10:.2f}ms"
        )
        if index == 0:
            lines.append(
                f"{timestamp(elapsed_ms - 50)} host [WII-STREAM] "
                "archive ceiling=24576KB->25088KB pressure=0x0"
            )
        sequence += 1
        lines.append(
            p0_line(
                make_event(
                    "snapshot",
                    sequence,
                    elapsed_ms,
                    profile_id=PROFILE,
                    build_id=build_id,
                    generic_free=2_000 - index * 100,
                    generic_largest=1_000 - index * 100,
                    gx_free=8_000 - index * 100,
                    gx_largest=4_000 - index * 100,
                    request_pending=4 + index,
                )
            )
        )
    lines.extend(
        [
            f"{timestamp(elapsed_ms)} host [WII-ISLAND] splash prepared ready=1",
            f"{timestamp(elapsed_ms)} host [reVC-WII] Game loop exited. Total frames: 100",
        ]
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class StreamingBaselineReportTests(unittest.TestCase):
    def run_tool(
        self,
        temp_root: Path,
        logs: list[Path],
        *extra_args: str,
        output_name: str = "report.json",
    ) -> tuple[subprocess.CompletedProcess[str], Path]:
        dol = temp_root / "main.dol"
        dol.write_bytes(b"baseline-dol")
        output = temp_root / output_name
        command = [
            sys.executable,
            str(SCRIPT),
            "--output",
            str(output),
            "--route-id",
            "washington-beach-v1",
            "--checkpoint",
            "second-splash",
            "--checkpoint",
            "washington-beach",
            "--dol",
            str(dol),
        ]
        for log in logs:
            command.extend(("--log", str(log)))
        command.extend(extra_args)
        result = subprocess.run(
            command,
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        return result, output

    def test_reports_only_the_active_trim_interval(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            log = root / "dolphin-REVC02-20260809-000000-000.log"
            make_log(log)
            result, output = self.run_tool(
                root,
                [log],
                "--expected-profile",
                PROFILE,
                "--expected-build",
                BUILD,
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
            run = report["runs"][0]

            self.assertEqual(report["profile_id"], PROFILE)
            self.assertFalse(report["comparison_readiness"]["ready"])
            self.assertEqual(report["normalization"]["summary_window_count"], 2)
            self.assertEqual(run["active_start_ms"], 5_000)
            self.assertEqual(run["active_duration_ms"], 10_000)
            self.assertEqual(run["streaming"]["victims"], 5)
            self.assertEqual(run["streaming"]["reload10"], 3)
            self.assertAlmostEqual(run["streaming"]["model_weighted_average_ms"], 250.0)
            self.assertEqual(run["streaming"]["pending_max"], 5)
            self.assertEqual(run["archive_ceiling"]["grow_count"], 1)
            self.assertEqual(run["failure_markers"]["txd_fail"], 0)
            self.assertIn("memory_active_interval", run)
            self.assertIn("frame_samples_full_run", run)
            self.assertEqual(run["frame_samples"]["work_over_40ms"], 1)
            self.assertEqual(run["frame_samples"]["scope"], "diagnostic-log-samples-not-all-frames")
            self.assertEqual(
                report["dol_sha256"],
                hashlib.sha256(b"baseline-dol").hexdigest().upper(),
            )

    def test_aligns_multiple_runs_by_complete_window_count(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "dolphin-REVC02-20260809-000000-001.log"
            second = root / "dolphin-REVC02-20260809-000000-002.log"
            make_log(
                first,
                active_windows=[
                    {"duration_ms": 5_000, "victims": 2, "reload10": 1},
                    {"duration_ms": 5_000, "victims": 2, "reload10": 1},
                    {"duration_ms": 5_000, "victims": 20, "reload10": 20},
                ],
            )
            make_log(
                second,
                active_windows=[
                    {"duration_ms": 5_100, "victims": 4, "reload10": 2},
                    {"duration_ms": 5_100, "victims": 4, "reload10": 2},
                ],
            )
            result, output = self.run_tool(root, [first, second])
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))

            self.assertEqual(report["run_count"], 2)
            self.assertEqual(report["normalization"]["summary_window_count"], 2)
            self.assertEqual([run["streaming"]["victims"] for run in report["runs"]], [4, 8])
            reload = report["aggregate"]["streaming.reload10_per_second"]
            self.assertAlmostEqual(reload["median"], (0.2 + 4 / 10.2) / 2)
            self.assertAlmostEqual(reload["worst"], 4 / 10.2)

    def test_rejects_mixed_builds_and_does_not_write_output(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "dolphin-REVC02-20260809-000000-001.log"
            second = root / "dolphin-REVC02-20260809-000000-002.log"
            make_log(first)
            make_log(second, build_id="other-build")
            result, output = self.run_tool(root, [first, second])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("multiple builds", result.stderr)
            self.assertFalse(output.exists())

    def test_marks_three_matched_runs_ready_for_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            logs = []
            for index in range(3):
                log = root / f"dolphin-REVC02-20260809-000000-00{index}.log"
                make_log(log)
                logs.append(log)
            result, output = self.run_tool(root, logs)
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertTrue(report["comparison_readiness"]["ready"])
            self.assertTrue(
                report["comparison_readiness"]["manual_visual_confirmation_required"]
            )

    def test_refuses_to_overwrite_an_existing_report(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            log = root / "dolphin-REVC02-20260809-000000-001.log"
            make_log(log)
            output = root / "report.json"
            output.write_text("existing\n", encoding="utf-8")
            result, _ = self.run_tool(root, [log])
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusing to overwrite", result.stderr)
            self.assertEqual(output.read_text(encoding="utf-8"), "existing\n")


if __name__ == "__main__":
    unittest.main()
