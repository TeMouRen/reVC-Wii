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
class P15QualityTierTests(unittest.TestCase):
    def test_profile_keeps_p15_physical_tuple(self) -> None:
        self.assertIn(
            "P15-noaudio-hud-weapon-pin:OFF:OFF:6291456:8388608:0",
            BUILD_SOURCE,
        )
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'if\(WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard"\)\s+'
                r'set\(WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE 1\)'
            ),
        )

    def test_used_weapon_raster_is_persisted_by_model_txd_resolution(self) -> None:
        resolver = re.search(
            r'ResolveActiveHudWeaponRaster\(CPlayerPed \*playerPed\)'
            r'[\s\S]{0,1300}?return weaponRaster;',
            HUD_SOURCE,
        )
        self.assertIsNotNone(resolver)
        assert resolver is not None
        self.assertIn("weaponModel->GetTxdSlot()", resolver.group(0))
        self.assertIn("markPersistentUiTexture(weaponRaster)", resolver.group(0))
        self.assertNotIn("ALLOWLIST", resolver.group(0))

    def test_opaque_quality_tier_preserves_dimensions(self) -> None:
        demotion = re.search(
            r'static bool\s+demoteOpaqueRGBA8ToRGB565\([\s\S]*?#else',
            GX_MEMORY_SOURCE,
        )
        self.assertIsNotNone(demotion)
        assert demotion is not None
        source = demotion.group(0)
        self.assertIn("raster->type != Raster::TEXTURE", source)
        self.assertIn("natras->cpuData != nil", source)
        self.assertIn("natras->hasAlpha != 0", source)
        self.assertIn("natras->alphaKind != GX_RASTER_ALPHA_NONE", source)
        self.assertIn("oldPoolBit = texPoolStoragePoolBit(oldData)", source)
        self.assertIn("WiiMemoryRecordResidentDelta(entry->ownerStreamId, oldPoolBit", source)
        self.assertIn("natras->gxFmt = GX_TF_RGB565", source)
        self.assertNotIn("raster->width", source)
        self.assertNotIn("raster->height", source)

    def test_failed_opaque_demotion_does_not_fall_through_to_resize(self) -> None:
        self.assertRegex(
            GX_MEMORY_SOURCE,
            re.compile(
                r'if\(scan->gxFmt == GX_TF_RGBA8[\s\S]{0,650}'
                r'if\(demoteOpaqueRGBA8ToRGB565[\s\S]{0,450}'
                r'scanPrev = &scan->next;\s+scan = scan->next;\s+continue;'
            ),
        )

    def test_fragmented_gx_replacement_uses_transactional_staging(self) -> None:
        demotion = re.search(
            r'demoteOpaqueRGBA8ToRGB565\([\s\S]{0,5200}?return true;',
            GX_MEMORY_SOURCE,
        )
        self.assertIsNotNone(demotion)
        assert demotion is not None
        source = demotion.group(0)
        stage_pos = source.index("allocCpuTemp(newSize, 32)")
        convert_pos = source.index("convertGX_RGBA8ToOpaqueRGB565(stagedData")
        free_pos = source.index("gxMemFree(oldData)", convert_pos)
        retry_pos = source.index("newData = gxMemAlloc(newSize, 32)", free_pos)
        fallback_pos = source.index("newData = stagedData", retry_pos)
        self.assertLess(stage_pos, convert_pos)
        self.assertLess(convert_pos, free_pos)
        self.assertLess(free_pos, retry_pos)
        self.assertLess(retry_pos, fallback_pos)
        self.assertLess(source.index("GX_DrawDone()"), free_pos)

if __name__ == "__main__":
    unittest.main()
