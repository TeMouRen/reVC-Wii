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


if __name__ == "__main__":
    unittest.main()
