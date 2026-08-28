from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "tools" / "wii" / "analyze_matfx_assets.py"


class AnalyzeMatfxAssetsTests(unittest.TestCase):
    def test_reports_asset_stats_comparison_and_linear_sweep(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            base_path = root / "base.png"
            env_path = root / "env.png"
            env_other_path = root / "env-other.png"
            output = root / "report.json"
            render_dir = root / "renders"
            Image.new("RGBA", (2, 2), (100, 100, 100, 255)).save(base_path)
            Image.new("RGBA", (2, 2), (0, 0, 0, 255)).save(env_path)
            env = Image.open(env_path)
            env.putpixel((0, 0), (240, 240, 240, 255))
            env.putpixel((1, 1), (120, 120, 120, 255))
            env.save(env_path)
            env_other = Image.new("RGBA", (2, 2), (0, 0, 0, 255))
            env_other.putpixel((0, 0), (200, 200, 200, 255))
            env_other.save(env_other_path)
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--base",
                    str(base_path),
                    "--env",
                    str(env_path),
                    "--compare-env",
                    str(env_other_path),
                    "--output",
                    str(output),
                    "--render-dir",
                    str(render_dir),
                ],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(report["base"]["width"], 2)
            self.assertEqual(report["environment"]["bright_pixels_ge_240"], 1)
            self.assertEqual(report["environment_comparison"]["metrics"]["changed_pixels"], 2)
            sweep = report["environment_coefficient_sweep"]
            self.assertEqual([row["coefficient"] for row in sweep], [0.0, 0.25, 0.5, 1.0])
            self.assertEqual(sweep[0]["mean_contribution"], 0.0)
            self.assertGreater(sweep[-1]["mean_contribution"], sweep[1]["mean_contribution"])
            self.assertTrue((render_dir / "matfx_proxy_coeff_0.png").is_file())


if __name__ == "__main__":
    unittest.main()
