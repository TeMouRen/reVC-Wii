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


class P6Ps2PolicyTests(unittest.TestCase):
    def test_profile_keeps_p4_physical_layout_and_disables_audio(self) -> None:
        self.assertIn(
            "P6-noaudio-ps2-policy:OFF:OFF:6291456:8388608:0",
            BUILD_SOURCE,
        )
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'WII_MEMORY_PROFILE_ID STREQUAL "P4-noaudio-balanced" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P5-noaudio-global-lru" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "P6-noaudio-ps2-policy"'
            ),
        )

    def test_only_p6_enables_transition_purge(self) -> None:
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'if\(WII_MEMORY_PROFILE_ID STREQUAL "P6-noaudio-ps2-policy"\)\s+'
                r'set\(WII_STREAM_PS2_TRANSITION_PURGE_VALUE 1\)\s+'
                r'else\(\)\s+'
                r'set\(WII_STREAM_PS2_TRANSITION_PURGE_VALUE 0\)'
            ),
        )
        self.assertIn(
            "WII_STREAM_PS2_TRANSITION_PURGE=${WII_STREAM_PS2_TRANSITION_PURGE_VALUE}",
            CMAKE_SOURCE,
        )

    def test_p6_does_not_enable_pressure_free_global_lru(self) -> None:
        global_lru_block = re.search(
            r'if\(WII_MEMORY_PROFILE_ID STREQUAL "P5-noaudio-global-lru".*?endif\(\)',
            CMAKE_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(global_lru_block)
        self.assertNotIn("P6-noaudio-ps2-policy", global_lru_block.group(0))

    def test_transition_purge_is_compile_time_isolated(self) -> None:
        self.assertIn(
            "#if defined(WII) && WII_STREAM_PS2_TRANSITION_PURGE",
            STREAMING_SOURCE,
        )
        self.assertIn("[WII-PS2-PURGE]", STREAMING_SOURCE)

    def test_transition_purge_is_single_shot(self) -> None:
        self.assertIn("static bool gWiiIslandPs2PurgeDone", STREAMING_SOURCE)
        self.assertIn("gWiiIslandPs2PurgeDone = false", STREAMING_SOURCE)
        self.assertIn(
            "gWiiIslandPhase != WII_ISLAND_READ || gWiiIslandPs2PurgeDone",
            STREAMING_SOURCE,
        )
        self.assertIn("gWiiIslandPs2PurgeDone = true", STREAMING_SOURCE)

    def test_purge_respects_dependencies_and_reports_resource_types(self) -> None:
        self.assertIn("IsTxdUsedByRequestedModels(txdId)", STREAMING_SOURCE)
        self.assertIn("CTxdStore::IsTxdAliasPinned(txdId)", STREAMING_SOURCE)
        self.assertIn("vehicles=%d models=%d txds=%d", STREAMING_SOURCE)
        self.assertIn("skipDeps=%d skipAlias=%d", STREAMING_SOURCE)
        self.assertNotIn("RemoveCol(streamId)", STREAMING_SOURCE)
        self.assertNotIn("RemoveAnim(streamId)", STREAMING_SOURCE)


if __name__ == "__main__":
    unittest.main()
