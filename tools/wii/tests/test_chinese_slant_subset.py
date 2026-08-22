import re
import unittest
from collections import Counter
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SRC_ROOT = REPO_ROOT / "src"
FONT_CPP = SRC_ROOT / "renderer" / "Font.cpp"
HUD_CPP = SRC_ROOT / "renderer" / "Hud.cpp"
MUSIC_CPP = SRC_ROOT / "audio" / "MusicManager.cpp"
USER_CPP = SRC_ROOT / "core" / "User.cpp"
SET_SLANT_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:CFont::)?SetSlant\(\s*([^)]*?)\s*\)"
)
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inl"}


class ChineseSlantSubsetTests(unittest.TestCase):
    def test_all_slant_callers_match_the_enumerated_text_sources(self):
        calls = Counter()
        for path in SRC_ROOT.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in CPP_SUFFIXES:
                continue
            source = path.read_text(encoding="utf-8", errors="ignore")
            for match in SET_SLANT_RE.finditer(source):
                argument = re.sub(r"\s+", "", match.group(1))
                if argument == "floats":
                    continue
                calls[(path.relative_to(REPO_ROOT).as_posix(), argument)] += 1

        self.assertEqual(
            calls,
            Counter({
                ("src/audio/MusicManager.cpp", "0.15f"): 1,
                ("src/audio/MusicManager.cpp", "0.0f"): 2,
                ("src/renderer/Font.cpp", "0.0f"): 1,
                ("src/renderer/Hud.cpp", "0.15f"): 2,
                ("src/renderer/Hud.cpp", "0.0f"): 2,
                ("src/renderer/Hud.cpp", "0.f"): 2,
            }),
        )

        music = MUSIC_CPP.read_text(encoding="utf-8")
        station_start = music.index("static wchar ModernTalkingStationText[]")
        station_end = music.index("};", station_start) + 2
        music[station_start:station_end].encode("ascii")
        self.assertIn("string = ModernTalkingStationText;", music)

        hud = HUD_CPP.read_text(encoding="utf-8")
        user = USER_CPP.read_text(encoding="utf-8")
        self.assertIn("wchar *zoneText = m_ZoneToPrint;", hud)
        self.assertIn("wchar *vehicleText = m_pVehicleNameToPrint;", hud)
        self.assertIn("text = m_pZone->GetTranslatedName();", user)
        self.assertIn("text = m_pZone2->GetTranslatedName();", user)
        self.assertIn("m_pCurrentVehicle->GetModelIndex()))->m_gameName", user)

    def test_runtime_reports_the_first_unexpected_subset_miss(self):
        source = FONT_CPP.read_text(encoding="utf-8")
        self.assertIn("ChsSlantSubsetMissReported", source)
        self.assertIn("compact slant atlas missing U+%04X; using base atlas", source)
        self.assertIn("chinese_wm_slant.map", source)
        self.assertIn('"chinese_wm_slant", "wm_vcchs_font_slant_i8"', source)
        self.assertNotIn("chinese_wm_slant_subset", source)


if __name__ == "__main__":
    unittest.main()
