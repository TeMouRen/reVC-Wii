from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CMAKE_SOURCE = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BUILD_SOURCE = (REPO_ROOT / "build.sh").read_text(encoding="utf-8")
HUD_SOURCE = (REPO_ROOT / "src" / "renderer" / "Hud.cpp").read_text(
    encoding="utf-8"
)
GX_MEMORY_SOURCE = (
    REPO_ROOT / "vendor" / "librw" / "src" / "gx" / "gxmemory.cpp"
).read_text(encoding="utf-8")
GX_RASTER_SOURCE = (
    REPO_ROOT / "vendor" / "librw" / "src" / "gx" / "gxraster.cpp"
).read_text(encoding="utf-8")


class P15HudWeaponPinTests(unittest.TestCase):
    def test_profile_keeps_p14_layout_and_atomic_handoff(self) -> None:
        self.assertIn(
            "P15-noaudio-hud-weapon-pin:OFF:OFF:6291456:8388608:0",
            BUILD_SOURCE,
        )
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'elseif\(WII_MEMORY_PROFILE_ID STREQUAL '
                r'"P15-noaudio-hud-weapon-pin" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard"\)\s+'
                r'set\(WII_STREAM_ATOMIC_BIG_HANDOFF_VALUE 1\)'
            ),
        )
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'if\(WII_MEMORY_PROFILE_ID STREQUAL '
                r'"P7-noaudio-ps2-world-scan" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard"\)\s+'
                r'set\(WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE 1\)\s+'
                r'else\(\)\s+'
                r'set\(WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE 0\)\s+'
                r'endif\(\)'
            ),
        )

    def test_hud_pins_the_resolved_model_texture_without_a_name_list(self) -> None:
        self.assertRegex(
            HUD_SOURCE,
            re.compile(
                r'ResolveActiveHudWeaponRaster\(CPlayerPed \*playerPed\)'
                r'[\s\S]{0,760}RwTexDictionaryFindNamedTexture\('
                r'[\s\S]{0,180}weaponModel->GetModelName\(\)'
            ),
        )
        self.assertIn(
            "setActiveHudWeaponRaster(activeHudWeaponRaster)", HUD_SOURCE
        )
        self.assertNotIn("WII_HUD_WEAPON_ALLOWLIST", HUD_SOURCE)
        self.assertNotIn("WII_HUD_WEAPON_ALLOWLIST", GX_MEMORY_SOURCE)

    def test_active_raster_survives_the_final_protected_fallback(self) -> None:
        self.assertRegex(
            GX_MEMORY_SOURCE,
            re.compile(
                r'if\(scan->raster == gActiveHudWeaponRaster \|\|\s+'
                r'hasPersistentUiTextureUsage\(scan\) \|\|\s+'
                r'\(!allowProtected && isBudgetProtectedTexture\(scan\)\)\)'
            ),
        )

    def test_destroying_the_raster_clears_the_pin_before_unregister(self) -> None:
        clear_pos = GX_RASTER_SOURCE.index("clearActiveHudWeaponRaster(raster)")
        unregister_pos = GX_RASTER_SOURCE.index("texPoolUnregister(natras->gxData)")
        self.assertLess(clear_pos, unregister_pos)


if __name__ == "__main__":
    unittest.main()
