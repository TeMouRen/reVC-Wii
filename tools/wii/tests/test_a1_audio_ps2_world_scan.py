from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CMAKE_SOURCE = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BUILD_SOURCE = (REPO_ROOT / "build.sh").read_text(encoding="utf-8")


class A1AudioPs2WorldScanTests(unittest.TestCase):
    def test_profile_adds_only_the_bounded_audio_pool_to_p7_layout(self) -> None:
        self.assertIn(
            "A1-audio-ps2-world-scan:OFF:ON:6291456:8388608:4194304",
            BUILD_SOURCE,
        )
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan".*?'
                r'NOT WII_AUDIO_DECODE_ENABLE.*?'
                r'WII_AUDIO_MEM2_POOL_BYTES STREQUAL "4194304"',
                re.DOTALL,
            ),
        )

    def test_profile_reuses_p7_streaming_policy_without_global_lru(self) -> None:
        for setting in (
            "WII_STREAM_PS2_WORLD_SCAN_RADIUS_VALUE",
            "WII_STREAM_P7_BLOCKING_HANDOFF_VALUE",
            "WII_STREAM_P7_VISIBLE_TXD_GUARD_VALUE",
        ):
            block = re.search(
                rf'if\(.*?A1-audio-ps2-world-scan.*?set\({setting} 1\).*?endif\(\)',
                CMAKE_SOURCE,
                re.DOTALL,
            )
            self.assertIsNotNone(block, msg=f"A1 must enable {setting}")

        global_lru_block = re.search(
            r'if\(WII_MEMORY_PROFILE_ID STREQUAL "P5-noaudio-global-lru".*?endif\(\)',
            CMAKE_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(global_lru_block)
        assert global_lru_block is not None
        self.assertNotIn("A1-audio-ps2-world-scan", global_lru_block.group(0))

    def test_profile_enables_existing_pcm_cache(self) -> None:
        self.assertRegex(
            CMAKE_SOURCE,
            re.compile(
                r'if\(WII_MEMORY_PROFILE_ID STREQUAL "A0-audio-cache" OR\s+'
                r'WII_MEMORY_PROFILE_ID STREQUAL "A1-audio-ps2-world-scan"\)\s+'
                r'set\(WII_AUDIO_PCM_CACHE_ENABLE_VALUE 1\)'
            ),
        )


if __name__ == "__main__":
    unittest.main()
