from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
GX_RASTER_SOURCE = (
    REPO_ROOT / "vendor" / "librw" / "src" / "gx" / "gxraster.cpp"
).read_text(encoding="utf-8")
GX_HEADER_SOURCE = (
    REPO_ROOT / "vendor" / "librw" / "src" / "gx" / "rwgx.h"
).read_text(encoding="utf-8")
MEMORY_SOURCE = (REPO_ROOT / "src" / "rw" / "MemoryMgr.cpp").read_text(
    encoding="utf-8"
)
AUDIO_SOURCE = (REPO_ROOT / "src" / "audio" / "AudioLogic.cpp").read_text(
    encoding="utf-8"
)


class WiiArrestCleanupTests(unittest.TestCase):
    def test_raster_cpu_data_uses_tracked_generic_mem2_storage(self) -> None:
        self.assertIn("GX_RASTER_CPU_GENERIC_MEM2", GX_HEADER_SOURCE)
        lock = GX_RASTER_SOURCE.split("rasterLock(Raster *raster", 1)[1]
        lock = lock.split("rasterUnlock(Raster *raster", 1)[0]
        self.assertIn("natras->cpuData = MemoryMgrMallocMem2Strict", lock)
        self.assertIn(
            "natras->cpuDataSize = (uint32)expectedCpuDataSize", lock
        )
        self.assertIn(
            "natras->cpuDataStorage = GX_RASTER_CPU_GENERIC_MEM2", lock
        )

    def test_raster_destroy_never_passes_cpu_data_to_libc_free(self) -> None:
        destroy = GX_RASTER_SOURCE.split("rasterDestroy(Raster *raster)", 1)[1]
        destroy = destroy.split("rasterLock(Raster *raster", 1)[0]
        self.assertNotIn("free(natras->cpuData)", destroy)
        self.assertIn("WiiMemoryOwnsGenericMem2(natras->cpuData)", destroy)
        self.assertIn("natras->cpuDataSize == expectedCpuDataSize", destroy)
        self.assertIn("MemoryMgrFreeMem2(natras->cpuData)", destroy)
        self.assertIn("[WII-RASTER-FREE] rejected cpuData", destroy)

    def test_mem2_free_checks_header_read_lower_bound(self) -> None:
        free_body = MEMORY_SOURCE.split(
            "WiiMem2FreeFromPool(Mem2Pool *pool, void *ptr)", 1
        )[1].split("WiiMem2UserSize", 1)[0]
        boundary = free_body.index(
            "pool->base + sizeof(Mem2AllocHeader*)"
        )
        header_read = free_body.index("((Mem2AllocHeader**)ptr)[-1]")
        self.assertLess(boundary, header_read)

    def test_mem2_user_size_checks_header_read_lower_bound(self) -> None:
        size_body = MEMORY_SOURCE.split(
            "WiiMem2UserSize(const Mem2Pool *pool, const void *ptr)", 1
        )[1].split("WiiMem2GetArenaStats", 1)[0]
        boundary = size_body.index(
            "pool->base + sizeof(Mem2AllocHeader*)"
        )
        header_read = size_body.index(
            "((Mem2AllocHeader* const*)ptr)[-1]"
        )
        self.assertLess(boundary, header_read)

    def test_disabled_audio_finishes_mission_audio_without_fallback(self) -> None:
        preload = AUDIO_SOURCE.split(
            "cAudioManager::PreloadMissionAudio(uint8 slot, Const char *name)", 1
        )[1].split("cAudioManager::GetMissionAudioLoadingStatus", 1)[0]
        self.assertIn("#if defined(WII) && !WII_AUDIO_DECODE_ENABLE", preload)
        self.assertIn("m_nMissionAudioSampleIndex[slot] = NO_SAMPLE", preload)
        self.assertIn(
            "m_nMissionAudioPlayStatus[slot] = PLAY_STATUS_FINISHED", preload
        )
        self.assertIn("suppressed: decode disabled", preload)


if __name__ == "__main__":
    unittest.main()
