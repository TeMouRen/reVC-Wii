from __future__ import annotations

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
STREAMING_HEADER = (REPO_ROOT / "src" / "core" / "Streaming.h").read_text(
    encoding="utf-8"
)
STREAMING_SOURCE = (REPO_ROOT / "src" / "core" / "Streaming.cpp").read_text(
    encoding="utf-8"
)
MAIN_SOURCE = (REPO_ROOT / "src" / "core" / "main.cpp").read_text(
    encoding="utf-8"
)
GAME_SOURCE = (REPO_ROOT / "src" / "core" / "Game.cpp").read_text(
    encoding="utf-8"
)
CMAKE_SOURCE = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
BUILD_SOURCE = (REPO_ROOT / "build.sh").read_text(encoding="utf-8")
CONFIG_SOURCE = (REPO_ROOT / "src" / "core" / "config.h").read_text(
    encoding="utf-8"
)
TXD_SOURCE = (REPO_ROOT / "src" / "rw" / "TxdStore.cpp").read_text(
    encoding="utf-8"
)
RENDERER_SOURCE = (REPO_ROOT / "src" / "renderer" / "Renderer.cpp").read_text(
    encoding="utf-8"
)


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


def function_region(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


class T1StreamingLifecycleTests(unittest.TestCase):
    def test_big_building_fast_path_requires_entity_instance_and_loaded_model(self) -> None:
        scan = function_body(RENDERER_SOURCE, "CRenderer::ScanBigBuildingList(CPtrList &list)")
        guard_start = scan.index("bool needsVisibilitySetup")
        guard_end = scan.index("if(needsVisibilitySetup)", guard_start)
        guard = scan[guard_start:guard_end]
        self.assertIn("ent->m_rwObject == nil", guard)
        self.assertIn("STREAMSTATE_LOADED", guard)
        self.assertIn("if(needsVisibilitySetup)", scan)

    def test_big_building_admission_skips_duplicate_active_requests(self) -> None:
        scan = function_body(RENDERER_SOURCE, "CRenderer::ScanBigBuildingList(CPtrList &list)")
        admission_start = scan.index("case VIS_STREAMME:")
        admission = scan[admission_start:]
        self.assertIn("streamState == STREAMSTATE_LOADED", admission)
        self.assertIn("streamState == STREAMSTATE_READING", admission)
        self.assertIn("streamState == STREAMSTATE_STARTED", admission)
        self.assertIn("ent->m_rwObject == nil", admission)
        self.assertIn("CStreaming::ms_numPriorityRequests < 4", admission)
        self.assertIn("gWiiBigBuildingRequestsThisFrame >= 2 &&", admission)
        self.assertIn("!canPromote", admission)

    def test_loadscene_protection_is_named_and_reaches_resident_txd(self) -> None:
        self.assertIn("STREAMFLAGS_LOADSCENE_PROTECT = 0x20", STREAMING_HEADER)
        self.assertNotIn("STREAMFLAGS_20", STREAMING_HEADER)

        request_model = function_body(
            STREAMING_SOURCE, "CStreaming::RequestModel(int32 id, int32 flags)"
        )
        self.assertIn("const int32 dependencyFlags = flags;", request_model)
        self.assertIn("STREAMFLAGS_LOADSCENE_PROTECT", request_model)
        self.assertIn("RequestTxd(mi->GetTxdSlot(), dependencyFlags);", request_model)
        self.assertLess(
            request_model.index("RequestTxd(mi->GetTxdSlot(), dependencyFlags);"),
            request_model.index("if(ms_aInfoForModel[id].m_loadState == STREAMSTATE_LOADED)"),
        )

    def test_island_handoff_protects_landing_set_before_retiring_old_level(self) -> None:
        handoff = function_body(
            STREAMING_SOURCE, "CStreaming::LoadBigBuildingsWhenNeeded(void)"
        )
        protect_surface = handoff.index("ProtectCurrentSurfaceForLanding();")
        retire_old = handoff.index("RemoveUnusedBigBuildings(CGame::currLevel);")
        self.assertLess(protect_surface, retire_old)
        protect_landing = handoff.index(
            "AddModelsToRequestList(landingPosition, STREAMFLAGS_LOADSCENE_PROTECT);"
        )
        self.assertGreater(protect_landing, retire_old)
        self.assertIn("InstanceLoadedModels(landingPosition);", handoff)
        self.assertIn("ClearLoadSceneProtection();", handoff)

        remove_buildings = function_body(
            STREAMING_SOURCE, "CStreaming::RemoveBuildings(eLevelName level)"
        )
        self.assertIn("CanRemoveEntityDuringIslandHandoff(e)", remove_buildings)

    def test_gx_pressure_retires_a_dependency_closed_txd_with_real_release(self) -> None:
        make_space = function_body(
            STREAMING_SOURCE, "CStreaming::MakeSpaceFor(int32 size)"
        )
        self.assertIn(
            "RetireLeastUsedGxTxd(STREAMFLAGS_LOADSCENE_PROTECT)", make_space
        )
        gx_recovery = make_space[
            make_space.index("#ifdef WII") : make_space.index("#endif")
        ]
        self.assertNotIn(
            "RemoveLeastUsedModel(STREAMFLAGS_LOADSCENE_PROTECT)", gx_recovery
        )

        retirement = function_body(
            STREAMING_SOURCE,
            "CStreaming::RetireLeastUsedGxTxd(uint32 excludeMask)",
        )
        residency_snapshot = function_body(
            STREAMING_SOURCE, "WiiSnapshotTxdGxResidency(void)"
        )
        self.assertIn("texPoolVisitOwnerResidency", residency_snapshot)
        self.assertIn("IsTxdUsedByRequestedModels", retirement)
        self.assertIn("GetTxdSlot()", retirement)
        self.assertIn("txdGuardRefs", retirement)
        self.assertIn("RemoveRefWithoutDelete", retirement)
        self.assertIn("external owner and must keep the TXD resident", retirement)
        self.assertIn("txdRemoved", retirement)
        self.assertIn("hasLoadedModelObject", retirement)
        self.assertIn("refsAreRetirable", retirement)
        self.assertNotIn(
            "while(CTxdStore::GetNumRefs(candidateTxd) > 0)", retirement
        )
        self.assertIn("after.gxUsed < before.gxUsed", retirement)

        convert = function_body(
            STREAMING_SOURCE,
            "CStreaming::ConvertBufferToObject(int8 *buf, int32 streamId)",
        )
        finish = function_body(
            STREAMING_SOURCE,
            "CStreaming::FinishLoadingLargeFile(int8 *buf, int32 streamId)",
        )
        self.assertIn("WiiStreamResourceAttributionScope", convert)
        self.assertIn("WiiStreamResourceAttributionScope", finish)

    def test_transition_does_not_force_compaction_or_reload_target_splash(self) -> None:
        tidy = function_region(
            GAME_SOURCE,
            "CGame::TidyUpMemory(bool moveTextures, bool flushDraw)",
            "void CGame::ProcessTidyUpMemory(void)",
        )
        self.assertNotIn("gxMemCompact", tidy)

        loading_screen = function_body(
            MAIN_SOURCE, "LoadingIslandScreen(const char *levelName)"
        )
        self.assertIn("LoadSplash(nil)", loading_screen)
        self.assertNotIn("LoadSplash(islandSplash", loading_screen)

    def test_lifecycle_audit_is_bounded_and_opt_in(self) -> None:
        self.assertIn(
            'option(WII_STREAM_LIFECYCLE_AUDIT "Emit bounded Wii streaming lifecycle audit events" OFF)',
            CMAKE_SOURCE,
        )
        self.assertIn('WII_STREAM_LIFECYCLE_AUDIT="${WII_STREAM_LIFECYCLE_AUDIT:-OFF}"', BUILD_SOURCE)
        self.assertIn('WII_STREAM_BIG_BUILDING_PROBE="${WII_STREAM_BIG_BUILDING_PROBE:-ON}"', BUILD_SOURCE)
        self.assertIn('-DWII_STREAM_LIFECYCLE_AUDIT="$WII_STREAM_LIFECYCLE_AUDIT"', BUILD_SOURCE)
        self.assertIn("#define WII_STREAM_LIFECYCLE_AUDIT 0", CONFIG_SOURCE)
        for marker in (
            "event=handoff_begin",
            "event=handoff_end",
            "event=gx_scan",
            "event=gx_blocker",
            "event=gx_retire",
            "event=residual",
        ):
            self.assertIn(marker, STREAMING_SOURCE)
        self.assertIn("event=txd_remove_blocked", TXD_SOURCE)
        self.assertIn("gWiiLifecycleAuditGxBlockedEpisode", STREAMING_SOURCE)
        self.assertIn("!gWiiLifecycleAuditGxBlockedEpisode", STREAMING_SOURCE)
        self.assertIn("if(gWiiLifecycleAuditHandoff.active)", STREAMING_SOURCE)
        self.assertIn("for(int rank = 0; rank < 8; rank++)", STREAMING_SOURCE)
        self.assertIn("scan->topCount < 8", STREAMING_SOURCE)
        self.assertIn("gxRetireAttempts < 4", STREAMING_SOURCE)


if __name__ == "__main__":
    unittest.main()
