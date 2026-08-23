#!/usr/bin/env python3
"""Generate a read-only GX/TXD ownership report for a GTA IMG archive.

The report deliberately separates offline asset facts from runtime log facts. It
does not modify an archive, a deployed DOL, or the game source/runtime.
"""

from __future__ import annotations

import argparse
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
    dff_bytes_by_txd: Counter[str] = Counter()
    refs_by_txd: dict[str, set[tuple[str, str]]] = defaultdict(set)
    unresolved_refs_by_txd: dict[str, set[str]] = defaultdict(set)

    for entry in entries:
        if entry.extension != ".dff":
            continue
        model = Path(entry.name).stem
        binding = bindings.get(model.casefold())
        if binding is None:
            unmapped_dffs.append(entry.name)
            model_rows.append(
                {
                    "model": model,
                    "dff": entry.name,
                    "dff_entry_index": entry.index,
                    "dff_bytes": entry.size_bytes,
                    "txd": None,
                    "binding": None,
                    "texture_references": [],
                    "parse_error": None,
                    "classification": "dff_without_ide_txd_binding",
                }
            )
            continue

        key = txd_key(binding.txd)
        models_by_txd[key].append(model)
        dff_bytes_by_txd[key] += entry.size_bytes
        refs: list[dict[str, str]] = []
        parse_error = None
        try:
            parsed = extract_dff_texture_references(read_entry(image_handle, entry), entry.name)
            for ref in parsed:
                refs.append({"name": ref.name, "mask": ref.mask})
                ref_key = (ref.name.casefold(), ref.mask.casefold())
                refs_by_txd[key].add(ref_key)
                txd_inventory = inventory_by_txd.get(key)
                known = set()
                if txd_inventory:
                    known = {
                        (str(item.get("name") or "").casefold(),
                         str(item.get("mask") or "").casefold())
                        for item in txd_inventory.get("textures", [])
                    }
                if ref_key not in known:
                    unresolved_refs_by_txd[key].add(
                        f"{ref.name}" + (f" (mask={ref.mask})" if ref.mask else "")
                    )
        except RwParseError as exc:
            parse_error = str(exc)
            dff_parse_errors.append({"dff": entry.name, "error": parse_error})

        model_rows.append(
            {
                "model": model,
                "dff": entry.name,
                "dff_entry_index": entry.index,
                "dff_bytes": entry.size_bytes,
                "txd": binding.txd,
                "binding": {"source": binding.source, "line": binding.line},
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
        if key in GLOBAL_TXD_NAMES:
            classification = "global_frontend_or_system_txd"
        elif not mapped_models:
            classification = "txd_without_mapped_dff_candidate_global_or_script"
        else:
            classification = "model_backed_txd"
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
                "dff_bytes": dff_bytes_by_txd.get(key, 0),
                "texture_reference_count": len(refs_by_txd.get(key, set())),
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
        "unmapped_dffs": sorted(unmapped_dffs, key=str.casefold),
        "dff_parse_error_count": len(dff_parse_errors),
        "dff_parse_errors": dff_parse_errors,
        "models": sorted(model_rows, key=lambda row: str(row["model"]).casefold()),
        "txds": sorted(txd_rows, key=lambda row: str(row["name"]).casefold()),
    }
    return dependency, {"models_by_txd": models_by_txd}


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
            }
            for row in no_model
        ],
        "loose_models_root": str(loose_models_root),
        "loose_txds": loose_rows,
        "archive_extension_counts": dict(sorted(other_extensions.items())),
        "classification_limit": (
            "No-mapped-DFF means the static IDE/DFF audit cannot prove a model owner. "
            "It may be a script, frontend, particle, cutscene, or other global consumer. "
            "Do not treat it as safe to retire without runtime owner evidence."
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
        )
        if not model["txd"]:
            continue
        txd_name = canonical_archive_txd_name(str(model["txd"]))
        txd_id = graph_node_id("txd", "archive", txd_name)
        add_node(txd_id, "txd", txd_name, scope="archive")
        binding = model.get("binding") or {}
        add_edge(
            model_id,
            txd_id,
            "ide_binding",
            dff=model["dff"],
            binding_source=binding.get("source"),
            line=binding.get("line"),
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
                )
                add_edge(model_id, unresolved_id, "unresolved_texture_reference", mask=ref_mask)

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
            classification=txd["classification"],
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
        "schema": "wii-model-txd-texture-graph-v1",
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
        "This is an offline graph. Model to TXD edges come from IDE bindings; TXD to texture edges come from the GX inventory; direct model texture edges come from parsed DFF material references.",
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

    txd_rows = dependency["txds"]
    unknowns = classify_unknowns(txd_rows, entries, archive.parent)
    graph = build_dependency_graph(
        dependency, inventory_by_txd, unknowns, image_path
    )
    report = {
        "schema": "wii-gx-lifecycle-audit-v1",
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "offline archive cost, static DFF/IDE dependency mapping, and optional runtime evidence",
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
            f"- DFF parse errors: **{dep['dff_parse_error_count']}**",
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
            "## Unknown or Global Candidates",
            "",
            f"- TXDs with no mapped DFF: **{unknown['txds_without_mapped_dff_count']}** ({bytes_to_mib(unknown['txds_without_mapped_dff_resident_bytes'])} MiB)",
            f"- Known global/frontend names found: `{', '.join(unknown['global_frontend_or_system_txds']) or 'none'}`",
            f"- Loose model-directory TXDs: **{len(unknown['loose_txds'])}**; these are outside `gta3.img` and are classified separately.",
            "",
            unknown["classification_limit"],
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
            "This report proves byte cost and static dependency relationships. It does not prove that a TXD is retired-safe: runtime stream owners, collision/LOD residency, active material references, and handoff state still require lifecycle instrumentation.",
            "",
        ]
    )
    return "\n".join(lines)


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
        "graph_nodes": graph["node_count"],
        "graph_edges": graph["edge_count"],
        "graph_unresolved_texture_nodes": graph["summary"]["unresolved_texture_nodes"],
        "graph_orphan_archive_textures": graph["summary"]["archive_textures_without_direct_model_reference"],
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
