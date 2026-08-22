from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SAMPLE_MANAGER_SOURCE = (REPO_ROOT / "src" / "audio" / "sampman_null.cpp").read_text(
    encoding="utf-8"
)


class WiiPs2OnlySfxTests(unittest.TestCase):
    def test_pc_pcm_backend_is_excluded_from_wii_compilation(self) -> None:
        self.assertRegex(
            SAMPLE_MANAGER_SOURCE,
            re.compile(
                r'#if !defined\(WII\)\s+'
                r'.*?GC_SFX_BACKEND_PC_PCM,.*?'
                r'#endif\s+'
                r'GC_SFX_BACKEND_PS2_VAG,',
                re.DOTALL,
            ),
        )

    def test_wii_initialisation_selects_ps2_without_pc_layout_probe(self) -> None:
        initialise = re.search(
            r'bool8\s+cSampleManager::InitialiseSampleBanks\(void\).*?'
            r'void\s+cSampleManager::SetStreamedFileLoopFlag',
            SAMPLE_MANAGER_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(initialise)
        assert initialise is not None
        self.assertRegex(
            initialise.group(0),
            re.compile(
                r'#if defined\(WII\).*?'
                r'gSfxBackend = GC_SFX_BACKEND_PS2_VAG;.*?'
                r'InitialisePs2SampleBanks\(m_aSamples\).*?'
                r'#else.*?GCM_VFS_FileExistsExact\("audio/sfx.sdt"\)',
                re.DOTALL,
            ),
        )

    def test_pc_bank_io_helpers_are_non_wii_only(self) -> None:
        self.assertRegex(
            SAMPLE_MANAGER_SOURCE,
            re.compile(
                r'#if !defined\(WII\)\s+'
                r'static bool8 OpenSampleBankDataPath.*?'
                r'static bool8 BuildSampleBankOffsets.*?'
                r'#endif',
                re.DOTALL,
            ),
        )
        self.assertRegex(
            SAMPLE_MANAGER_SOURCE,
            re.compile(
                r'cSampleManager::LoadSampleBank\(uint8 nBank\).*?'
                r'if \(gSfxBackend == GC_SFX_BACKEND_PS2_VAG\).*?'
                r'#if !defined\(WII\).*?gSampleDataFile.*?#else\s+return FALSE;\s+#endif',
                re.DOTALL,
            ),
        )

    def test_pc_descriptor_reader_is_non_wii_only(self) -> None:
        self.assertRegex(
            SAMPLE_MANAGER_SOURCE,
            re.compile(
                r'#if !defined\(WII\)\s+'
                r'static uint32 ReadLE32\(FILE \*file\);\s+'
                r'#endif',
            ),
        )
        self.assertRegex(
            SAMPLE_MANAGER_SOURCE,
            re.compile(
                r'#if !defined\(WII\)\s+'
                r'static uint32 ReadLE32\(FILE \*file\).*?'
                r'#endif',
                re.DOTALL,
            ),
        )


if __name__ == "__main__":
    unittest.main()
