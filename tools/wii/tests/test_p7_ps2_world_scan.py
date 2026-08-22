from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CMAKE_SOURCE = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BUILD_SOURCE = (REPO_ROOT / "build.sh").read_text(encoding="utf-8")
STREAMING_SOURCE = (REPO_ROOT / "src" / "core" / "Streaming.cpp").read_text(
    encoding="utf-8"
)


class P7Ps2WorldScanTests(unittest.TestCase):
    def test_profile_reuses_p4_layout_and_disables_audio(self) -> None:
        self.assertIn(
            "P7-noaudio-ps2-world-scan:OFF:OFF:6291456:8388608:0",
            BUILD_SOURCE,
        )
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'WII_MEMORY_PROFILE_ID STREQUAL "P6-noaudio-ps2-policy" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P7-noaudio-ps2-world-scan"'
            ),
        )

    def test_p7_keeps_archive_lru_and_transition_purge_disabled(self) -> None:
        global_lru_block = re.search(
            r'if\(WII_MEMORY_PROFILE_ID STREQUAL "P5-noaudio-global-lru".*?endif\(\)',
            CMAKE_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(global_lru_block)
        self.assertNotIn("P7-noaudio-ps2-world-scan", global_lru_block.group(0))

        purge_block = re.search(
            r'if\(WII_MEMORY_PROFILE_ID STREQUAL "P6-noaudio-ps2-policy"\).*?'
            r'set\(WII_STREAM_PS2_TRANSITION_PURGE_VALUE 1\).*?'
            r'else\(\).*?set\(WII_STREAM_PS2_TRANSITION_PURGE_VALUE 0\)',
            CMAKE_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(purge_block)
        self.assertNotIn("P7-noaudio-ps2-world-scan", purge_block.group(0))

    def test_p7_disables_adaptive_archive_ceiling(self) -> None:
        adaptive_one = CMAKE_SOURCE.index(
            "set(WII_STREAM_ADAPTIVE_ARCHIVE_CEILING_VALUE 1)"
        )
        adaptive_start = CMAKE_SOURCE.rfind(
            "if(WII_MEMORY_PROFILE_ID", 0, adaptive_one
        )
        adaptive_zero = CMAKE_SOURCE.index(
            "set(WII_STREAM_ADAPTIVE_ARCHIVE_CEILING_VALUE 0)", adaptive_one
        )
        adaptive_block = CMAKE_SOURCE[adaptive_start:adaptive_zero]
        self.assertIn("WII_STREAM_ADAPTIVE_ARCHIVE_CEILING_VALUE 1", adaptive_block)
        self.assertNotIn("P7-noaudio-ps2-world-scan", adaptive_block)

    def test_p7_and_atomic_derivatives_enable_ps2_world_scan_radius(self) -> None:
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'if\(WII_MEMORY_PROFILE_ID STREQUAL "P7-noaudio-ps2-world-scan" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P14-noaudio-atomic-handoff" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan"\)\s+'
                r'set\(WII_STREAM_PS2_WORLD_SCAN_RADIUS_VALUE 1\)\s+'
                r'else\(\)\s+'
                r'set\(WII_STREAM_PS2_WORLD_SCAN_RADIUS_VALUE 0\)'
            ),
        )
        self.assertIn(
            "WII_STREAM_PS2_WORLD_SCAN_RADIUS=${WII_STREAM_PS2_WORLD_SCAN_RADIUS_VALUE}",
            CMAKE_SOURCE,
        )
        self.assertIn(
            "lodDistSq = Max(lodDistSq, sq(STREAM_DIST));",
            STREAMING_SOURCE,
        )
        self.assertIn(
            "lodDistSq = Min(lodDistSq, sq(STREAM_DIST));",
            STREAMING_SOURCE,
        )

    def test_p7_leaves_blocking_handoff_for_audio_derivative(self) -> None:
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'if\(WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan"\)\s+'
                r'set\(WII_STREAM_P7_BLOCKING_HANDOFF_VALUE 1\)\s+'
                r'else\(\)\s+'
                r'set\(WII_STREAM_P7_BLOCKING_HANDOFF_VALUE 0\)'
            ),
        )
        self.assertIn(
            "WII_STREAM_P7_BLOCKING_HANDOFF=${WII_STREAM_P7_BLOCKING_HANDOFF_VALUE}",
            CMAKE_SOURCE,
        )
        self.assertIn(
            "#ifndef WII_STREAM_P7_BLOCKING_HANDOFF",
            STREAMING_SOURCE,
        )
        self.assertNotIn("WiiIslandTransitionOwnsSpatialRetire", STREAMING_SOURCE)
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r'gWiiIslandPhase = WII_ISLAND_RETIRE;\s+'
                r'#if WII_STREAM_P7_BLOCKING_HANDOFF.*?'
                r'const CVector &spatialRetireOrigin = TheCamera.GetPosition\(\);\s+'
                r'CStreaming::ms_oldSectorX =\s+'
                r'CWorld::GetSectorIndexX\(spatialRetireOrigin\.x\);\s+'
                r'CStreaming::ms_oldSectorY =\s+'
                r'CWorld::GetSectorIndexY\(spatialRetireOrigin\.y\);\s+'
                r'#endif',
                re.DOTALL,
            ),
        )

    def test_p7_releases_ready_target_before_old_level_retire_finishes(self) -> None:
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r'if\(visualReady \|\| visualTimeout\)\{.*?'
                r'gWiiIslandVisualHandoffPending = false;.*?'
                r'#if WII_STREAM_P7_BLOCKING_HANDOFF\s+'
                r'if\(visualReady\)\s+'
                r'WiiIslandReleaseRetireProtection\("handoff"\);\s+'
                r'#endif',
                re.DOTALL,
            ),
        )

    def test_p7_uses_real_pool_pressure_without_archive_repayment(self) -> None:
        self.assertNotIn(
            "WII_STREAM_P7_RETIRE_ARCHIVE_MAX_REMOVALS_PER_FRAME",
            STREAMING_SOURCE,
        )
        self.assertNotIn("p7RetireArchiveRepay", STREAMING_SOURCE)
        self.assertNotIn("archiveRetentionTarget", STREAMING_SOURCE)
        self.assertNotIn("deferArchiveTrim", STREAMING_SOURCE)
        self.assertNotIn("transitionAdmissionHeadroom", STREAMING_SOURCE)
        self.assertNotIn(
            "IsIslandTransitionActive() || archiveRetentionHeadroom",
            STREAMING_SOURCE,
        )
        self.assertIn(
            "while(pressureRemovals < WII_STREAM_PRESSURE_MAX_REMOVALS)",
            STREAMING_SOURCE,
        )

    def test_p7_disables_later_hud_and_visible_txd_guards(self) -> None:
        hud_block = re.search(
            r'if\(WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan" OR\s+'
            r'WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR\s+'
            r'WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard"\)\s+'
            r'set\(WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE 1\).*?'
            r'else\(\).*?set\(WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE 0\)',
            CMAKE_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(hud_block)
        assert hud_block is not None
        self.assertNotIn("P7-noaudio-ps2-world-scan", hud_block.group(0))

        visible_txd_block = re.search(
            r'if\(WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan"\)\s+'
            r'set\(WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE 1\).*?'
            r'else\(\).*?set\(WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE 0\)',
            CMAKE_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(visible_txd_block)
        assert visible_txd_block is not None
        self.assertNotIn("P7-noaudio-ps2-world-scan", visible_txd_block.group(0))

    def test_p7_splash_visual_gate_requires_visible_target_big_buildings(self) -> None:
        self.assertIn(
            "WII_STREAM_SPLASH_VISUAL_GATE=${WII_STREAM_SPLASH_VISUAL_GATE_VALUE}",
            CMAKE_SOURCE,
        )
        self.assertIn("WII_STREAM_SPLASH_VISUAL_GATE", STREAMING_SOURCE)
        self.assertIn("WII_ISLAND_SPLASH_MIN_DISPLAY_MS", STREAMING_SOURCE)
        working_set_block = re.search(
            r'static void\s+WiiIslandRequestWorkingSet\(void\).*?'
            r'static void\s+WiiIslandCaptureRetireProtection',
            STREAMING_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(working_set_block)
        assert working_set_block is not None
        self.assertIn("WiiIslandRequestVisibleBigBuildings", working_set_block.group(0))
        self.assertIn("splashWindowReady", STREAMING_SOURCE)


if __name__ == "__main__":
    unittest.main()
