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


class P16GxHeadroomGuardTests(unittest.TestCase):
    def test_profile_keeps_p15_tuple_and_is_listed_in_build(self) -> None:
        self.assertIn(
            "P16-noaudio-gx-headroom-guard:OFF:OFF:6291456:8388608:0",
            BUILD_SOURCE,
        )
        self.assertIn(
            'WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard"',
            CMAKE_SOURCE,
        )

    def test_profile_inherits_p15_feature_flags_and_exclusively_enables_guard(self) -> None:
        for definition in (
            "WII_STREAM_ATOMIC_BIG_HANDOFF_VALUE",
            "WII_HUD_ACTIVE_WEAPON_RASTER_PIN_VALUE",
            "WII_HUD_USED_WEAPON_RASTER_PERSIST_VALUE",
        ):
            self.assertRegex(
                CMAKE_SOURCE,
                re.compile(
                    rf'WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin" OR\s+'
                    rf'WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard"\)\s+'
                    rf"set\({definition} 1\)"
                ),
            )

        guard_block = re.search(
            r'if\(WII_MEMORY_PROFILE_ID STREQUAL "P16-noaudio-gx-headroom-guard"\)\s+'
            r'set\(WII_STREAM_GX_HEADROOM_GUARD_VALUE 1\)\s+'
            r'else\(\)\s+'
            r'set\(WII_STREAM_GX_HEADROOM_GUARD_VALUE 0\)',
            CMAKE_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(guard_block)
        assert guard_block is not None
        self.assertNotIn("P7-noaudio-ps2-world-scan", guard_block.group(0))
        self.assertNotIn("P15-noaudio-hud-weapon-pin", guard_block.group(0))

    def test_compile_definition_is_exported_and_exclusive(self) -> None:
        self.assertIn(
            "WII_STREAM_GX_HEADROOM_GUARD=${WII_STREAM_GX_HEADROOM_GUARD_VALUE}",
            CMAKE_SOURCE,
        )
        self.assertIn(
            'message(STATUS "  GX headroom guard: ${WII_STREAM_GX_HEADROOM_GUARD_VALUE}")',
            CMAKE_SOURCE,
        )
        self.assertIn("#ifndef WII_STREAM_GX_HEADROOM_GUARD", STREAMING_SOURCE)

    def test_guard_reuses_archive_keep_thresholds(self) -> None:
        self.assertIn(
            "WII_STREAM_GX_HEADROOM_GUARD_FREE_BYTES =",
            STREAMING_SOURCE,
        )
        self.assertIn(
            "WII_STREAM_ARCHIVE_KEEP_GX_FREE_BYTES;",
            STREAMING_SOURCE,
        )
        self.assertIn(
            "WII_STREAM_GX_HEADROOM_GUARD_LARGEST_BYTES =",
            STREAMING_SOURCE,
        )
        self.assertIn(
            "WII_STREAM_ARCHIVE_KEEP_GX_LARGEST_BYTES;",
            STREAMING_SOURCE,
        )
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r'if\(\(pressure & WII_STREAM_PRESSURE_GX\) == 0 &&\s+'
                r'!WiiStreamHasGxAdmissionHeadroom\(snapshot\)\)\s+'
                r'pressure \|= WII_STREAM_PRESSURE_GX_ADMISSION;'
            ),
        )

    def test_make_space_for_recomputes_pressure_via_helper_on_all_paths(self) -> None:
        make_space = re.search(
            r"bool\s+CStreaming::MakeSpaceFor\(int32 size\)\s*\{[\s\S]*?#else",
            STREAMING_SOURCE,
        )
        self.assertIsNotNone(make_space)
        assert make_space is not None
        source = make_space.group(0)
        self.assertGreaterEqual(
            source.count("WiiStreamGetStreamingPressureForSnapshotWithAdmission("),
            4,
        )
        self.assertNotIn(
            "WiiMemoryGetStreamingPressureForSnapshot(&poolBefore)", source
        )
        self.assertNotIn("WiiMemoryGetStreamingPressureForSnapshot(&poolAfter)", source)
        self.assertNotIn("WiiMemoryGetStreamingPressure()", source)
        self.assertIn("uint32 servicePressureBit = WiiStreamPressureServiceBit(poolBit);", source)
        self.assertIn("WiiStreamDiagSetTrimPressure(pressure, servicePressureBit);", source)
        self.assertIn("hard=0x%X admission=0x%X blocked=0x%X", source)

    def test_admission_pressure_blocks_growth_without_starting_hard_retreat(self) -> None:
        self.assertIn(
            "if(admissionPressure == 0 && WiiStreamHasExpansionHeadroom(snapshot)",
            STREAMING_SOURCE,
        )
        self.assertRegex(
            STREAMING_SOURCE,
            re.compile(
                r"if\(hardPressure != 0\)[\s\S]*?"
                r"else\{[\s\S]*?admissionPressure == 0 && "
                r"WiiStreamHasExpansionHeadroom\(snapshot\)"
            ),
        )
        persistent_pressure = re.search(
            r"WiiStreamApplyPersistentPressure\(uint32 pressure\)"
            r"[\s\S]*?^\}",
            STREAMING_SOURCE,
            re.MULTILINE,
        )
        self.assertIsNotNone(persistent_pressure)
        assert persistent_pressure is not None
        self.assertIn(
            "uint32 hardPressure = WiiStreamHardPressureBits(pressure);",
            persistent_pressure.group(0),
        )
        self.assertNotIn("WiiStreamAdmissionPressureBits", persistent_pressure.group(0))


if __name__ == "__main__":
    unittest.main()
