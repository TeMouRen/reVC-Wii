from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CMAKE_SOURCE = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
STREAMING_SOURCE = (REPO_ROOT / "src" / "core" / "Streaming.cpp").read_text(
    encoding="utf-8"
)


class P17RecentVisibleTxdTests(unittest.TestCase):
    def test_only_p7_enables_recent_visible_txd_guard(self) -> None:
        guard_block = re.search(
            r'if\(WII_MEMORY_PROFILE_ID STREQUAL "P7-noaudio-ps2-world-scan" OR\s+'
            r'WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan"\)\s+'
            r'set\(WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE 1\)\s+'
            r'else\(\)\s+'
            r'set\(WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE 0\)',
            CMAKE_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(guard_block)
        assert guard_block is not None
        self.assertNotIn("P16-noaudio-gx-headroom-guard", guard_block.group(0))

    def test_compile_definition_is_exported(self) -> None:
        self.assertIn(
            "WII_STREAM_P7_VISIBLE_TXD_GUARD=${WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE}",
            CMAKE_SOURCE,
        )
        self.assertIn(
            'message(STATUS "  P7 recent TXD guard: ${WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE}")',
            CMAKE_SOURCE,
        )
        self.assertIn("#ifndef WII_STREAM_P7_VISIBLE_TXD_GUARD", STREAMING_SOURCE)
        self.assertIn(
            "WII_STREAM_P7_RECENT_VISIBLE_TXD_GRACE_MS = 1000u",
            STREAMING_SOURCE,
        )

    def test_gx_and_archive_txd_passes_protect_recent_visible_textures(self) -> None:
        self.assertIn(
            "static uint32 gWiiStreamRecentVisibleTxdMs[STREAM_OFFSET_COL - STREAM_OFFSET_TXD];",
            STREAMING_SOURCE,
        )
        self.assertIn("WiiStreamMarkRecentVisibleTxd(streamId);", STREAMING_SOURCE)
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r"if\(\(poolBit == WII_STREAM_PRESSURE_GX \|\| poolBit == 0\) &&\s+"
                r"streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL\)\{\s+"
                r"if\(recentVisibleNowMs == 0\)\s+"
                r"recentVisibleNowMs = WiiStreamNowMs\(\);\s+"
                r"if\(WiiStreamProtectsRecentVisibleTxd\(streamId, recentVisibleNowMs\)\)\{\s+"
                r"if\(recentVisibleFallbackId == -1\)\s+"
                r"recentVisibleFallbackId = streamId;\s+"
                r"continue;",
                re.DOTALL,
            ),
        )
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r"if\(poolBit == WII_STREAM_PRESSURE_GX && pass == 0 &&\s+"
                r"recentVisibleFallbackId != -1\)\{[\s\S]*?"
                r"CStreaming::RemoveModel\(recentVisibleFallbackId\);",
                re.DOTALL,
            ),
        )

    def test_same_frame_world_load_protection_stays_stronger_than_recent_visible_guard(self) -> None:
        same_frame = STREAMING_SOURCE.index(
            "if(WiiStreamProtectsSameFrameWorldLoad(streamId)){"
        )
        recent_visible = STREAMING_SOURCE.index(
            "if((poolBit == WII_STREAM_PRESSURE_GX || poolBit == 0) &&"
        )
        self.assertLess(same_frame, recent_visible)


if __name__ == "__main__":
    unittest.main()
