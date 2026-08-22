from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_report_streaming_baseline import BUILD, PROFILE, make_log, timestamp


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "tools" / "wii" / "report_streaming_diagnostics.py"


def append_diagnostics(path: Path) -> None:
    lines = [
        f"{timestamp(1_000)} host [WII-STREAM-DIAG] enabled state=210KB "
        "events=128 detail=per-resource summary=5000ms schema=lifecycle-v2 "
        "pools=used/free/largest bytes nl=used/free/raw",
        f"{timestamp(10_000)} host [WII-CHURN] rank=1 id=42 type=model "
        "name='beachlod' trim=3 r5=2 r10=2 load=2 gap=1200/1800 "
        "queue=400/700 service=800/1100 reason=archive-cap req=visible "
        "age=req500/load3000/vis200 cf=7000/9000 bypass=3 loc=1/0 dropped=1",
        f"{timestamp(15_000)} host [WII-CHURN] rank=1 id=42 type=model "
        "name='beachlod' trim=2 r5=1 r10=2 load=1 gap=2000/2600 "
        "queue=600/600 service=900/900 reason=archive-cap req=visible "
        "age=req600/load2500/vis300 cf=7001/10000 bypass=2 loc=1/0 dropped=2",
        f"{timestamp(15_100)} host [WII-CHURN] rank=2 id=7000 type=txd "
        "name='hud' trim=1 r5=0 r10=1 load=1 gap=4000/4000 "
        "queue=200/200 service=300/300 reason=pool-gx req=normal "
        "age=req5000/load8000/vis4294967295 cf=-1/4294967295 bypass=0 "
        "loc=1/0 dropped=2",
        f"{timestamp(10_100)} host [WII-FRAME-HIST] win=5000ms frames=150 risky=8 "
        "over40=4 over50=1 work=avg18000/p95b40000/max52000 "
        "hist=100/25/15/6/3/1 stream=frames30/calls40/remove20/a18/p2 "
        "make=total120000/max8000 removeUs=70000 corr=stream403/nostream401",
        f"{timestamp(15_200)} host [WII-FRAME-HIST] win=5000ms frames=150 risky=6 "
        "over40=2 over50=0 work=avg17000/p95b33330/max45000 "
        "hist=110/20/14/4/2/0 stream=frames20/calls25/remove10/a10/p0 "
        "make=total60000/max5000 removeUs=30000 corr=stream401/nostream401",
        f"{timestamp(10_200)} host [WII-TRIM-VICTIM] id=42",
        f"{timestamp(10_300)} host [WII-TRIM-RELOAD] id=42",
        f"{timestamp(10_400)} host [WII-LOAD-LAT] id=42 total=1200 "
        "queue=400 service=800",
        f"{timestamp(15_300)} host [WII-ISLAND] splash prepared ready=1",
    ]
    with path.open("a", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")


class StreamingDiagnosticReportTests(unittest.TestCase):
    def run_tool(self, root: Path, log: Path) -> tuple[subprocess.CompletedProcess[str], Path]:
        dol = root / "main.dol"
        dol.write_bytes(b"diagnostic-dol")
        output = root / "diagnostic.json"
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--log",
                str(log),
                "--output",
                str(output),
                "--route-id",
                "washington-beach-v1",
                "--checkpoint",
                "second-splash",
                "--checkpoint",
                "washington-beach",
                "--expected-profile",
                PROFILE,
                "--expected-build",
                BUILD,
                "--dol",
                str(dol),
            ],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
        )
        return result, output

    def test_reports_churn_latency_split_and_frame_correlation(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            log = root / "dolphin.log"
            make_log(log)
            append_diagnostics(log)
            result, output = self.run_tool(root, log)
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))

            self.assertTrue(report["readiness"]["ready"])
            self.assertFalse(report["readiness"]["performance_acceptance_allowed"])
            self.assertEqual(report["diagnostic_contract"]["schema"], "lifecycle-v2")
            self.assertEqual(report["diagnostic_contract"]["raw_trim_events"], 1)
            self.assertEqual(report["diagnostic_contract"]["churn_replacement_count_max"], 2)
            split = report["diagnostic_contract"]["latency_split"]
            self.assertTrue(split["valid"])
            self.assertEqual(split["checked_events"], 1)
            self.assertEqual(split["invalid_events"], 0)
            top = report["top_churn"][0]
            self.assertEqual(top["name"], "beachlod")
            self.assertEqual(top["trims"], 5)
            self.assertEqual(top["reload10"], 4)
            self.assertEqual(top["class_bias_bypasses"], 5)
            self.assertAlmostEqual(top["queue_average_ms"], 1400 / 3)
            self.assertAlmostEqual(top["service_average_ms"], 2500 / 3)
            frames = report["frame_histograms"]
            self.assertEqual(frames["frames"], 300)
            self.assertEqual(frames["over_40ms"], 6)
            self.assertEqual(frames["stream_frames"], 50)
            self.assertEqual(frames["removals"], 30)
            self.assertEqual(frames["stream_over_40ms"], 4)
            self.assertEqual(frames["no_stream_over_40ms"], 2)

    def test_marks_mixed_clock_latency_split_not_ready(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            log = root / "dolphin.log"
            make_log(log)
            append_diagnostics(log)
            text = log.read_text(encoding="utf-8").replace(
                "total=1200 queue=400 service=800",
                "total=1200 queue=400 service=1200",
            )
            log.write_text(text, encoding="utf-8")
            result, output = self.run_tool(root, log)
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertFalse(report["readiness"]["ready"])
            self.assertFalse(report["readiness"]["latency_split_valid"])
            self.assertIn("latency split is invalid", report["readiness"]["reason"])
            split = report["diagnostic_contract"]["latency_split"]
            self.assertFalse(split["valid"])
            self.assertEqual(split["invalid_events"], 1)
            self.assertEqual(split["maximum_error_ms"], 400)

    def test_rejects_summary_only_log(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            log = root / "dolphin.log"
            make_log(log)
            with log.open("a", encoding="utf-8") as handle:
                handle.write(
                    f"{timestamp(1_000)} host [WII-STREAM-DIAG] enabled state=63KB "
                    "events=1 detail=summary-only summary=5000ms schema=lifecycle-v2\n"
                )
            result, output = self.run_tool(root, log)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not a per-resource", result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
