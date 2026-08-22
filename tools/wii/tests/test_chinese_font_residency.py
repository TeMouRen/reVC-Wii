import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
FONT_CPP = REPO_ROOT / "src" / "renderer" / "Font.cpp"
CMAKE = REPO_ROOT / "CMakeLists.txt"


class ChineseFontResidencyTests(unittest.TestCase):
    def test_chinese_atlas_is_marked_as_persistent_ui_after_pool_registration(self):
        source = FONT_CPP.read_text(encoding="utf-8")
        register = source.index("rw::gx::texPoolRegister(ras, natras->gxData")
        mark = source.index("rw::gx::markPersistentUiTexture(ras);", register)
        create = source.index("rw::Texture *tex = rw::Texture::create(ras);", mark)
        self.assertLess(register, mark)
        self.assertLess(mark, create)

    def test_p15_font_txd_upload_uses_persistent_ui_context(self):
        source = FONT_CPP.read_text(encoding="utf-8")
        cmake = CMAKE.read_text(encoding="utf-8")

        self.assertIn(
            "#if defined(WII) && WII_FONT_PERSISTENT_UPLOAD", source
        )
        self.assertIn(
            'pushPersistentUiTextureUploadContext("font-txd")', source
        )
        self.assertIn(
            'popPersistentUiTextureUploadContext("font-txd")', source
        )
        self.assertGreaterEqual(
            source.count("CScopedFontTextureUpload fontUpload;"), 2
        )
        p15 = cmake.index(
            'WII_MEMORY_PROFILE_ID STREQUAL "P15-noaudio-hud-weapon-pin"'
        )
        enabled = cmake.index("set(WII_FONT_PERSISTENT_UPLOAD_VALUE 1)", p15)
        definition = cmake.index(
            "WII_FONT_PERSISTENT_UPLOAD=${WII_FONT_PERSISTENT_UPLOAD_VALUE}"
        )
        self.assertLess(p15, enabled)
        self.assertLess(enabled, definition)

        initialise = source.index("CFont::Initialise(void)")
        initial_scope = source.index("CScopedFontTextureUpload fontUpload;", initialise)
        initial_close = source.index(
            "\n\t} // font texture upload context", initial_scope
        )
        font_pop = source.index("CTxdStore::PopCurrentTxd();", initialise)
        button_load = source.index('LoadButtons("MODELS/X360BTNS.TXD");', font_pop)
        self.assertLess(initial_scope, font_pop)
        self.assertLess(initial_scope, initial_close)
        self.assertLess(initial_close, button_load)


if __name__ == "__main__":
    unittest.main()
