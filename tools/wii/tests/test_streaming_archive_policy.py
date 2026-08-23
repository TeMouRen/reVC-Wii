from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
STREAMING_SOURCE = REPO_ROOT / "src" / "core" / "Streaming.cpp"


class StreamingArchivePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = STREAMING_SOURCE.read_text(encoding="utf-8")

    def test_transition_reserve_uses_small_archive_guard(self) -> None:
        self.assertRegex(
            self.source,
            re.compile(
                r"size_t effectiveRequest = requested \+ transitionReserve;\s+"
                r"(?:\/\/[^\n]*\n\s*)*"
                r"size_t archiveTransitionGuard = transitionReserve == 0 \? 0 :\s+"
                r"Min\(transitionReserve,\s+"
                r"WII_STREAM_ARCHIVE_TRANSITION_GUARD\);\s+"
                r"size_t archiveRequest = requested \+ archiveTransitionGuard;\s+"
                r"size_t archiveTarget = archiveRequest < archiveCeiling \?\s+"
                r"archiveCeiling - archiveRequest : 0;"
            ),
        )
        self.assertNotIn("archiveRequest >= archiveCeiling", self.source)

    def test_transition_reserve_still_protects_real_pool_pressure(self) -> None:
        self.assertIn(
            "activePressure, poolBefore, effectiveRequest);",
            self.source,
        )
        self.assertIn(
            "size_t softTarget = effectiveRequest < ms_memoryAvailable ?",
            self.source,
        )

    def test_trim_telemetry_reports_request_reserve_and_guard(self) -> None:
        self.assertIn(
            "archive=%uKB request=%uKB reserve=%uKB guard=%uKB",
            self.source,
        )
        self.assertIn("(unsigned)(requested / 1024u)", self.source)
        self.assertIn("(unsigned)(transitionReserve / 1024u)", self.source)
        self.assertIn("(unsigned)(archiveTransitionGuard / 1024u)", self.source)

    def test_archive_retreat_requires_sustained_real_pressure(self) -> None:
        self.assertIn(
            "WII_STREAMING_ARCHIVE_HARD_PRESSURE_MS = 2000u",
            self.source,
        )
        self.assertIn(
            "hardPressureAgeMs >= WII_STREAMING_ARCHIVE_HARD_PRESSURE_MS",
            self.source,
        )
        self.assertIn(
            "WII_STREAMING_ARCHIVE_PRESSURE_SAMPLE_GAP_MS = 250u",
            self.source,
        )
        self.assertIn(
            "now - gWiiStreamArchivePressureLastSeenMs >",
            self.source,
        )
        self.assertNotIn("WII_STREAMING_ARCHIVE_SOFT_RETREAT_MS", self.source)
        self.assertNotIn('reason = "soft"', self.source)

    def test_archive_retreat_is_one_step_and_can_recover(self) -> None:
        self.assertNotIn("WII_STREAMING_ARCHIVE_CEILING_STEP * 2u", self.source)
        self.assertIn(
            "WII_STREAMING_ARCHIVE_RECOVERY_MS = 5000u",
            self.source,
        )
        self.assertIn(
            "gWiiStreamArchiveRetreated &&\n"
            "\t\t                        retentionHeadroom",
            self.source,
        )
        self.assertIn('reason = "recover"', self.source)
        self.assertIn("recovery_age=%ums retreated=%d", self.source)

    def test_archive_size_debt_is_not_an_eviction_driver(self) -> None:
        self.assertIn(
            "WII_STREAM_ARCHIVE_MAX_ELASTIC_DEBT_BYTES = 2u * 1024u * 1024u",
            self.source,
        )
        self.assertIn(
            "WiiStreamDiagBeginTrim(initialPressure, archiveTarget);",
            self.source,
        )
        self.assertIn(
            "static uint32 gWiiStreamDependencyUnwindFrame = UINT32_MAX;",
            self.source,
        )
        self.assertNotIn("size_t archiveAllowance =", self.source)
        self.assertNotIn("size_t archiveLimit =", self.source)
        self.assertNotIn("ms_memoryUsed > archiveLimit", self.source)
        pressure_loop = self.source.index(
            "while(pressureRemovals < WII_STREAM_PRESSURE_MAX_REMOVALS)"
        )
        dependency_fallback = self.source.index(
            "uint32 dependencyPressure =",
            pressure_loop,
        )
        self.assertGreater(dependency_fallback, pressure_loop)
        self.assertRegex(
            self.source,
            re.compile(
                r"uint32 dependencyPressure =\s*"
                r"WiiStreamHardPressureBits\(pressure & blockedPressure\);.*?"
                r"if\(\(pressure & blockedPressure & WII_STREAM_PRESSURE_GX_ADMISSION\) != 0\)\s*"
                r"dependencyPressure \|= WII_STREAM_PRESSURE_GX;.*?"
                r"if\(dependencyUnwindAvailable && dependencyPressure != 0\)\{.*?"
                r"uint32 dependencyPoolBit = WiiStreamSelectPressureBit\(\s*"
                r"dependencyPressure,\s*"
                r"poolBefore, effectiveRequest\);.*?"
                r"dependencyPoolBit = WiiStreamPressureServiceBit\(dependencyPoolBit\);.*?"
                r"WiiStreamRemoveLeastUsedForPool\(\s*"
                r"STREAMFLAGS_20, dependencyPoolBit, &removedId, &selectionDeferred\);.*?"
                r"archiveRemovals\+\+;",
                re.DOTALL,
            ),
        )
        self.assertNotIn("WII_STREAM_ARCHIVE_MAX_REMOVALS_PER_FRAME", self.source)
        self.assertNotIn("WII_STREAM_P7_RETIRE_ARCHIVE_MAX_REMOVALS_PER_FRAME", self.source)
        self.assertIn("while(pressureRemovals < WII_STREAM_PRESSURE_MAX_REMOVALS)", self.source)

    def test_normal_world_path_retires_only_on_pool_headroom(self) -> None:
        self.assertRegex(
            self.source,
            re.compile(
                r"DeleteFarAwayRwObjects\(TheCamera\.GetPosition\(\)\);\s+"
                r"#ifdef WII\s+"
                r"wiiAfterPrepTicks = gettime\(\);\s+"
                r"if\(!ms_disableStreaming\)\{[\s\S]{0,420}"
                r"WiiStreamRetireOneHeadroomResource\(\s+"
                r"headroomSnapshot, headroomPressure, nil\);"
            ),
        )
        self.assertIn(
            "WiiStreamArchiveRetirePool(const WiiMemoryPoolSnapshot &snapshot)",
            self.source,
        )

    def test_headroom_retire_services_gx_pressure_without_cross_pool_eviction(self) -> None:
        helper = self.source.index(
            "WiiStreamRetireOneHeadroomResource(const WiiMemoryPoolSnapshot &snapshot,"
        )
        helper_end = self.source.index("\nbool\nCStreaming::MakeSpaceFor", helper)
        helper_source = self.source[helper:helper_end]
        self.assertIn("uint32 hardPressure = WiiStreamHardPressureBits(pressure);", helper_source)
        self.assertIn("uint32 gxAdmissionPressure", helper_source)
        self.assertIn(
            "(hardPressure & WII_STREAM_PRESSURE_GX) == 0 &&",
            helper_source,
        )
        self.assertIn("gxAdmissionPressure != 0", helper_source)
        self.assertIn("WII_STREAM_PRESSURE_GX :", helper_source)
        self.assertNotIn("CStreaming::ms_memoryUsed", helper_source)
        adaptive_branch = self.source.index(
            "#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING",
            self.source.index("CStreaming::MakeSpaceFor"),
        )
        adaptive_source = self.source[adaptive_branch:]
        self.assertIn("ms_memoryUsed > archiveCeiling + retentionAllowance", adaptive_source)

    def test_dependency_unwind_normalizes_gx_admission_pressure(self) -> None:
        unwind = self.source.index(
            "// If the owning pool is still under hard pressure after targeted removal"
        )
        unwind_end = self.source.index(
            "#if WII_STREAM_ADAPTIVE_ARCHIVE_CEILING", unwind
        )
        unwind_source = self.source[unwind:unwind_end]
        self.assertIn("uint32 dependencyPressure", unwind_source)
        self.assertIn("WII_STREAM_PRESSURE_GX_ADMISSION", unwind_source)
        self.assertIn("dependencyPressure |= WII_STREAM_PRESSURE_GX", unwind_source)

    def test_headroom_retire_marks_frame_after_success(self) -> None:
        helper = self.source.index(
            "WiiStreamRetireOneHeadroomResource(const WiiMemoryPoolSnapshot &snapshot,"
        )
        failure = self.source.index("if(!didRemove){", helper)
        progress = self.source.index("WiiStreamPoolMadeProgress(", failure)
        success_mark = self.source.index(
            "gWiiStreamArchiveRetireFrame = frame;", failure
        )
        self.assertLess(progress, success_mark)
        self.assertLess(failure, success_mark)

    def test_ineffective_targeted_retirement_allows_dependency_unwind(self) -> None:
        pressure_loop = self.source.index(
            "while(pressureRemovals < WII_STREAM_PRESSURE_MAX_REMOVALS)"
        )
        pressure_loop_end = self.source.index(
            "// If the owning pool is still under hard pressure", pressure_loop
        )
        pressure_source = self.source[pressure_loop:pressure_loop_end]
        self.assertIn("blockedPressure |= poolBit;", pressure_source)
        self.assertNotIn("WII_STREAM_PRESSURE_MAX_NO_PROGRESS", self.source)

        dependency = self.source.index(
            "if(dependencyUnwindAvailable && dependencyPressure != 0", pressure_loop_end
        )
        self.assertNotIn("!ineffectiveGxTargetedRemoval", self.source[dependency:dependency + 180])

        persistent = self.source.index(
            "if(WiiStreamApplyPersistentPressure(pressure) &&", dependency
        )
        self.assertIn(
            "(WiiStreamHardPressureBits(pressure) & WII_STREAM_PRESSURE_GX) == 0",
            self.source[persistent:persistent + 180],
        )

    def test_dependency_unwind_never_falls_back_to_global_lru(self) -> None:
        dependency_fallback = self.source.index(
            "uint32 dependencyPoolBit = WiiStreamSelectPressureBit("
        )
        persistent_pressure = self.source.index(
            "if(WiiStreamApplyPersistentPressure(pressure) &&",
            dependency_fallback,
        )
        fallback_source = self.source[dependency_fallback:persistent_pressure]
        self.assertIn(
            "dependencyPressure,",
            fallback_source,
        )
        self.assertIn(
            "dependencyPoolBit = WiiStreamPressureServiceBit(dependencyPoolBit);",
            fallback_source,
        )
        self.assertIn(
            "STREAMFLAGS_20, dependencyPoolBit, &removedId, &selectionDeferred",
            fallback_source,
        )
        self.assertNotRegex(
            fallback_source,
            re.compile(r"WiiStreamRemoveLeastUsedForPool\(\s*STREAMFLAGS_20,\s*0,"),
        )

    def test_gx_txd_retires_loaded_dependencies_before_the_dictionary(self) -> None:
        helper = self.source.index("WiiStreamFindTxdDependencyVictim")
        selector = self.source.index("WiiStreamRemoveLeastUsedForPool")
        self.assertLess(helper, selector)
        self.assertIn("info->m_next == nil", self.source[helper:selector])
        self.assertIn("mi->GetNumRefs() != 0", self.source[helper:selector])
        self.assertIn("WiiStreamProtectsSameFrameWorldLoad(modelId)", self.source[helper:selector])
        self.assertRegex(
            self.source,
            re.compile(
                r"if\(poolBit == WII_STREAM_PRESSURE_GX &&\s+"
                r"streamId >= STREAM_OFFSET_TXD && streamId < STREAM_OFFSET_COL\).*?"
                r"WiiStreamFindTxdDependencyVictim\(",
                re.DOTALL,
            ),
        )

    def test_gx_model_victims_require_measured_resident_bytes(self) -> None:
        residency = self.source.index("WiiStreamHasRetireablePoolResidency")
        selector = self.source.index("WiiStreamRemoveLeastUsedForPool")
        self.assertLess(residency, selector)
        helper = self.source[residency:selector]
        self.assertIn("poolBit != WII_STREAM_PRESSURE_GX", helper)
        self.assertIn("gWiiStreamResidentCost[streamId].gxKiB", helper)
        self.assertIn("WII_STREAM_RESIDENT_UNKNOWN_KIB", helper)
        self.assertRegex(
            self.source,
            re.compile(
                r"if\(!WiiStreamResourceMatchesPool\(streamId, poolBit\)\)\s+"
                r"continue;\s+"
                r"if\(!WiiStreamHasRetireablePoolResidency\(streamId, poolBit\)\)\s+"
                r"continue;",
            ),
        )

    def test_gx_txd_alias_pins_are_not_selected_as_victims(self) -> None:
        self.assertIn(
            "!CTxdStore::IsTxdAliasPinned(streamId - STREAM_OFFSET_TXD)",
            self.source,
        )

    def test_dependency_unwind_is_reported_separately_from_archive_cap(self) -> None:
        self.assertIn("WII_STREAM_TRIM_DEPENDENCY_UNWIND", self.source)
        self.assertIn('return "dependency-unwind";', self.source)
        self.assertNotIn("WII_STREAM_TRIM_ARCHIVE_CAP", self.source)

    def test_archive_elastic_allowance_tracks_tightest_pool_surplus(self) -> None:
        self.assertRegex(
            self.source,
            re.compile(
                r"static size_t\s+"
                r"WiiStreamArchiveElasticAllowance\(\s*"
                r"const WiiMemoryPoolSnapshot &snapshot, uint32 pressure\).*?"
                r"snapshot\.genericFree <= WII_STREAM_ARCHIVE_KEEP_GENERIC_FREE_BYTES.*?"
                r"snapshot\.genericLargest <= WII_STREAM_ARCHIVE_KEEP_GENERIC_LARGEST_BYTES.*?"
                r"newlibRawFree <= WII_STREAM_ARCHIVE_KEEP_NEWLIB_RAW_BYTES.*?"
                r"snapshot\.gxFree <= WII_STREAM_ARCHIVE_KEEP_GX_FREE_BYTES.*?"
                r"snapshot\.gxLargest <= WII_STREAM_ARCHIVE_KEEP_GX_LARGEST_BYTES.*?"
                r"size_t allowance = WII_STREAM_ARCHIVE_MAX_ELASTIC_DEBT_BYTES;.*?"
                r"allowance = Min\(allowance, snapshot\.genericFree -.*?"
                r"allowance = Min\(allowance, snapshot\.genericLargest -.*?"
                r"allowance = Min\(allowance, newlibRawFree -.*?"
                r"allowance = Min\(allowance, snapshot\.gxFree -.*?"
                r"allowance = Min\(allowance, snapshot\.gxLargest -",
                re.DOTALL,
            ),
        )

    def test_archive_ceiling_still_tracks_real_pool_headroom(self) -> None:
        self.assertRegex(
            self.source,
            re.compile(
                r"static bool\s+"
                r"WiiStreamHasArchiveGrowthHeadroom\(\s*"
                r"const WiiMemoryPoolSnapshot &snapshot, uint32 pressure\).*?"
                r"snapshot\.genericFree >= WII_STREAM_ARCHIVE_GROW_GENERIC_FREE_BYTES.*?"
                r"snapshot\.genericLargest >= WII_STREAM_ARCHIVE_GROW_GENERIC_LARGEST_BYTES.*?"
                r"newlibRawFree >= WII_STREAM_ARCHIVE_GROW_NEWLIB_RAW_BYTES.*?"
                r"snapshot\.gxFree >= WII_STREAM_ARCHIVE_GROW_GX_FREE_BYTES.*?"
                r"snapshot\.gxLargest >= WII_STREAM_ARCHIVE_GROW_GX_LARGEST_BYTES;",
                re.DOTALL,
            ),
        )
        self.assertIn("WiiStreamHasArchiveGrowthHeadroom", self.source)

    def test_visual_handoff_keeps_proxy_until_visible_big_buildings_are_ready(self) -> None:
        self.assertIn("WiiIslandRequestVisibleBigBuildings", self.source)
        self.assertIn("WiiIslandRefreshVisualHandoff", self.source)
        self.assertIn(
            "CRenderer::ShouldModelBeStreamed(building, position)",
            self.source,
        )
        self.assertIn("gWiiIslandVisualHandoffPending = true", self.source)
        self.assertIn("WiiIslandRefreshVisualHandoff(playerPosition);", self.source)
        self.assertIn(
            "now - gWiiIslandLastRequestAtMs < WII_ISLAND_PREFETCH_INTERVAL_MS",
            self.source,
        )
        self.assertIn(
            "gWiiIslandRetireComplete && !gWiiIslandVisualHandoffPending",
            self.source,
        )
        self.assertIn("[WII-ISLAND] visual handoff", self.source)
        self.assertRegex(
            self.source,
            re.compile(
                r"if\(gWiiIslandVisualHandoffPending &&\s+"
                r"\(\(gWiiIslandTargetLevel == LEVEL_MAINLAND"
            ),
        )


if __name__ == "__main__":
    unittest.main()
