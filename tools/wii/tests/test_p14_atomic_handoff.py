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


class P14AtomicHandoffTests(unittest.TestCase):
    def test_profile_is_an_isolated_no_audio_p7_derivative(self) -> None:
        self.assertIn(
            "P14-noaudio-atomic-handoff:OFF:OFF:6291456:8388608:0",
            BUILD_SOURCE,
        )
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'WII_MEMORY_PROFILE_ID STREQUAL "P7-noaudio-ps2-world-scan" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P14-noaudio-atomic-handoff" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan"\)\s+'
                r'set\(WII_STREAM_PS2_WORLD_SCAN_RADIUS_VALUE 1\)'
            ),
        )
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'if\(WII_MEMORY_PROFILE_ID STREQUAL "P14-noaudio-atomic-handoff"\)\s+'
                r'set\(WII_STREAM_ATOMIC_BIG_HANDOFF_VALUE 1\)'
            ),
        )
        self.assertNotRegex(
            CMAKE_SOURCE,
            re.compile(
                r'WII_MEMORY_PROFILE_ID STREQUAL "P14-noaudio-atomic-handoff"[\s\S]{0,120}'
                r'set\(WII_STREAM_GX_HEADROOM_GUARD_VALUE 1\)'
            ),
        )

    def test_read_requests_official_big_building_set(self) -> None:
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r'#if WII_STREAM_ATOMIC_BIG_HANDOFF\s+'
                r'if\(gWiiIslandPhase == WII_ISLAND_READ\)\s+'
                r'CStreaming::RequestBigBuildings\(gWiiIslandTargetLevel,\s+'
                r'gWiiIslandAtomicHandoffPosition\);'
            ),
        )
        self.assertNotIn("WII_ISLAND_ATOMIC_BIG_ALLOWLIST", STREAMING_SOURCE)

    def test_atomic_handoff_position_is_frozen_at_transition_begin(self) -> None:
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r'gWiiIslandWorkPosition = position;\s+'
                r'#if WII_STREAM_ATOMIC_BIG_HANDOFF\s+'
                r'// Freeze the original landing sample[\s\S]{0,220}'
                r'gWiiIslandAtomicHandoffPosition = position;'
            ),
        )

    def test_source_big_buildings_are_retired_behind_splash(self) -> None:
        read_start = STREAMING_SOURCE.index("WiiIslandEnterReadPhase(void)")
        read_end = STREAMING_SOURCE.index(
            "static bool\nWiiIslandCommitTransition", read_start
        )
        read_body = STREAMING_SOURCE[read_start:read_end]
        splash = read_body.index("WiiBeginIslandTransitionSplash")
        retire = read_body.index("CStreaming::RemoveBigBuildings(gWiiIslandSourceLevel)")
        self.assertLess(splash, retire)

    def test_atomic_readiness_requires_model_txd_and_instance(self) -> None:
        self.assertIn("WiiIslandCountAtomicBigBuildings", STREAMING_SOURCE)
        self.assertIn("!building->bStreamBIGBuilding", STREAMING_SOURCE)
        self.assertIn(
            "!CRenderer::ShouldModelBeStreamed(building, position)",
            STREAMING_SOURCE,
        )
        self.assertIn("modelInfo->GetTxdSlot()", STREAMING_SOURCE)
        self.assertIn("building->m_rwObject == nil", STREAMING_SOURCE)
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r'if\(gWiiIslandPhase == WII_ISLAND_READ\)\{[\s\S]{0,420}'
                r'WiiIslandCountAtomicBigBuildings[\s\S]{0,180}'
                r'missingRequired \+= atomicMissing;'
            ),
        )
        self.assertIn("gWiiIslandAtomicHandoffPosition", STREAMING_SOURCE)

    def test_proxy_is_removed_before_splash_ends_only_after_atomic_ready(self) -> None:
        commit_start = STREAMING_SOURCE.index("WiiIslandCommitTransition(void)")
        commit_end = STREAMING_SOURCE.index(
            "static void\nWiiIslandReturnPlayerToSource", commit_start
        )
        commit_body = STREAMING_SOURCE[commit_start:commit_end]
        ready = commit_body.index("gWiiIslandAtomicBigReady = true")
        remove = commit_body.index(
            "CStreaming::RemoveIslandsNotUsed(gWiiIslandTargetLevel)"
        )
        splash_end = commit_body.index("WiiEndIslandTransitionSplash")
        self.assertLess(ready, remove)
        self.assertLess(remove, splash_end)

    def test_proxy_cannot_be_removed_before_atomic_ready(self) -> None:
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r'CStreaming::RemoveIslandsNotUsed\(eLevelName level\)[\s\S]{0,240}'
                r'WII_STREAM_ATOMIC_BIG_HANDOFF[\s\S]{0,180}'
                r'!gWiiIslandAtomicBigReady\)\s+return;'
            ),
        )


if __name__ == "__main__":
    unittest.main()
