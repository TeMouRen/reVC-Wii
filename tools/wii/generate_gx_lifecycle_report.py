#!/usr/bin/env python3
"""Generate a read-only GX/TXD ownership report for a GTA IMG archive.

The report deliberately separates offline asset facts from runtime log facts. It
does not modify an archive, a deployed DOL, or the game source/runtime.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


OFFLINE_TOOLS = Path(r"F:\Wii Work\PS2 img Workplace\tools")
if str(OFFLINE_TOOLS) not in sys.path:
    sys.path.insert(0, str(OFFLINE_TOOLS))

from asset_contract import AssetContractError, load_model_txd_map  # noqa: E402
from img_workbench import open_archive, read_entry  # noqa: E402
from rw_dff import RwParseError, extract_dff_texture_references  # noqa: E402
from scan_gx_archive import inspect_txd  # noqa: E402


DEFAULT_ARCHIVE = Path(
    r"F:\Wii Work\提取测试\reVC-v2-archive-retention-test\files\models\gta3.img"
)
DEFAULT_INVENTORY = Path(
    r"F:\Wii Work\PS2 img Workplace\output\reports\gx-v2-phase-c-merged-20260817\v2-archive-inventory.json"
)
DEFAULT_DATA_ROOT = Path(
    r"F:\Wii Work\提取测试\reVC-v2-archive-retention-test\files\data"
)
DEFAULT_LOG = Path(
    r"C:\Users\20493\AppData\Roaming\Dolphin Emulator\Logs\dolphin-REVC02-20260823-185206-992.log"
)
DEFAULT_OUTPUT = Path(
    r"F:\Wii Work\PS2 img Workplace\output\reports\wii-gx-lifecycle-audit-20260823"
)

GLOBAL_TXD_NAMES = {
    "fonts.txd",
    "fronten1.txd",
    "fronten2.txd",
    "generic.txd",
    "hud.txd",
    "particle.txd",
}

LEVEL_NAMES = {0: "GENERIC", 1: "BEACH", 2: "MAINLAND"}
WORLD_MODEL_SECTIONS = {"objs", "tobj"}
LOD_DISTANCE = 300.0


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def bytes_to_mib(value: int) -> float:
    return round(value / (1024 * 1024), 3)


def txd_key(name: str) -> str:
    normalized = Path(name).name.casefold()
    return normalized if normalized.endswith(".txd") else f"{normalized}.txd"


def load_inventory(path: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    inventory = payload.get("inventory")
    if not isinstance(inventory, list):
        raise ValueError(f"inventory list missing in {path}")
    return payload, {str(row["source"]).casefold(): row for row in inventory}


def inventory_summary(inventory_rows: list[dict[str, Any]]) -> dict[str, Any]:
    formats: Counter[str] = Counter()
    format_bytes: Counter[str] = Counter()
    alpha_kinds: Counter[str] = Counter()
    texture_count = 0
    for row in inventory_rows:
        for texture in row.get("textures", []):
            texture_count += 1
            fmt = str(texture.get("format") or "UNKNOWN")
            formats[fmt] += 1
            format_bytes[fmt] += int(texture.get("resident_bytes") or 0)
            alpha_kinds[str(texture.get("alpha_kind") or "unknown")] += 1
    return {
        "txd_count": len(inventory_rows),
        "texture_count": texture_count,
        "formats": dict(sorted(formats.items())),
        "resident_bytes_by_format": dict(
            sorted((key, value) for key, value in format_bytes.items())
        ),
        "resident_bytes_total": sum(format_bytes.values()),
        "alpha_kinds": dict(sorted(alpha_kinds.items())),
    }


def source_package(path: Path, data_root: Path) -> str:
    relative = path.resolve().relative_to(data_root.resolve())
    if relative.parent == Path(".") or relative.parent.name.casefold() == "maps":
        return path.stem.casefold()
    return relative.parent.name.casefold()


def parse_ide_metadata(data_root: Path) -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    errors: list[str] = []
    for path in sorted(data_root.rglob("*.ide"), key=lambda item: str(item).casefold()):
        section = ""
        for line_number, raw_line in enumerate(
            path.read_text(encoding="ascii").splitlines(), start=1
        ):
            line = raw_line.partition("#")[0].strip()
            if not line:
                continue
            lowered = line.casefold()
            if lowered == "end":
                section = ""
                continue
            if not section and "," not in line:
                section = lowered
                continue
            if section not in {"objs", "tobj", "weap", "hier", "cars", "peds"}:
                continue
            try:
                fields = [field.strip() for field in next(csv.reader([line]))]
                model_id = int(fields[0], 0)
                model = fields[1]
                txd = fields[2]
                first_lod_distance = None
                lod_distances: list[float] = []
                flags = None
                if section in WORLD_MODEL_SECTIONS:
                    atomic_count = int(fields[3], 0)
                    lod_distances = [float(value) for value in fields[4 : 4 + atomic_count]]
                    if len(lod_distances) != atomic_count:
                        raise ValueError("missing LOD distance")
                    first_lod_distance = lod_distances[0]
                    flags = int(fields[4 + atomic_count], 0)
                records.append(
                    {
                        "id": model_id,
                        "model": model,
                        "txd": txd,
                        "section": section,
                        "source": str(path),
                        "relative_source": path.resolve()
                        .relative_to(data_root.resolve())
                        .as_posix(),
                        "source_package": source_package(path, data_root),
                        "line": line_number,
                        "lod_distances": lod_distances,
                        "first_lod_distance": first_lod_distance,
                        "flags": flags,
                        "ignore_draw_distance": bool(flags is not None and flags & 0x100),
                        "runtime_big_building": bool(
                            first_lod_distance is not None
                            and first_lod_distance > LOD_DISTANCE
                        ),
                        "related_model": None,
                        "related_by": [],
                        "lod_role": "non_world_model",
                    }
                )
            except (IndexError, ValueError) as exc:
                errors.append(f"{path}:{line_number}: {exc}")

    by_source: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        by_source[record["source"]].append(record)
    for source_records in by_source.values():
        simple_records = sorted(
            (row for row in source_records if row["section"] in WORLD_MODEL_SECTIONS),
            key=lambda row: int(row["id"]),
        )
        for record in simple_records:
            if not record["runtime_big_building"]:
                continue
            suffix = str(record["model"])[3:].casefold()
            related = next(
                (
                    candidate
                    for candidate in simple_records
                    if candidate is not record
                    and str(candidate["model"])[3:].casefold() == suffix
                ),
                None,
            )
            if related:
                record["related_model"] = related["model"]
                related["related_by"].append(record["model"])

    for record in records:
        if record["runtime_big_building"]:
            record["lod_role"] = (
                "runtime_big_building_with_near_model"
                if record["related_model"]
                else "runtime_big_building_without_near_model"
            )
        elif record["related_by"]:
            record["lod_role"] = "near_model_related_to_big_building"
        elif record["section"] in WORLD_MODEL_SECTIONS:
            record["lod_role"] = "ordinary_world_model"

    return {
        "records": records,
        "by_id": {int(row["id"]): row for row in records},
        "by_name": {str(row["model"]).casefold(): row for row in records},
        "errors": errors,
    }


def parse_map_zones(data_root: Path) -> dict[str, Any]:
    path = data_root / "map.zon"
    zones: list[dict[str, Any]] = []
    errors: list[str] = []
    section = ""
    for line_number, raw_line in enumerate(
        path.read_text(encoding="ascii").splitlines(), start=1
    ):
        line = raw_line.partition("#")[0].strip()
        if not line:
            continue
        lowered = line.casefold()
        if lowered == "end":
            section = ""
            continue
        if not section and "," not in line:
            section = lowered
            continue
        if section != "zone":
            continue
        try:
            fields = [field.strip() for field in next(csv.reader([line]))]
            level_id = int(fields[8], 0)
            zones.append(
                {
                    "name": fields[0],
                    "type": int(fields[1], 0),
                    "minimum": [float(value) for value in fields[2:5]],
                    "maximum": [float(value) for value in fields[5:8]],
                    "level_id": level_id,
                    "level": LEVEL_NAMES.get(level_id, f"UNKNOWN_{level_id}"),
                    "line": line_number,
                }
            )
        except (IndexError, ValueError) as exc:
            errors.append(f"{path}:{line_number}: {exc}")
    return {"path": str(path), "zones": zones, "errors": errors}


def level_from_position(zones: list[dict[str, Any]], position: tuple[float, float, float]) -> str:
    x, y, z = position
    for zone in zones:
        minimum = zone["minimum"]
        maximum = zone["maximum"]
        if (
            minimum[0] <= x <= maximum[0]
            and minimum[1] <= y <= maximum[1]
            and minimum[2] <= z <= maximum[2]
        ):
            return str(zone["level"])
    return "GENERIC"


def static_world_scope(levels: set[str]) -> str:
    if not levels:
        return "no_world_instance"
    if levels == {"GENERIC"}:
        return "generic_only"
    if levels == {"BEACH"}:
        return "beach_only"
    if levels == {"MAINLAND"}:
        return "mainland_only"
    return "cross_level_shared"


def parse_ipl_instances(
    data_root: Path, zones: list[dict[str, Any]], model_info_by_id: dict[int, dict[str, Any]]
) -> dict[str, Any]:
    aggregates: dict[int, dict[str, Any]] = {}
    errors: list[str] = []
    total_instances = 0
    model_info_misses: Counter[str] = Counter()
    id_name_mismatches = 0
    for path in sorted(data_root.rglob("*.ipl"), key=lambda item: str(item).casefold()):
        section = ""
        for line_number, raw_line in enumerate(
            path.read_text(encoding="ascii").splitlines(), start=1
        ):
            line = raw_line.partition("#")[0].strip()
            if not line:
                continue
            lowered = line.casefold()
            if lowered == "end":
                section = ""
                continue
            if not section and "," not in line:
                section = lowered
                continue
            if section != "inst":
                continue
            try:
                fields = [field.strip() for field in next(csv.reader([line]))]
                model_id = int(fields[0], 0)
                model_name = fields[1]
                if len(fields) == 13:
                    area = float(fields[2])
                    position = tuple(float(value) for value in fields[3:6])
                elif len(fields) == 12:
                    area = 0.0
                    position = tuple(float(value) for value in fields[2:5])
                else:
                    raise ValueError(f"unexpected IPL inst field count {len(fields)}")
                spatial_level = level_from_position(zones, position)
                model_info = model_info_by_id.get(model_id)
                runtime_level = spatial_level
                if model_info is None:
                    model_info_misses[f"{model_id}:{model_name}"] += 1
                else:
                    if str(model_info["model"]).casefold() != model_name.casefold():
                        id_name_mismatches += 1
                    if model_info["runtime_big_building"] and (
                        float(model_info["first_lod_distance"] or 0.0) > 2500.0
                        or model_info["ignore_draw_distance"]
                    ):
                        runtime_level = "GENERIC"
                aggregate = aggregates.setdefault(
                    model_id,
                    {
                        "model": model_name,
                        "instance_count": 0,
                        "spatial_levels": Counter(),
                        "runtime_levels": Counter(),
                        "areas": Counter(),
                        "sources": Counter(),
                    },
                )
                aggregate["instance_count"] += 1
                aggregate["spatial_levels"][spatial_level] += 1
                aggregate["runtime_levels"][runtime_level] += 1
                aggregate["areas"][str(int(area) if area.is_integer() else area)] += 1
                aggregate["sources"][
                    path.resolve().relative_to(data_root.resolve()).as_posix()
                ] += 1
                total_instances += 1
            except (IndexError, ValueError) as exc:
                errors.append(f"{path}:{line_number}: {exc}")

    serializable: dict[int, dict[str, Any]] = {}
    for model_id, row in aggregates.items():
        serializable[model_id] = {
            "model": row["model"],
            "instance_count": row["instance_count"],
            "spatial_levels": dict(sorted(row["spatial_levels"].items())),
            "runtime_levels": dict(sorted(row["runtime_levels"].items())),
            "areas": dict(sorted(row["areas"].items())),
            "sources": dict(sorted(row["sources"].items())),
        }
    return {
        "by_model_id": serializable,
        "total_instances": total_instances,
        "model_info_miss_count": sum(model_info_misses.values()),
        "model_info_misses": dict(sorted(model_info_misses.items())),
        "id_name_mismatch_count": id_name_mismatches,
        "errors": errors,
    }


def parse_col_model_names(payload: bytes, source: str) -> tuple[list[str], list[str]]:
    names: list[str] = []
    errors: list[str] = []
    offset = 0
    while offset + 8 <= len(payload):
        if not any(payload[offset:]):
            break
        if payload[offset : offset + 4] != b"COLL":
            next_offset = payload.find(b"COLL", offset + 1)
            if next_offset < 0:
                errors.append(f"{source}: no COLL header at offset {offset}")
                break
            errors.append(f"{source}: skipped {next_offset - offset} bytes before COLL header")
            offset = next_offset
        size = int.from_bytes(payload[offset + 4 : offset + 8], "little")
        if size < 24 or offset + 8 + size > len(payload):
            errors.append(f"{source}: invalid COLL size {size} at offset {offset}")
            break
        raw_name = payload[offset + 8 : offset + 32].split(b"\0", 1)[0]
        names.append(raw_name.decode("ascii", errors="replace"))
        offset += 8 + ((size + 3) & ~3)
    return names, errors


def build_dependency_report(
    entries: list[Any], image_handle: Any, inventory_by_txd: dict[str, Any], data_root: Path
) -> tuple[dict[str, Any], dict[str, Any]]:
    try:
        bindings = load_model_txd_map(data_root)
        binding_error = None
    except AssetContractError as exc:
        bindings = {}
        binding_error = str(exc)

    model_rows: list[dict[str, Any]] = []
    dff_parse_errors: list[dict[str, str]] = []
    unmapped_dffs: list[str] = []
    models_by_txd: dict[str, list[str]] = defaultdict(list)
    inferred_models_by_txd: dict[str, list[str]] = defaultdict(list)
    dff_bytes_by_txd: Counter[str] = Counter()
    inferred_dff_bytes_by_txd: Counter[str] = Counter()
    refs_by_txd: dict[str, set[tuple[str, str]]] = defaultdict(set)
    inferred_refs_by_txd: dict[str, set[tuple[str, str]]] = defaultdict(set)
    unresolved_refs_by_txd: dict[str, set[str]] = defaultdict(set)
    txd_entries_by_key = {
        entry.name.casefold(): entry for entry in entries if entry.extension == ".txd"
    }
    known_textures_by_txd = {
        key: {
            (
                str(item.get("name") or "").casefold(),
                str(item.get("mask") or "").casefold(),
            )
            for item in inventory.get("textures", [])
        }
        for key, inventory in inventory_by_txd.items()
    }
    txd_display_by_key = {
        entry.name.casefold(): entry.name for entry in entries if entry.extension == ".txd"
    }
    exact_texture_owners: dict[tuple[str, str], set[str]] = defaultdict(set)
    named_texture_owners: dict[str, set[str]] = defaultdict(set)
    for owner, texture_keys in known_textures_by_txd.items():
        for name, mask in texture_keys:
            exact_texture_owners[(name, mask)].add(owner)
            named_texture_owners[name].add(owner)
    unresolved_reference_rows: list[dict[str, Any]] = []
    cross_txd_consumers: dict[str, set[str]] = defaultdict(set)

    def annotate_reference_resolution(
        references: list[dict[str, Any]], declared_key: str | None, model: str
    ) -> None:
        declared_known = known_textures_by_txd.get(declared_key or "", set())
        for reference in references:
            name = str(reference["name"]).casefold()
            mask = str(reference.get("mask") or "").casefold()
            key = (name, mask)
            if key in declared_known:
                reference["resolution"] = "declared_txd"
                reference["archive_candidate_txds"] = []
                continue
            exact_candidates = sorted(
                exact_texture_owners.get(key, set()) - ({declared_key} if declared_key else set())
            )
            name_candidates = sorted(
                named_texture_owners.get(name, set()) - ({declared_key} if declared_key else set())
            )
            if exact_candidates:
                candidates = exact_candidates
                resolution = (
                    "unique_exact_cross_txd_candidate"
                    if len(candidates) == 1
                    else "ambiguous_exact_cross_txd_candidates"
                )
            elif name_candidates:
                candidates = name_candidates
                resolution = (
                    "unique_name_only_cross_txd_candidate"
                    if len(candidates) == 1
                    else "ambiguous_name_only_cross_txd_candidates"
                )
            else:
                candidates = []
                resolution = "missing_from_archive_inventory"
            display_candidates = [
                txd_display_by_key.get(candidate, candidate) for candidate in candidates
            ]
            reference["resolution"] = resolution
            reference["archive_candidate_txds"] = display_candidates
            unresolved_reference_rows.append(
                {
                    "model": model,
                    "declared_or_inferred_txd": (
                        txd_display_by_key.get(declared_key, declared_key)
                        if declared_key
                        else None
                    ),
                    "texture": reference["name"],
                    "mask": reference.get("mask") or "",
                    "resolution": resolution,
                    "archive_candidate_txds": display_candidates,
                }
            )
            consumer = f"{model}:{reference['name']}:{reference.get('mask') or ''}"
            for candidate in candidates:
                cross_txd_consumers[candidate].add(consumer)

    for entry in entries:
        if entry.extension != ".dff":
            continue
        model = Path(entry.name).stem
        binding = bindings.get(model.casefold())
        refs: list[dict[str, str]] = []
        parse_error = None
        try:
            parsed = extract_dff_texture_references(read_entry(image_handle, entry), entry.name)
            for ref in parsed:
                refs.append({"name": ref.name, "mask": ref.mask})
        except RwParseError as exc:
            parse_error = str(exc)
            dff_parse_errors.append({"dff": entry.name, "error": parse_error})

        inferred_txd = None
        inference = None
        if binding is None:
            unmapped_dffs.append(entry.name)
            candidate_key = f"{model}.txd".casefold()
            candidate_entry = txd_entries_by_key.get(candidate_key)
            if candidate_entry is not None:
                inferred_txd = candidate_entry.name
                annotate_reference_resolution(refs, candidate_key, model)
                known = known_textures_by_txd.get(candidate_key, set())
                ref_keys = {
                    (str(ref["name"]).casefold(), str(ref.get("mask") or "").casefold())
                    for ref in refs
                }
                missing = sorted(
                    f"{ref[0]}" + (f" (mask={ref[1]})" if ref[1] else "")
                    for ref in ref_keys - known
                )
                complete_match = bool(ref_keys) and not missing and parse_error is None
                inference = {
                    "basis": "same archive basename; not an IDE/runtime binding",
                    "texture_reference_count": len(ref_keys),
                    "complete_texture_set_match": complete_match,
                    "missing_texture_references": missing,
                }
                inferred_models_by_txd[candidate_key].append(model)
                inferred_dff_bytes_by_txd[candidate_key] += entry.size_bytes
                inferred_refs_by_txd[candidate_key].update(ref_keys)
                classification = "dff_without_ide_binding_same_name_txd_candidate"
            else:
                classification = "dff_without_ide_txd_binding"
                annotate_reference_resolution(refs, None, model)
            model_rows.append(
                {
                    "model": model,
                    "dff": entry.name,
                    "dff_entry_index": entry.index,
                    "dff_bytes": entry.size_bytes,
                    "txd": None,
                    "binding": None,
                    "inferred_txd": inferred_txd,
                    "inference": inference,
                    "texture_references": refs,
                    "parse_error": parse_error,
                    "classification": classification,
                }
            )
            continue

        key = txd_key(binding.txd)
        models_by_txd[key].append(model)
        dff_bytes_by_txd[key] += entry.size_bytes
        known = known_textures_by_txd.get(key, set())
        annotate_reference_resolution(refs, key, model)
        for ref in refs:
            ref_key = (
                str(ref["name"]).casefold(),
                str(ref.get("mask") or "").casefold(),
            )
            refs_by_txd[key].add(ref_key)
            if ref_key not in known:
                unresolved_refs_by_txd[key].add(
                    f"{ref['name']}"
                    + (f" (mask={ref['mask']})" if ref.get("mask") else "")
                )
        model_rows.append(
            {
                "model": model,
                "dff": entry.name,
                "dff_entry_index": entry.index,
                "dff_bytes": entry.size_bytes,
                "txd": binding.txd,
                "binding": {"source": binding.source, "line": binding.line},
                "inferred_txd": None,
                "inference": None,
                "texture_references": refs,
                "parse_error": parse_error,
                "classification": "model_backed_txd",
            }
        )

    txd_rows: list[dict[str, Any]] = []
    txd_entries = [entry for entry in entries if entry.extension == ".txd"]
    for txd_slot, entry in enumerate(txd_entries):
        key = entry.name.casefold()
        inv = inventory_by_txd.get(key)
        resident = sum(
            int(texture.get("resident_bytes") or 0)
            for texture in (inv or {}).get("textures", [])
        )
        by_format: Counter[str] = Counter()
        for texture in (inv or {}).get("textures", []):
            by_format[str(texture.get("format") or "UNKNOWN")] += int(
                texture.get("resident_bytes") or 0
            )
        mapped_models = sorted(models_by_txd.get(key, []), key=str.casefold)
        inferred_models = sorted(inferred_models_by_txd.get(key, []), key=str.casefold)
        cross_txd_references = sorted(cross_txd_consumers.get(key, set()), key=str.casefold)
        if key in GLOBAL_TXD_NAMES:
            classification = "global_frontend_or_system_txd"
        elif mapped_models:
            classification = "model_backed_txd"
        elif inferred_models:
            complete = all(
                bool(
                    next(
                        row.get("inference")
                        for row in model_rows
                        if row["model"].casefold() == model.casefold()
                    ).get("complete_texture_set_match")
                )
                for model in inferred_models
            )
            classification = (
                "same_name_dff_candidate_complete_texture_match"
                if complete
                else "same_name_dff_candidate_unproven"
            )
        elif cross_txd_references:
            classification = "cross_txd_texture_donor_candidate"
        else:
            classification = "txd_without_dff_or_ide_owner"
        txd_rows.append(
            {
                "name": entry.name,
                "entry_index": entry.index,
                "txd_slot_assumption": txd_slot,
                "runtime_stream_id_assumption": 6500 + txd_slot,
                "archive_bytes": entry.size_bytes,
                "resident_bytes": resident,
                "resident_mib": bytes_to_mib(resident),
                "resident_bytes_by_format": dict(sorted(by_format.items())),
                "texture_count": len((inv or {}).get("textures", [])),
                "mapped_model_count": len(mapped_models),
                "mapped_models": mapped_models,
                "inferred_unbound_model_count": len(inferred_models),
                "inferred_unbound_models": inferred_models,
                "cross_txd_reference_count": len(cross_txd_references),
                "cross_txd_references": cross_txd_references,
                "dff_bytes": dff_bytes_by_txd.get(key, 0),
                "inferred_dff_bytes": inferred_dff_bytes_by_txd.get(key, 0),
                "texture_reference_count": len(refs_by_txd.get(key, set())),
                "inferred_texture_reference_count": len(
                    inferred_refs_by_txd.get(key, set())
                ),
                "unresolved_texture_references": sorted(
                    unresolved_refs_by_txd.get(key, set()), key=str.casefold
                ),
                "classification": classification,
                "inventory_present": inv is not None,
            }
        )

    dependency = {
        "data_root": str(data_root),
        "binding_count": len(bindings),
        "binding_error": binding_error,
        "dff_count": sum(entry.extension == ".dff" for entry in entries),
        "mapped_dff_count": sum(row["txd"] is not None for row in model_rows),
        "unmapped_dff_count": len(unmapped_dffs),
        "same_name_txd_candidate_count": sum(
            row["inferred_txd"] is not None for row in model_rows
        ),
        "unmapped_dffs": sorted(unmapped_dffs, key=str.casefold),
        "dff_parse_error_count": len(dff_parse_errors),
        "dff_parse_errors": dff_parse_errors,
        "unresolved_reference_classification": {
            "count": len(unresolved_reference_rows),
            "counts_by_resolution": dict(
                sorted(Counter(row["resolution"] for row in unresolved_reference_rows).items())
            ),
            "rows": sorted(
                unresolved_reference_rows,
                key=lambda row: (
                    str(row["model"]).casefold(),
                    str(row["texture"]).casefold(),
                    str(row["mask"]).casefold(),
                ),
            ),
        },
        "models": sorted(model_rows, key=lambda row: str(row["model"]).casefold()),
        "txds": sorted(txd_rows, key=lambda row: str(row["name"]).casefold()),
    }
    return dependency, {"models_by_txd": models_by_txd}


def build_static_world_report(
    entries: list[Any], image_handle: Any, data_root: Path, dependency: dict[str, Any]
) -> dict[str, Any]:
    ide = parse_ide_metadata(data_root)
    zones = parse_map_zones(data_root)
    instances = parse_ipl_instances(data_root, zones["zones"], ide["by_id"])
    by_name = ide["by_name"]
    by_model_id = instances["by_model_id"]

    def model_info_view(metadata: dict[str, Any]) -> dict[str, Any]:
        return {
            "id": metadata["id"],
            "section": metadata["section"],
            "source": metadata["relative_source"],
            "source_package": metadata["source_package"],
            "lod_distances": metadata["lod_distances"],
            "first_lod_distance": metadata["first_lod_distance"],
            "flags": metadata["flags"],
            "ignore_draw_distance": metadata["ignore_draw_distance"],
            "runtime_big_building": metadata["runtime_big_building"],
            "related_model": metadata["related_model"],
            "related_by": metadata["related_by"],
            "lod_role": metadata["lod_role"],
        }

    for model in dependency["models"]:
        metadata = by_name.get(str(model["model"]).casefold())
        model["model_info"] = model_info_view(metadata) if metadata else None
        model["static_world"] = (
            by_model_id.get(int(metadata["id"])) if metadata else None
        )

    scope_cost: Counter[str] = Counter()
    scope_txd_count: Counter[str] = Counter()
    lod_cost: Counter[str] = Counter()
    lod_txd_count: Counter[str] = Counter()
    for txd in dependency["txds"]:
        metadata_rows = [
            by_name[name.casefold()]
            for name in txd["mapped_models"]
            if name.casefold() in by_name
        ]
        world_metadata = [
            row for row in metadata_rows if row["section"] in WORLD_MODEL_SECTIONS
        ]
        instanced_metadata = [
            row for row in world_metadata if int(row["id"]) in by_model_id
        ]
        runtime_level_counts: Counter[str] = Counter()
        spatial_level_counts: Counter[str] = Counter()
        area_counts: Counter[str] = Counter()
        source_packages: set[str] = set()
        lod_roles: Counter[str] = Counter()
        for metadata in instanced_metadata:
            instance = by_model_id[int(metadata["id"])]
            runtime_level_counts.update(instance["runtime_levels"])
            spatial_level_counts.update(instance["spatial_levels"])
            area_counts.update(instance["areas"])
            source_packages.add(str(metadata["source_package"]))
            lod_roles[str(metadata["lod_role"])] += 1
        runtime_levels = set(runtime_level_counts)
        scope = static_world_scope(runtime_levels)
        big_count = sum(bool(row["runtime_big_building"]) for row in instanced_metadata)
        ordinary_count = len(instanced_metadata) - big_count
        if not instanced_metadata:
            lod_scope = "no_world_model"
        elif big_count and ordinary_count:
            lod_scope = "mixed_lod_and_ordinary"
        elif big_count:
            lod_scope = "lod_only"
        else:
            lod_scope = "ordinary_only"
        txd["static_world_scope"] = scope
        txd["runtime_level_instance_counts"] = dict(sorted(runtime_level_counts.items()))
        txd["spatial_level_instance_counts"] = dict(sorted(spatial_level_counts.items()))
        txd["area_instance_counts"] = dict(sorted(area_counts.items()))
        txd["world_instance_count"] = sum(runtime_level_counts.values())
        txd["interior_instance_count"] = sum(
            count for area, count in area_counts.items() if area != "0"
        )
        txd["world_model_info_count"] = len(world_metadata)
        txd["instanced_world_model_count"] = len(instanced_metadata)
        txd["lod_scope"] = lod_scope
        txd["lod_role_counts"] = dict(sorted(lod_roles.items()))
        txd["source_packages"] = sorted(source_packages)
        scope_cost[scope] += int(txd["resident_bytes"])
        scope_txd_count[scope] += 1
        lod_cost[lod_scope] += int(txd["resident_bytes"])
        lod_txd_count[lod_scope] += 1

    loose_cols_by_stem = {
        path.stem.casefold(): path
        for path in data_root.rglob("*.col")
        if path.is_file()
    }
    collision_rows: list[dict[str, Any]] = []
    collision_errors: list[str] = []
    for entry in entries:
        if entry.extension != ".col":
            continue
        names, errors = parse_col_model_names(read_entry(image_handle, entry), entry.name)
        collision_errors.extend(errors)
        runtime_levels: Counter[str] = Counter()
        spatial_levels: Counter[str] = Counter()
        matched_models: list[str] = []
        unmatched_models: list[str] = []
        packages: set[str] = set()
        for name in names:
            metadata = by_name.get(name.casefold())
            if metadata is None:
                unmatched_models.append(name)
                continue
            matched_models.append(name)
            packages.add(str(metadata["source_package"]))
            instance = by_model_id.get(int(metadata["id"]))
            if instance:
                runtime_levels.update(instance["runtime_levels"])
                spatial_levels.update(instance["spatial_levels"])
        loose_path = loose_cols_by_stem.get(Path(entry.name).stem.casefold())
        loose_names: list[str] = []
        loose_errors: list[str] = []
        if loose_path:
            loose_names, loose_errors = parse_col_model_names(
                loose_path.read_bytes(), str(loose_path)
            )
            collision_errors.extend(loose_errors)
        collision_rows.append(
            {
                "name": entry.name,
                "entry_index": entry.index,
                "archive_bytes": entry.size_bytes,
                "model_count": len(names),
                "model_info_match_count": len(matched_models),
                "unmatched_models": sorted(set(unmatched_models), key=str.casefold),
                "world_instance_count": sum(runtime_levels.values()),
                "runtime_level_instance_counts": dict(sorted(runtime_levels.items())),
                "spatial_level_instance_counts": dict(sorted(spatial_levels.items())),
                "static_world_scope": static_world_scope(set(runtime_levels)),
                "source_packages": sorted(packages),
                "loose_counterpart": str(loose_path) if loose_path else None,
                "loose_model_count": len(loose_names) if loose_path else None,
                "loose_model_set_matches_archive": (
                    set(name.casefold() for name in loose_names)
                    == set(name.casefold() for name in names)
                    if loose_path and not errors and not loose_errors
                    else None
                ),
                "parse_errors": errors,
            }
        )

    world_records = [
        row for row in ide["records"] if row["section"] in WORLD_MODEL_SECTIONS
    ]
    instanced_world_ids = {
        model_id for model_id in by_model_id if model_id in ide["by_id"]
    }
    return {
        "runtime_rules": {
            "zone_assignment": (
                "First containing map.zon entry, otherwise GENERIC; equivalent to "
                "CTheZones::GetLevelFromPosition."
            ),
            "big_building": (
                "objs/tobj first LOD distance > 300; related near model matches the "
                "model name after its first three characters within the IDE."
            ),
            "generic_override": (
                "Big buildings with first LOD distance > 2500 or IDE flag 0x100 "
                "are assigned GENERIC by CEntity::SetupBigBuilding."
            ),
        },
        "zone_file": zones["path"],
        "zone_count": len(zones["zones"]),
        "zone_errors": zones["errors"],
        "model_info_count": len(ide["records"]),
        "world_model_info_count": len(world_records),
        "runtime_big_building_count": sum(
            bool(row["runtime_big_building"]) for row in world_records
        ),
        "big_building_without_near_model_count": sum(
            row["lod_role"] == "runtime_big_building_without_near_model"
            for row in world_records
        ),
        "ide_parse_errors": ide["errors"],
        "ipl_instance_count": instances["total_instances"],
        "instanced_model_count": len(by_model_id),
        "instanced_world_model_count": len(instanced_world_ids),
        "ipl_model_info_miss_count": instances["model_info_miss_count"],
        "ipl_model_info_misses": instances["model_info_misses"],
        "ipl_id_name_mismatch_count": instances["id_name_mismatch_count"],
        "ipl_parse_errors": instances["errors"],
        "txd_cost_by_static_world_scope": {
            scope: {
                "txd_count": scope_txd_count[scope],
                "resident_bytes": resident,
                "resident_mib": bytes_to_mib(resident),
            }
            for scope, resident in sorted(scope_cost.items())
        },
        "txd_cost_by_lod_scope": {
            scope: {
                "txd_count": lod_txd_count[scope],
                "resident_bytes": resident,
                "resident_mib": bytes_to_mib(resident),
            }
            for scope, resident in sorted(lod_cost.items())
        },
        "collision": {
            "archive_col_count": len(collision_rows),
            "archive_bytes": sum(int(row["archive_bytes"]) for row in collision_rows),
            "rows": sorted(collision_rows, key=lambda row: str(row["name"]).casefold()),
            "parse_errors": collision_errors,
            "ownership_note": (
                "COL ownership is derived from model names inside each COLL stream and "
                "those models' IPL instances, matching how CColStore builds slot bounds."
            ),
        },
        "cost_note": (
            "TXD bytes are counted once in one static-world bucket. Shared TXDs are not "
            "split between islands, so bucket totals are residency upper bounds rather "
            "than simultaneous working-set predictions."
        ),
    }


def parse_runtime_log(path: Path | None) -> dict[str, Any]:
    if path is None or not path.is_file():
        return {"path": str(path) if path else None, "available": False}
    snapshots: list[dict[str, Any]] = []
    resident_lines: list[dict[str, Any]] = []
    same_frame_pressure = 0
    compaction_lines = 0
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "[WII-P0]" in raw and "{" in raw:
            candidate = raw[raw.find("{") :]
            try:
                payload = json.loads(candidate)
                if isinstance(payload, dict):
                    snapshots.append(payload)
            except json.JSONDecodeError:
                pass
        if "[WII-RESIDENT]" in raw:
            # The logger writes compact pool fields as gx1234/u5678 (no '=')
            # and the owner fields as owner=g0/u0 gx3282/u8982.
            match = re.search(r"owner=([^ ]+).*?\bgx(\d+)/u(\d+)", raw)
            if match:
                resident_lines.append(
                    {
                        "owner": match.group(1),
                        "gx_owner_kib": int(match.group(2)),
                        "gx_unknown_kib": int(match.group(3)),
                    }
                )
        if "restored priority request" in raw and "same-frame-pressure" in raw:
            same_frame_pressure += 1
        if "[WII-P0]" not in raw and "compaction" in raw.casefold() and "gx" in raw.casefold():
            compaction_lines += 1

    gx_owner = [int(item["gx_owner_kib"]) for item in resident_lines]
    gx_unknown = [int(item["gx_unknown_kib"]) for item in resident_lines]
    return {
        "path": str(path),
        "available": True,
        "p0_snapshot_count": len(snapshots),
        "p0_final_snapshot": snapshots[-1] if snapshots else None,
        "profile_id": snapshots[-1].get("profile_id") if snapshots else None,
        "build_id": snapshots[-1].get("build_id") if snapshots else None,
        "texture_candidate_state": snapshots[-1].get("texture_candidate_state") if snapshots else None,
        "resident_sample_count": len(resident_lines),
        "resident_gx_owner_kib_min": min(gx_owner) if gx_owner else None,
        "resident_gx_owner_kib_final": gx_owner[-1] if gx_owner else None,
        "resident_gx_unknown_kib_max": max(gx_unknown) if gx_unknown else None,
        "resident_gx_unknown_kib_final": gx_unknown[-1] if gx_unknown else None,
        "p0_gx_free_kib_min": min(
            int(item.get("gx_free") or 0) // 1024 for item in snapshots
            if item.get("gx_free") is not None
        ) if any(item.get("gx_free") is not None for item in snapshots) else None,
        "p0_gx_free_kib_final": (
            int(snapshots[-1].get("gx_free") or 0) // 1024
            if snapshots and snapshots[-1].get("gx_free") is not None else None
        ),
        "p0_gx_unknown_bytes_max": max(
            int(item.get("gx_unknown_bytes") or 0) for item in snapshots
        ) if snapshots else None,
        "p0_gx_unknown_bytes_final": (
            int(snapshots[-1].get("gx_unknown_bytes") or 0)
            if snapshots else None
        ),
        "p0_compaction_generation_max": max(
            int(item.get("gx_compaction_generation") or 0) for item in snapshots
        ) if snapshots else None,
        "p0_compaction_generation_final": (
            int(snapshots[-1].get("gx_compaction_generation") or 0)
            if snapshots else None
        ),
        "resident_samples": resident_lines[-20:],
        "same_frame_pressure_restore_count": same_frame_pressure,
        "compaction_line_count": compaction_lines,
        "owner_mapping_limitation": (
            "WII-RESIDENT reports pool ownership but does not identify the owning TXD/model; "
            "offline attribution below is therefore classified, not a runtime address map."
        ),
    }


def classify_unknowns(
    txd_rows: list[dict[str, Any]], entries: list[Any], loose_models_root: Path
) -> dict[str, Any]:
    no_model = [row for row in txd_rows if row["mapped_model_count"] == 0]
    global_rows = [row for row in txd_rows if row["classification"] == "global_frontend_or_system_txd"]
    classification_counts: Counter[str] = Counter()
    classification_bytes: Counter[str] = Counter()
    for row in no_model:
        classification = str(row["classification"])
        classification_counts[classification] += 1
        classification_bytes[classification] += int(row["resident_bytes"])
    other_extensions = Counter(entry.extension for entry in entries)
    loose_rows = []
    if loose_models_root.is_dir():
        for path in sorted(loose_models_root.glob("*.txd"), key=lambda item: item.name.casefold()):
            classification = (
                "global_frontend_or_system_loose_txd"
                if path.name.casefold() in GLOBAL_TXD_NAMES
                else "loose_txd_unknown_runtime_owner"
            )
            row = {
                "name": path.name,
                "path": str(path),
                "file_bytes": path.stat().st_size,
                "classification": classification,
                "inventory_present": False,
                "textures": [],
                "parse_error": None,
            }
            try:
                parsed = inspect_txd(
                    path.read_bytes(), str(path), require_gx=False, require_eof=True
                )
                row["inventory_present"] = True
                row["textures"] = parsed.get("textures", [])
                row["texture_count"] = len(row["textures"])
                row["resident_bytes"] = sum(
                    int(texture.get("resident_bytes") or 0)
                    for texture in row["textures"]
                )
            except Exception as exc:  # keep an unknown loose asset visible
                row["parse_error"] = str(exc)
                row["texture_count"] = 0
                row["resident_bytes"] = 0
            loose_rows.append(row)
    return {
        "global_frontend_or_system_txds": [row["name"] for row in global_rows],
        "txds_without_mapped_dff_count": len(no_model),
        "txds_without_mapped_dff_resident_bytes": sum(
            int(row["resident_bytes"]) for row in no_model
        ),
        "txds_without_mapped_dff": [
            {
                "name": row["name"],
                "resident_bytes": row["resident_bytes"],
                "resident_mib": row["resident_mib"],
                "classification": row["classification"],
                "inferred_unbound_models": row["inferred_unbound_models"],
                "inferred_texture_reference_count": row[
                    "inferred_texture_reference_count"
                ],
                "cross_txd_reference_count": row["cross_txd_reference_count"],
            }
            for row in no_model
        ],
        "classification_summary": {
            classification: {
                "txd_count": classification_counts[classification],
                "resident_bytes": resident,
                "resident_mib": bytes_to_mib(resident),
            }
            for classification, resident in sorted(classification_bytes.items())
        },
        "cross_txd_donor_candidates": [
            {
                "name": row["name"],
                "resident_bytes": row["resident_bytes"],
                "resident_mib": row["resident_mib"],
                "cross_txd_reference_count": row["cross_txd_reference_count"],
                "cross_txd_references": row["cross_txd_references"][:50],
            }
            for row in txd_rows
            if row["cross_txd_reference_count"]
        ],
        "loose_models_root": str(loose_models_root),
        "loose_txds": loose_rows,
        "archive_extension_counts": dict(sorted(other_extensions.items())),
        "classification_limit": (
            "A same-name DFF/TXD pair with a complete texture-set match is a strong "
            "archive candidate, not proof of a runtime model-info binding. TXDs without "
            "an IDE binding may also be script, frontend, particle, cutscene, unused "
            "archive residue, or another global consumer. Do not retire them from this "
            "classification alone."
        ),
    }


def graph_node_id(kind: str, scope: str, name: str, mask: str = "") -> str:
    parts = [kind, scope, name.casefold()]
    if mask:
        parts.append(mask.casefold())
    return ":".join(parts)


def build_dependency_graph(
    dependency: dict[str, Any],
    inventory_by_txd: dict[str, Any],
    unknowns: dict[str, Any],
    archive_path: Path,
) -> dict[str, Any]:
    """Build a model -> TXD -> texture graph plus direct DFF references."""
    nodes: dict[str, dict[str, Any]] = {}
    edges: dict[tuple[str, str, str], dict[str, Any]] = {}
    referenced_textures: set[tuple[str, str, str]] = set()

    def canonical_archive_txd_name(name: str) -> str:
        inventory = inventory_by_txd.get(txd_key(name))
        if inventory and inventory.get("source"):
            return str(inventory["source"])
        normalized = Path(name).name
        return normalized if normalized.casefold().endswith(".txd") else f"{normalized}.TXD"

    def add_node(node_id: str, kind: str, label: str, **attributes: Any) -> None:
        if node_id not in nodes:
            nodes[node_id] = {"id": node_id, "kind": kind, "label": label, **attributes}
        else:
            # A model reference may create a texture node before the inventory
            # pass adds its format/size metadata. Merge the later evidence.
            nodes[node_id].update(
                {key: value for key, value in attributes.items() if value is not None}
            )

    def add_edge(source: str, target: str, relation: str, **attributes: Any) -> None:
        key = (source, target, relation)
        if key not in edges:
            edges[key] = {
                "source": source,
                "target": target,
                "relation": relation,
                **attributes,
            }

    for model in dependency["models"]:
        model_id = graph_node_id("model", "archive", str(model["model"]))
        add_node(
            model_id,
            "model",
            str(model["model"]),
            dff=model["dff"],
            dff_entry_index=model["dff_entry_index"],
            dff_bytes=model["dff_bytes"],
            classification=model["classification"],
            parse_error=model["parse_error"],
            model_info=model.get("model_info"),
            static_world=model.get("static_world"),
        )
        linked_txd = model["txd"] or model.get("inferred_txd")
        if not linked_txd:
            continue
        txd_name = canonical_archive_txd_name(str(linked_txd))
        txd_id = graph_node_id("txd", "archive", txd_name)
        add_node(txd_id, "txd", txd_name, scope="archive")
        binding = model.get("binding") or {}
        relation = "ide_binding" if model["txd"] else "same_name_archive_candidate"
        add_edge(
            model_id,
            txd_id,
            relation,
            dff=model["dff"],
            binding_source=binding.get("source"),
            line=binding.get("line"),
            inference=model.get("inference"),
            texture_reference_count=len(model["texture_references"]),
        )
        txd_key_name = txd_key(txd_name)
        known_inventory = inventory_by_txd.get(txd_key_name, {})
        known_keys = {
            (
                str(texture.get("name") or "").casefold(),
                str(texture.get("mask") or "").casefold(),
            )
            for texture in known_inventory.get("textures", [])
        }
        for ref in model["texture_references"]:
            ref_name = str(ref["name"])
            ref_mask = str(ref.get("mask") or "")
            ref_key = (ref_name.casefold(), ref_mask.casefold())
            if ref_key in known_keys:
                texture_id = graph_node_id("texture", "archive", txd_name, f"{ref_name}|{ref_mask}")
                add_node(
                    texture_id,
                    "texture",
                    ref_name,
                    scope="archive",
                    txd=txd_name,
                    mask=ref_mask,
                )
                add_edge(model_id, texture_id, "dff_texture_reference", mask=ref_mask)
                referenced_textures.add((txd_key_name, ref_name.casefold(), ref_mask.casefold()))
            else:
                unresolved_id = graph_node_id(
                    "unresolved", "archive", txd_name, f"{ref_name}|{ref_mask}"
                )
                add_node(
                    unresolved_id,
                    "unresolved_texture",
                    ref_name,
                    scope="archive",
                    txd=txd_name,
                    mask=ref_mask,
                    resolution=ref.get("resolution"),
                    archive_candidate_txds=ref.get("archive_candidate_txds", []),
                )
                add_edge(
                    model_id,
                    unresolved_id,
                    "unresolved_texture_reference",
                    mask=ref_mask,
                    resolution=ref.get("resolution"),
                    archive_candidate_txds=ref.get("archive_candidate_txds", []),
                )

    for txd in dependency["txds"]:
        txd_name = str(txd["name"])
        txd_id = graph_node_id("txd", "archive", txd_name)
        add_node(
            txd_id,
            "txd",
            txd_name,
            scope="archive",
            entry_index=txd["entry_index"],
            txd_slot_assumption=txd["txd_slot_assumption"],
            runtime_stream_id_assumption=txd["runtime_stream_id_assumption"],
            resident_bytes=txd["resident_bytes"],
            texture_count=txd["texture_count"],
            mapped_model_count=txd["mapped_model_count"],
            inferred_unbound_model_count=txd["inferred_unbound_model_count"],
            classification=txd["classification"],
            static_world_scope=txd.get("static_world_scope"),
            lod_scope=txd.get("lod_scope"),
            runtime_level_instance_counts=txd.get("runtime_level_instance_counts"),
        )
        inventory = inventory_by_txd.get(txd_name.casefold(), {})
        for texture in inventory.get("textures", []):
            name = str(texture.get("name") or "")
            mask = str(texture.get("mask") or "")
            texture_id = graph_node_id("texture", "archive", txd_name, f"{name}|{mask}")
            add_node(
                texture_id,
                "texture",
                name,
                scope="archive",
                txd=txd_name,
                mask=mask,
                format=texture.get("format"),
                alpha_kind=texture.get("alpha_kind"),
                width=texture.get("width"),
                height=texture.get("height"),
                resident_bytes=int(texture.get("resident_bytes") or 0),
                runtime_path=texture.get("runtime_path"),
                direct_model_reference=(
                    txd_name.casefold(), name.casefold(), mask.casefold()
                ) in referenced_textures,
            )
            add_edge(
                txd_id,
                texture_id,
                "txd_inventory_member",
                mask=mask,
                format=texture.get("format"),
                resident_bytes=int(texture.get("resident_bytes") or 0),
            )

    for loose in unknowns["loose_txds"]:
        txd_name = str(loose["name"])
        txd_id = graph_node_id("txd", "loose", txd_name)
        add_node(
            txd_id,
            "txd",
            txd_name,
            scope="loose",
            file_bytes=loose["file_bytes"],
            resident_bytes=loose.get("resident_bytes", 0),
            texture_count=loose.get("texture_count", 0),
            classification=loose["classification"],
            parse_error=loose.get("parse_error"),
        )
        for texture in loose.get("textures", []):
            name = str(texture.get("name") or "")
            mask = str(texture.get("mask") or "")
            texture_id = graph_node_id("texture", "loose", txd_name, f"{name}|{mask}")
            add_node(
                texture_id,
                "texture",
                name,
                scope="loose",
                txd=txd_name,
                mask=mask,
                format=texture.get("format"),
                alpha_kind=texture.get("alpha_kind"),
                width=texture.get("width"),
                height=texture.get("height"),
                resident_bytes=int(texture.get("resident_bytes") or 0),
                runtime_path=texture.get("runtime_path"),
                direct_model_reference=False,
            )
            add_edge(
                txd_id,
                texture_id,
                "txd_inventory_member",
                mask=mask,
                format=texture.get("format"),
                resident_bytes=int(texture.get("resident_bytes") or 0),
            )

    shared_txds = [
        txd for txd in dependency["txds"] if int(txd["mapped_model_count"]) > 1
    ]
    archive_texture_count = sum(
        node["kind"] == "texture" and node.get("scope") == "archive"
        for node in nodes.values()
    )
    archive_direct_refs = sum(
        node["kind"] == "texture"
        and node.get("scope") == "archive"
        and bool(node.get("direct_model_reference"))
        for node in nodes.values()
    )
    graph = {
        "schema": "wii-model-txd-texture-graph-v2",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "archive": str(archive_path),
        "node_count": len(nodes),
        "edge_count": len(edges),
        "nodes": sorted(nodes.values(), key=lambda item: item["id"]),
        "edges": sorted(
            edges.values(), key=lambda item: (item["source"], item["target"], item["relation"])
        ),
        "summary": {
            "model_nodes": sum(node["kind"] == "model" for node in nodes.values()),
            "archive_txd_nodes": sum(
                node["kind"] == "txd" and node.get("scope") == "archive"
                for node in nodes.values()
            ),
            "loose_txd_nodes": sum(
                node["kind"] == "txd" and node.get("scope") == "loose"
                for node in nodes.values()
            ),
            "archive_texture_nodes": archive_texture_count,
            "archive_textures_directly_referenced_by_models": archive_direct_refs,
            "archive_textures_without_direct_model_reference": archive_texture_count - archive_direct_refs,
            "unresolved_texture_nodes": sum(
                node["kind"] == "unresolved_texture" for node in nodes.values()
            ),
            "shared_archive_txd_count": len(shared_txds),
            "top_shared_archive_txds": [
                {
                    "name": row["name"],
                    "mapped_model_count": row["mapped_model_count"],
                    "resident_bytes": row["resident_bytes"],
                    "models": row["mapped_models"][:25],
                }
                for row in sorted(
                    shared_txds,
                    key=lambda item: int(item["mapped_model_count"]),
                    reverse=True,
                )[:25]
            ],
        },
    }
    return graph


def dot_escape(value: Any) -> str:
    return str(value).replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ")


def graph_to_dot(graph: dict[str, Any]) -> str:
    lines = [
        "digraph ModelTxdTexture {",
        '  graph [rankdir=LR, overlap=false, splines=true];',
        '  node [fontname="Arial", fontsize=9];',
    ]
    shape_by_kind = {
        "model": "box",
        "txd": "ellipse",
        "texture": "point",
        "unresolved_texture": "diamond",
    }
    for node in graph["nodes"]:
        shape = shape_by_kind.get(node["kind"], "ellipse")
        label = node["label"] if node["kind"] != "texture" else node["label"]
        if node["kind"] == "texture":
            label = f"{node['label']}\\n{node.get('format', '')} {node.get('resident_bytes', 0)}B"
        if node["kind"] == "unresolved_texture":
            label = f"{node['label']}\\nUNRESOLVED"
        lines.append(
            f'  "{dot_escape(node["id"])}" [shape={shape}, label="{dot_escape(label)}"];'
        )
    for edge in graph["edges"]:
        label = edge["relation"]
        if edge.get("format"):
            label += f"\\n{edge['format']} {edge.get('resident_bytes', 0)}B"
        lines.append(
            f'  "{dot_escape(edge["source"])}" -> "{dot_escape(edge["target"])}" '
            f'[label="{dot_escape(label)}"];'
        )
    lines.append("}")
    return "\n".join(lines) + "\n"


def graph_markdown(graph: dict[str, Any]) -> str:
    summary = graph["summary"]
    lines = [
        "# Model to TXD to Texture Dependency Graph",
        "",
        f"Archive: `{graph['archive']}`",
        "",
        "This is an offline graph. Model to TXD edges distinguish proven IDE bindings from same-name archive candidates; TXD to texture edges come from the GX inventory; direct model texture edges come from parsed DFF material references.",
        "",
        "## Counts",
        "",
        f"- Nodes: **{graph['node_count']}**; edges: **{graph['edge_count']}**",
        f"- Models: **{summary['model_nodes']}**",
        f"- Archive TXDs: **{summary['archive_txd_nodes']}**; loose TXDs: **{summary['loose_txd_nodes']}**",
        f"- Archive textures: **{summary['archive_texture_nodes']}**",
        f"- Archive textures with direct parsed model references: **{summary['archive_textures_directly_referenced_by_models']}**",
        f"- Archive textures without direct parsed model references: **{summary['archive_textures_without_direct_model_reference']}**",
        f"- Unresolved model texture references: **{summary['unresolved_texture_nodes']}**",
        f"- Shared archive TXDs (more than one mapped model): **{summary['shared_archive_txd_count']}**",
        "",
        "## Most Shared TXDs",
        "",
        "| TXD | Models | Resident MiB | Example models |",
        "|---|---:|---:|---|",
    ]
    for row in summary["top_shared_archive_txds"]:
        lines.append(
            f"| {row['name']} | {row['mapped_model_count']} | {bytes_to_mib(row['resident_bytes'])} | {', '.join(row['models'][:8])} |"
        )
    lines.extend(
        [
            "",
            "## Interpretation Boundary",
            "",
            "A TXD with no direct parsed model texture edge is not automatically unused: scripts, frontend code, particles, cutscenes, LOD-only paths, and runtime-generated material references remain outside this static graph.",
            "",
            "The DOT file is a complete graph export; large graphs are best filtered by `kind`, `scope`, or TXD name before rendering.",
        ]
    )
    return "\n".join(lines) + "\n"


def make_report(args: argparse.Namespace) -> tuple[dict[str, Any], dict[str, Any]]:
    archive = args.archive.resolve()
    dir_path, image_path, entries = open_archive(archive)
    inventory_payload, inventory_by_txd = load_inventory(args.inventory.resolve())
    inventory_rows = list(inventory_by_txd.values())

    current_hashes = {
        "img_sha256": sha256_file(image_path),
        "dir_sha256": sha256_file(dir_path),
    }
    source_hashes = {
        "img_sha256": str(inventory_payload.get("source_img_sha256") or ""),
        "dir_sha256": str(inventory_payload.get("source_dir_sha256") or ""),
    }
    with image_path.open("rb") as image_handle:
        dependency, _ = build_dependency_report(
            entries, image_handle, inventory_by_txd, args.data_root.resolve()
        )
        static_world = build_static_world_report(
            entries, image_handle, args.data_root.resolve(), dependency
        )

    txd_rows = dependency["txds"]
    unknowns = classify_unknowns(txd_rows, entries, archive.parent)
    graph = build_dependency_graph(
        dependency, inventory_by_txd, unknowns, image_path
    )
    report = {
        "schema": "wii-gx-lifecycle-audit-v2",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": (
            "offline archive cost, DFF/IDE/TXD dependency mapping, runtime-equivalent "
            "IPL island/LOD/COL classification, and optional runtime evidence"
        ),
        "archive": {
            "img": str(image_path),
            "dir": str(dir_path),
            "entry_count": len(entries),
            "hashes": current_hashes,
            "inventory_source_hashes": source_hashes,
            "matches_inventory": current_hashes == source_hashes,
        },
        "inventory": {
            "path": str(args.inventory.resolve()),
            "schema": inventory_payload.get("inventory_schema"),
            "source": inventory_payload.get("source"),
            "summary": inventory_summary(inventory_rows),
        },
        "assumptions": {
            "model_info_size": 6500,
            "runtime_stream_id": "6500 + TXD ordinal in gta3.dir",
            "runtime_stream_id_note": (
                "This ID is an explicit ordinal assumption for correlation only; verify against the deployed DOL before using it as a runtime owner key."
            ),
            "resident_bytes": "inventory resident_bytes, including GX tiled payload alignment",
        },
        "archive_extension_counts": dict(
            sorted(Counter(entry.extension for entry in entries).items())
        ),
        "dependency": dependency,
        "static_world_lifecycle": static_world,
        "unknown_owner_classification": unknowns,
        "dependency_graph": {
            "schema": graph["schema"],
            "summary": graph["summary"],
        },
        "runtime_evidence": parse_runtime_log(args.log.resolve() if args.log else None),
    }
    return report, graph


def markdown_report(report: dict[str, Any]) -> str:
    archive = report["archive"]
    inv = report["inventory"]["summary"]
    dep = report["dependency"]
    world = report["static_world_lifecycle"]
    unresolved = dep["unresolved_reference_classification"]
    unknown = report["unknown_owner_classification"]
    graph = report["dependency_graph"]
    runtime = report["runtime_evidence"]
    lines = [
        "# Wii GX Lifecycle Audit",
        "",
        f"Generated (UTC): `{report['generated_at_utc']}`",
        "",
        "## Scope",
        "",
        report["scope"],
        "",
        "## Archive and Inventory",
        "",
        f"- IMG: `{archive['img']}`",
        f"- DIR: `{archive['dir']}`",
        f"- Archive entries: **{archive['entry_count']}**",
        f"- Current IMG SHA256: `{archive['hashes']['img_sha256']}`",
        f"- Current DIR SHA256: `{archive['hashes']['dir_sha256']}`",
        f"- Inventory source hashes match current archive: **{archive['matches_inventory']}**",
        f"- TXDs: **{inv['txd_count']}**; textures: **{inv['texture_count']}**",
        f"- Total resident texture bytes: **{bytes_to_mib(inv['resident_bytes_total'])} MiB**",
        "",
        "Format resident bytes:",
        "",
        "| Format | Textures | Resident MiB |",
        "|---|---:|---:|",
    ]
    for fmt in sorted(inv["formats"]):
        lines.append(
            f"| {fmt} | {inv['formats'][fmt]} | {bytes_to_mib(inv['resident_bytes_by_format'][fmt])} |"
        )
    lines.extend(
        [
            "",
            "## Static Dependency Coverage",
            "",
            f"- IDE bindings: **{dep['binding_count']}**",
            f"- DFFs: **{dep['dff_count']}**; mapped: **{dep['mapped_dff_count']}**; unmapped: **{dep['unmapped_dff_count']}**",
            f"- Unmapped DFFs with a same-name archive TXD candidate: **{dep['same_name_txd_candidate_count']}**",
            f"- DFF parse errors: **{dep['dff_parse_error_count']}**",
            f"- Texture reference occurrences missing from the declared/inferred TXD: **{unresolved['count']}**.",
            "- Cross-TXD resolution: `"
            + ", ".join(
                f"{key}={value}"
                for key, value in unresolved["counts_by_resolution"].items()
            )
            + "`.",
            "",
            "Top TXDs by offline GX resident cost:",
            "",
            "| TXD | Resident MiB | Textures | Models | Classification |",
            "|---|---:|---:|---:|---|",
        ]
    )
    for row in sorted(dep["txds"], key=lambda item: int(item["resident_bytes"]), reverse=True)[:25]:
        lines.append(
            f"| {row['name']} | {row['resident_mib']} | {row['texture_count']} | {row['mapped_model_count']} | {row['classification']} |"
        )
    lines.extend(
        [
            "",
            "## Static World Lifecycle",
            "",
            f"- Map zones: **{world['zone_count']}**; IPL instances: **{world['ipl_instance_count']}**.",
            f"- IDE world models: **{world['world_model_info_count']}**; runtime big-building/LOD models: **{world['runtime_big_building_count']}**.",
            f"- Big-building models without a related near model: **{world['big_building_without_near_model_count']}**.",
            f"- IPL parse errors: **{len(world['ipl_parse_errors'])}**; IDE parse errors: **{len(world['ide_parse_errors'])}**; IPL model-info misses: **{world['ipl_model_info_miss_count']}**.",
            "",
            "TXD GX cost by runtime island scope:",
            "",
            "| Scope | TXDs | Resident MiB |",
            "|---|---:|---:|",
        ]
    )
    for scope, row in world["txd_cost_by_static_world_scope"].items():
        lines.append(f"| {scope} | {row['txd_count']} | {row['resident_mib']} |")
    lines.extend(
        [
            "",
            "TXD GX cost by LOD ownership:",
            "",
            "| LOD scope | TXDs | Resident MiB |",
            "|---|---:|---:|",
        ]
    )
    for scope, row in world["txd_cost_by_lod_scope"].items():
        lines.append(f"| {scope} | {row['txd_count']} | {row['resident_mib']} |")
    lines.extend(
        [
            "",
            f"Streamed collision archives: **{world['collision']['archive_col_count']}**; parse errors: **{len(world['collision']['parse_errors'])}**.",
            "",
            world["cost_note"],
            "",
            "## Unknown or Global Candidates",
            "",
            f"- TXDs with no mapped DFF: **{unknown['txds_without_mapped_dff_count']}** ({bytes_to_mib(unknown['txds_without_mapped_dff_resident_bytes'])} MiB)",
            f"- Known global/frontend names found: `{', '.join(unknown['global_frontend_or_system_txds']) or 'none'}`",
            f"- Loose model-directory TXDs: **{len(unknown['loose_txds'])}**; these are outside `gta3.img` and are classified separately.",
            f"- TXDs referenced as cross-TXD texture donor candidates: **{len(unknown['cross_txd_donor_candidates'])}** (only candidates, not lifetime-safe ownership proof).",
            "",
            unknown["classification_limit"],
            "",
            "| Classification | TXDs | Resident MiB |",
            "|---|---:|---:|",
        ]
    )
    for classification, row in unknown["classification_summary"].items():
        lines.append(
            f"| {classification} | {row['txd_count']} | {row['resident_mib']} |"
        )
    lines.extend(
        [
            "",
            "## Dependency Graph",
            "",
            f"- Nodes: **{graph['summary']['model_nodes']} models**, **{graph['summary']['archive_txd_nodes']} archive TXDs**, **{graph['summary']['archive_texture_nodes']} archive textures**.",
            f"- Direct parsed model texture references: **{graph['summary']['archive_textures_directly_referenced_by_models']}**; no direct model reference: **{graph['summary']['archive_textures_without_direct_model_reference']}**.",
            f"- Unresolved model texture references: **{graph['summary']['unresolved_texture_nodes']}**.",
            "- Full graph files are emitted next to this report as JSON, DOT, and Markdown summary.",
            "",
            "## Runtime Evidence (Optional Log)",
            "",
        ]
    )
    if runtime.get("available"):
        lines.extend(
            [
                f"- Log: `{runtime['path']}`",
                f"- Log profile/build: `{runtime['profile_id']}` / `{runtime['build_id']}`",
                f"- Texture candidate state in log: **{runtime['texture_candidate_state']}**",
                f"- GX resident samples: **{runtime['resident_sample_count']}**",
                f"- Resident owner GX bytes (min/final): **{runtime['resident_gx_owner_kib_min']} / {runtime['resident_gx_owner_kib_final']} KiB**",
                f"- Resident unknown GX bytes (max/final): **{runtime['resident_gx_unknown_kib_max']} / {runtime['resident_gx_unknown_kib_final']} KiB**",
                f"- Final P0 snapshot GX free: **{runtime['p0_gx_free_kib_final']} KiB**; P0 minimum: **{runtime['p0_gx_free_kib_min']} KiB**",
                f"- P0 GX unknown bytes (final/max): **{bytes_to_mib(runtime['p0_gx_unknown_bytes_final'] or 0)} / {bytes_to_mib(runtime['p0_gx_unknown_bytes_max'] or 0)} MiB**",
                f"- Same-frame priority restores: **{runtime['same_frame_pressure_restore_count']}**",
                f"- P0 compaction generation (final/max): **{runtime['p0_compaction_generation_final']} / {runtime['p0_compaction_generation_max']}**; explicit compaction lines: **{runtime['compaction_line_count']}**",
                "",
                runtime["owner_mapping_limitation"],
            ]
        )
    else:
        lines.append("No runtime log was supplied or found; this report remains offline-only.")
    lines.extend(
        [
            "",
            "## Interpretation Boundary",
            "",
            "This report proves byte cost, static dependency relationships, island/LOD assignment, and COL model ownership from shipped data. It does not prove that a TXD is retired-safe: live references, current visibility, request state, and handoff state still require runtime lifecycle instrumentation.",
            "",
        ]
    )
    return "\n".join(lines)


def write_lifecycle_matrices(report: dict[str, Any], output: Path) -> dict[str, str]:
    txd_path = output / "txd-lifecycle-matrix.csv"
    collision_path = output / "collision-lifecycle-matrix.csv"
    unknown_path = output / "unknown-owner-matrix.csv"

    with txd_path.open("w", encoding="utf-8", newline="") as handle:
        fieldnames = [
            "name",
            "resident_bytes",
            "resident_mib",
            "classification",
            "mapped_model_count",
            "inferred_unbound_model_count",
            "static_world_scope",
            "lod_scope",
            "world_model_info_count",
            "instanced_world_model_count",
            "world_instance_count",
            "interior_instance_count",
            "runtime_level_instance_counts",
            "spatial_level_instance_counts",
            "source_packages",
            "cross_txd_reference_count",
            "cross_txd_references",
        ]
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in report["dependency"]["txds"]:
            writer.writerow(
                {
                    key: (
                        json.dumps(row.get(key), sort_keys=True)
                        if isinstance(row.get(key), (dict, list))
                        else row.get(key)
                    )
                    for key in fieldnames
                }
            )

    with collision_path.open("w", encoding="utf-8", newline="") as handle:
        fieldnames = [
            "name",
            "archive_bytes",
            "model_count",
            "model_info_match_count",
            "world_instance_count",
            "static_world_scope",
            "runtime_level_instance_counts",
            "spatial_level_instance_counts",
            "source_packages",
            "unmatched_models",
            "loose_counterpart",
            "loose_model_set_matches_archive",
            "parse_errors",
        ]
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in report["static_world_lifecycle"]["collision"]["rows"]:
            writer.writerow(
                {
                    key: (
                        json.dumps(row.get(key), sort_keys=True)
                        if isinstance(row.get(key), (dict, list))
                        else row.get(key)
                    )
                    for key in fieldnames
                }
            )

    with unknown_path.open("w", encoding="utf-8", newline="") as handle:
        fieldnames = [
            "name",
            "resident_bytes",
            "resident_mib",
            "classification",
            "inferred_unbound_models",
            "inferred_texture_reference_count",
            "cross_txd_reference_count",
        ]
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in report["unknown_owner_classification"]["txds_without_mapped_dff"]:
            writer.writerow(
                {
                    key: (
                        json.dumps(row.get(key), sort_keys=True)
                        if isinstance(row.get(key), (dict, list))
                        else row.get(key)
                    )
                    for key in fieldnames
                }
            )
    return {
        "txd_lifecycle_csv": str(txd_path),
        "collision_lifecycle_csv": str(collision_path),
        "unknown_owner_csv": str(unknown_path),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--data-root", type=Path, default=DEFAULT_DATA_ROOT)
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report, graph = make_report(args)
    args.output.mkdir(parents=True, exist_ok=True)
    json_path = args.output / "gx-lifecycle-audit.json"
    md_path = args.output / "gx-lifecycle-audit.md"
    graph_json_path = args.output / "dependency-graph.json"
    graph_dot_path = args.output / "dependency-graph.dot"
    graph_md_path = args.output / "dependency-graph.md"
    report["matrix_files"] = write_lifecycle_matrices(report, args.output)
    report["dependency_graph"]["files"] = {
        "json": str(graph_json_path),
        "dot": str(graph_dot_path),
        "markdown": str(graph_md_path),
    }
    json_path.write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    md_path.write_text(markdown_report(report), encoding="utf-8")
    graph_json_path.write_text(json.dumps(graph, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    graph_dot_path.write_text(graph_to_dot(graph), encoding="utf-8")
    graph_md_path.write_text(graph_markdown(graph), encoding="utf-8")
    print(json_path)
    print(md_path)
    print(json.dumps({
        "archive_matches_inventory": report["archive"]["matches_inventory"],
        "entry_count": report["archive"]["entry_count"],
        "txd_count": report["inventory"]["summary"]["txd_count"],
        "texture_count": report["inventory"]["summary"]["texture_count"],
        "mapped_dffs": report["dependency"]["mapped_dff_count"],
        "unmapped_dffs": report["dependency"]["unmapped_dff_count"],
        "dff_parse_errors": report["dependency"]["dff_parse_error_count"],
        "same_name_txd_candidates": report["dependency"]["same_name_txd_candidate_count"],
        "world_instances": report["static_world_lifecycle"]["ipl_instance_count"],
        "runtime_big_buildings": report["static_world_lifecycle"]["runtime_big_building_count"],
        "collision_parse_errors": len(report["static_world_lifecycle"]["collision"]["parse_errors"]),
        "graph_nodes": graph["node_count"],
        "graph_edges": graph["edge_count"],
        "graph_unresolved_texture_nodes": graph["summary"]["unresolved_texture_nodes"],
        "graph_orphan_archive_textures": graph["summary"]["archive_textures_without_direct_model_reference"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
