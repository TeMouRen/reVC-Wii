#!/usr/bin/env python3
"""Audit MatFX environment assets and run a small offline CPU reference probe.

This tool deliberately does not emulate GX geometry or replace the runtime
MatFX pass.  It answers the narrower offline questions:

* Are the base/environment images present, dimensionally sane, and equivalent
  between two decoded asset sets?
* Does the environment image itself contain sparse high-value pixels that can
  become white dots when the reflection contribution is too strong?
* How does the decoded environment contribution change at the coefficient
  sweep used for Wii debugging (0, .25, .5, 1)?

The optional composite images use an identity UV proxy.  They are labelled as
such in the report because real MatFX UVs come from transformed normals at
runtime and cannot be recovered from two textures alone.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Iterable

from PIL import Image


DEFAULT_COEFFICIENTS = (0.0, 0.25, 0.5, 1.0)


def _percentile(values: list[int], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def _sha256(image: Image.Image) -> str:
    return hashlib.sha256(image.tobytes()).hexdigest()


def _image_summary(path: Path, image: Image.Image) -> dict[str, object]:
    pixels = list(image.getdata())
    rgb_luma = [sum(pixel[:3]) / 3.0 for pixel in pixels]
    alpha = [pixel[3] for pixel in pixels]
    bright_200 = sum(1 for value in rgb_luma if value >= 200.0)
    bright_240 = sum(1 for value in rgb_luma if value >= 240.0)
    return {
        "path": str(path.resolve()),
        "width": image.width,
        "height": image.height,
        "sha256_rgba": _sha256(image),
        "mean_rgb": [
            round(sum(pixel[channel] for pixel in pixels) / len(pixels), 6)
            for channel in range(3)
        ],
        "mean_luma": round(sum(rgb_luma) / len(rgb_luma), 6),
        "p95_luma": round(_percentile([round(value) for value in rgb_luma], 0.95), 6),
        "p99_luma": round(_percentile([round(value) for value in rgb_luma], 0.99), 6),
        "max_luma": round(max(rgb_luma, default=0.0), 6),
        "nonblack_pixels": sum(1 for value in rgb_luma if value > 0.0),
        "bright_pixels_ge_200": bright_200,
        "bright_pixels_ge_240": bright_240,
        "bright_fraction_ge_200": round(bright_200 / len(pixels), 8),
        "bright_fraction_ge_240": round(bright_240 / len(pixels), 8),
        "alpha_min": min(alpha, default=0),
        "alpha_max": max(alpha, default=0),
        "alpha_nonopaque_pixels": sum(1 for value in alpha if value != 255),
    }


def _load(path: Path) -> Image.Image:
    with Image.open(path) as source:
        return source.convert("RGBA")


def _compare_images(left: Image.Image, right: Image.Image) -> dict[str, object]:
    if left.size != right.size:
        return {
            "same_dimensions": False,
            "left_size": list(left.size),
            "right_size": list(right.size),
        }
    left_bytes = left.tobytes()
    right_bytes = right.tobytes()
    squared_error = 0
    max_error = 0
    changed_pixels = 0
    for offset in range(0, len(left_bytes), 4):
        pixel_changed = False
        for channel in range(4):
            error = abs(left_bytes[offset + channel] - right_bytes[offset + channel])
            squared_error += error * error
            max_error = max(max_error, error)
            pixel_changed |= error != 0
        changed_pixels += pixel_changed
    samples = len(left_bytes)
    mse = squared_error / samples if samples else 0.0
    psnr = math.inf if mse == 0.0 else 10.0 * math.log10((255.0 * 255.0) / mse)
    return {
        "same_dimensions": True,
        "left_sha256_rgba": hashlib.sha256(left_bytes).hexdigest(),
        "right_sha256_rgba": hashlib.sha256(right_bytes).hexdigest(),
        "changed_pixels": changed_pixels,
        "total_pixels": left.width * left.height,
        "changed_fraction": round(changed_pixels / (left.width * left.height), 8),
        "rgba_mse": round(mse, 8),
        "rgba_psnr_db": "inf" if math.isinf(psnr) else round(psnr, 6),
        "max_absolute_error": max_error,
    }


def _sample_repeat_bilinear(image: Image.Image, u: float, v: float) -> tuple[float, ...]:
    """Sample an RGBA image with repeat addressing and bilinear filtering."""
    width, height = image.size
    pixels = image.load()
    u %= 1.0
    v %= 1.0
    x = u * width - 0.5
    y = v * height - 0.5
    x0 = math.floor(x)
    y0 = math.floor(y)
    tx = x - x0
    ty = y - y0
    result: list[float] = []
    for channel in range(4):
        value = 0.0
        for dy, wy in ((0, 1.0 - ty), (1, ty)):
            for dx, wx in ((0, 1.0 - tx), (1, tx)):
                sample = pixels[(x0 + dx) % width, (y0 + dy) % height][channel]
                value += sample * wx * wy
        result.append(value)
    return tuple(result)


def _proxy_composite(
    base: Image.Image, env: Image.Image, coefficient: float
) -> tuple[Image.Image, dict[str, object]]:
    """Composite env over base using identity UVs as an offline proxy."""
    output = Image.new("RGBA", base.size)
    destination = output.load()
    base_pixels = base.load()
    env_values: list[float] = []
    saturated = 0
    for y in range(base.height):
        v = (y + 0.5) / base.height
        for x in range(base.width):
            u = (x + 0.5) / base.width
            base_pixel = base_pixels[x, y]
            env_pixel = _sample_repeat_bilinear(env, u, v)
            base_alpha = base_pixel[3] / 255.0
            # Equivalent to the opaque/fbAlpha-disabled PC path for this
            # proxy: base*alpha + env*coefficient*max(alpha, 1).
            fba = max(base_alpha, 1.0)
            channels = []
            for channel in range(3):
                value = base_pixel[channel] * base_alpha + env_pixel[channel] * coefficient * fba
                saturated += value >= 255.0
                channels.append(max(0, min(255, round(value))))
                env_values.append(env_pixel[channel] * coefficient * fba)
            destination[x, y] = (*channels, base_pixel[3])
    return output, {
        "uv_model": "identity-repeat-bilinear-proxy",
        "coefficient": coefficient,
        "env_contribution_mean": round(sum(env_values) / len(env_values), 6),
        "env_contribution_p95": round(_percentile([round(value) for value in env_values], 0.95), 6),
        "env_contribution_max": round(max(env_values, default=0.0), 6),
        "composite_channel_saturated_samples": saturated,
    }


def _coefficient_probe(env: Image.Image, coefficient: float) -> dict[str, object]:
    values = [channel for pixel in env.getdata() for channel in pixel[:3]]
    scaled = [value * coefficient for value in values]
    return {
        "coefficient": coefficient,
        "mean_contribution": round(sum(scaled) / len(scaled), 6),
        "p95_contribution": round(_percentile([round(value) for value in scaled], 0.95), 6),
        "max_contribution": round(max(scaled, default=0.0), 6),
        "samples_ge_200": sum(1 for value in scaled if value >= 200.0),
        "samples_ge_240": sum(1 for value in scaled if value >= 240.0),
        "clipped_samples": sum(1 for value in scaled if value > 255.0),
    }


def _parse_coefficients(raw: str) -> tuple[float, ...]:
    values = tuple(float(item.strip()) for item in raw.split(",") if item.strip())
    if not values or any(value < 0.0 for value in values):
        raise argparse.ArgumentTypeError("coefficients must be a non-empty list of non-negative numbers")
    return values


def _write_png(path: Path, image: Image.Image) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, format="PNG")


def build_report(
    base_path: Path,
    env_path: Path,
    compare_base_path: Path | None,
    compare_env_path: Path | None,
    coefficients: Iterable[float],
    render_dir: Path | None,
) -> dict[str, object]:
    base = _load(base_path)
    env = _load(env_path)
    report: dict[str, object] = {
        "tool": "analyze_matfx_assets.py",
        "scope": "offline asset audit and CPU reference probe; not a GX geometry emulation",
        "base": _image_summary(base_path, base),
        "environment": _image_summary(env_path, env),
        "proxy_composite": {
            "warning": "identity UV proxy only; runtime MatFX derives UVs from transformed normals",
            "coefficient_sweep": [],
        },
        "environment_coefficient_sweep": [_coefficient_probe(env, value) for value in coefficients],
    }
    if compare_base_path is not None:
        compare_base = _load(compare_base_path)
        report["base_comparison"] = {
            "left": str(base_path.resolve()),
            "right": str(compare_base_path.resolve()),
            "metrics": _compare_images(base, compare_base),
        }
    if compare_env_path is not None:
        compare_env = _load(compare_env_path)
        report["environment_comparison"] = {
            "left": str(env_path.resolve()),
            "right": str(compare_env_path.resolve()),
            "metrics": _compare_images(env, compare_env),
        }
    proxy_rows = report["proxy_composite"]["coefficient_sweep"]
    assert isinstance(proxy_rows, list)
    for coefficient in coefficients:
        image, metrics = _proxy_composite(base, env, coefficient)
        row = dict(metrics)
        if render_dir is not None:
            output_path = render_dir / f"matfx_proxy_coeff_{coefficient:g}.png"
            _write_png(output_path, image)
            row["png"] = str(output_path.resolve())
        proxy_rows.append(row)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path, help="decoded base/material texture PNG")
    parser.add_argument("--env", required=True, type=Path, help="decoded environment texture PNG")
    parser.add_argument("--compare-base", type=Path, help="second decoded base PNG to compare")
    parser.add_argument("--compare-env", type=Path, help="second decoded environment PNG to compare")
    parser.add_argument("--output", required=True, type=Path, help="JSON report path")
    parser.add_argument("--render-dir", type=Path, help="optional directory for identity-UV proxy PNGs")
    parser.add_argument(
        "--coefficients",
        type=_parse_coefficients,
        default=DEFAULT_COEFFICIENTS,
        help="comma-separated non-negative coefficients (default: 0,0.25,0.5,1)",
    )
    args = parser.parse_args(argv)
    for path in (args.base, args.env, args.compare_base, args.compare_env):
        if path is not None and not path.is_file():
            parser.error(f"file not found: {path}")
    try:
        report = build_report(
            args.base,
            args.env,
            args.compare_base,
            args.compare_env,
            args.coefficients,
            args.render_dir,
        )
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, ensure_ascii=True), encoding="utf-8")
    base = report["base"]
    env = report["environment"]
    assert isinstance(base, dict) and isinstance(env, dict)
    print(
        f"base={base['width']}x{base['height']} env={env['width']}x{env['height']} "
        f"env_sha256={env['sha256_rgba']} bright>=240={env['bright_pixels_ge_240']}"
    )
    comparison = report.get("environment_comparison")
    if isinstance(comparison, dict):
        metrics = comparison["metrics"]
        assert isinstance(metrics, dict)
        print(
            f"env_compare changed={metrics.get('changed_pixels', 'n/a')} "
            f"max_error={metrics.get('max_absolute_error', 'n/a')} "
            f"psnr={metrics.get('rgba_psnr_db', 'n/a')}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
