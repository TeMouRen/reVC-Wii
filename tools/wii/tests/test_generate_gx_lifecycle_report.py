from __future__ import annotations

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "generate_gx_lifecycle_report.py"
SPEC = importlib.util.spec_from_file_location("generate_gx_lifecycle_report", SCRIPT)
assert SPEC and SPEC.loader
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class StaticWorldClassificationTests(unittest.TestCase):
    def test_reproduces_zone_and_big_building_generic_override(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            bridge = root / "maps" / "bridge"
            bridge.mkdir(parents=True)
            (root / "map.zon").write_text(
                "zone\n"
                "Main, 3, -10, -10, -10, 0, 10, 10, 2\n"
                "Beach, 3, 0, -10, -10, 10, 10, 10, 1\n"
                "end\n",
                encoding="ascii",
            )
            (bridge / "bridge.ide").write_text(
                "objs\n"
                "100, NEAsbridge, bridge, 1, 100, 0\n"
                "101, LODsbridge, lodbridge, 1, 3000, 0\n"
                "end\n",
                encoding="ascii",
            )
            (bridge / "bridge.ipl").write_text(
                "inst\n"
                "100, NEAsbridge, 0, 5, 0, 0, 1, 1, 1, 0, 0, 0, 1\n"
                "101, LODsbridge, 0, 5, 0, 0, 1, 1, 1, 0, 0, 0, 1\n"
                "end\n",
                encoding="ascii",
            )

            ide = REPORT.parse_ide_metadata(root)
            zones = REPORT.parse_map_zones(root)
            instances = REPORT.parse_ipl_instances(root, zones["zones"], ide["by_id"])

            lod = ide["by_id"][101]
            self.assertTrue(lod["runtime_big_building"])
            self.assertEqual(lod["related_model"], "NEAsbridge")
            self.assertEqual(
                lod["lod_role"], "runtime_big_building_with_near_model"
            )
            self.assertEqual(
                instances["by_model_id"][100]["runtime_levels"], {"BEACH": 1}
            )
            self.assertEqual(
                instances["by_model_id"][101]["spatial_levels"], {"BEACH": 1}
            )
            self.assertEqual(
                instances["by_model_id"][101]["runtime_levels"], {"GENERIC": 1}
            )
            self.assertEqual(instances["errors"], [])

    def test_parses_collision_model_names_and_alignment(self) -> None:
        def col_entry(name: str, payload_size: int) -> bytes:
            body = name.encode("ascii").ljust(24, b"\0") + bytes(payload_size)
            padding = bytes((-len(body)) % 4)
            return b"COLL" + struct.pack("<I", len(body)) + body + padding

        payload = col_entry("bridge_a", 5) + col_entry("bridge_b", 8) + bytes(32)
        names, errors = REPORT.parse_col_model_names(payload, "fixture.col")
        self.assertEqual(names, ["bridge_a", "bridge_b"])
        self.assertEqual(errors, [])

    def test_static_world_scope_distinguishes_shared_and_empty(self) -> None:
        self.assertEqual(REPORT.static_world_scope(set()), "no_world_instance")
        self.assertEqual(REPORT.static_world_scope({"BEACH"}), "beach_only")
        self.assertEqual(
            REPORT.static_world_scope({"BEACH", "MAINLAND"}),
            "cross_level_shared",
        )


if __name__ == "__main__":
    unittest.main()
