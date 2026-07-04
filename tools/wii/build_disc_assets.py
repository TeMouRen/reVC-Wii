#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
import subprocess
import tempfile
import wave
from collections import defaultdict, deque
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


IMET_TITLE_OFFSET = 0x5E
IMET_TITLE_SPAN = 84
IMET_TITLE_SLOTS = 7
IMET_MD5_OFFSET = 0x5F0
IMET_HEADER_SIZE = 0x600
MAX_OPENING_BNR_SIZE = 512 * 1024
WII_DISC_MAGIC = 0x5D1C9EA3
WII_SECOND_MAGIC = 0x00000000

FFMPEG_EXE = Path(shutil.which("ffmpeg") or r"C:\Program Files (x86)\ffmpeg\bin\ffmpeg.exe")
GX_TEXCONV = Path(r"C:\devkitPro\tools\bin\gxtexconv.exe")

TITLE_TPL_SPECS = {
    "banner_bg00.tpl": {"size": (304, 228), "format": 4},
    "title_00.tpl": {"size": (400, 176), "format": 5},
    "icon_bg00.tpl": {"size": (128, 96), "format": 5},
    "title_i00.tpl": {"size": (128, 64), "format": 5},
}

CUSTOM_ART_FILES = {
    "banner_bg00.tpl": "304x228.png",
    "title_00.tpl": "400x176.png",
    "icon_bg00.tpl": "128x96.png",
    "title_i00.tpl": "128x64.png",
}

DSP_ADPCM_COEFFICIENTS = [
    1820,
    -856,
    3238,
    -1514,
    2333,
    -550,
    3336,
    -1376,
    2444,
    -949,
    3666,
    -1764,
    2654,
    -701,
    3420,
    -1398,
]


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def clamp(value: int, lower: int, upper: int) -> int:
    return max(lower, min(upper, value))


def to_posix_path(path: Path) -> str:
    return path.resolve().as_posix()


def read_be32(buf: bytes, offset: int) -> int:
    return struct.unpack(">I", buf[offset : offset + 4])[0]


def write_be32(buf: bytearray, offset: int, value: int) -> None:
    buf[offset : offset + 4] = struct.pack(">I", value)


def md5(data: bytes) -> bytes:
    return hashlib.md5(data).digest()


def run_checked(command: list[str], tool_name: str) -> None:
    result = subprocess.run(command, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        details = result.stderr.strip() or result.stdout.strip() or f"{tool_name} exited with {result.returncode}"
        raise RuntimeError(f"{tool_name} failed: {details}")


def load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        Path(r"C:\Windows\Fonts\arialbd.ttf"),
        Path(r"C:\Windows\Fonts\ARLRDBD.TTF"),
        Path(r"C:\Windows\Fonts\segoeuib.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size=size)
    return ImageFont.load_default()


def draw_vertical_gradient(image: Image.Image, top: tuple[int, int, int], bottom: tuple[int, int, int]) -> None:
    width, height = image.size
    draw = ImageDraw.Draw(image)
    for y in range(height):
        t = y / max(height - 1, 1)
        color = tuple(int((1.0 - t) * top[i] + t * bottom[i]) for i in range(3))
        draw.line((0, y, width, y), fill=color)


def add_vice_city_background(image: Image.Image) -> Image.Image:
    width, height = image.size
    draw_vertical_gradient(image, (18, 185, 202), (16, 43, 76))
    draw = ImageDraw.Draw(image, "RGBA")

    sun_bounds = (
        int(-width * 0.08),
        int(height * 0.06),
        int(width * 0.46),
        int(height * 0.62),
    )
    draw.ellipse(sun_bounds, fill=(255, 150, 86, 255))

    sun_top = sun_bounds[1] + 16
    sun_bottom = sun_bounds[3] - 16
    for y in range(sun_top, sun_bottom, 10):
        draw.line((sun_bounds[0], y, sun_bounds[2], y), fill=(255, 197, 136, 180), width=2)

    horizon_y = int(height * 0.74)
    for grid_y in range(horizon_y, height, max(8, height // 18)):
        draw.line((0, grid_y, width, grid_y), fill=(201, 248, 255, 90), width=1)
    step = max(20, width // 12)
    for x in range(-width, width * 2, step):
        draw.line((x, height, width // 2, horizon_y), fill=(201, 248, 255, 70), width=1)

    stripe_color = (255, 97, 152, 110)
    stripe_w = max(14, width // 18)
    for i in range(-2, 7):
        x0 = int(width * 0.58 + i * stripe_w * 1.4)
        draw.polygon(
            [
                (x0, 0),
                (x0 + stripe_w, 0),
                (x0 - stripe_w // 2, height),
                (x0 - stripe_w - stripe_w // 2, height),
            ],
            fill=stripe_color,
        )

    vignette = Image.new("RGBA", image.size, (0, 0, 0, 0))
    vignette_draw = ImageDraw.Draw(vignette)
    vignette_draw.rectangle((0, 0, width, height), fill=(0, 0, 0, 60))
    vignette_mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(vignette_mask).ellipse(
        (-width * 0.15, -height * 0.1, width * 1.15, height * 1.15),
        fill=255,
    )
    vignette_mask = vignette_mask.filter(ImageFilter.GaussianBlur(radius=max(12, width // 16)))
    vignette.putalpha(Image.eval(vignette_mask, lambda x: 255 - x))
    return Image.alpha_composite(image.convert("RGBA"), vignette)


def generate_background(size: tuple[int, int], alpha: bool) -> Image.Image:
    base = Image.new("RGB", size, (0, 0, 0))
    composed = add_vice_city_background(base)
    if alpha:
        return composed.convert("RGBA")
    return composed.convert("RGB")


def fit_inside(source_size: tuple[int, int], box_size: tuple[int, int]) -> tuple[int, int]:
    sw, sh = source_size
    bw, bh = box_size
    scale = min(bw / sw, bh / sh)
    return (max(1, int(sw * scale)), max(1, int(sh * scale)))


def paste_center(dest: Image.Image, sprite: Image.Image, y_offset: int = 0) -> None:
    x = (dest.width - sprite.width) // 2
    y = (dest.height - sprite.height) // 2 + y_offset
    dest.alpha_composite(sprite, (x, y))


def render_logo_layer(size: tuple[int, int], logo_path: Path, subtitle: str | None = None) -> Image.Image:
    canvas = Image.new("RGBA", size, (0, 0, 0, 0))
    logo = Image.open(logo_path).convert("RGBA")

    padding_x = max(16, size[0] // 12)
    padding_y = max(12, size[1] // 8)
    logo_box = (size[0] - padding_x * 2, size[1] - padding_y * 2 - (26 if subtitle and size[0] >= 256 else 0))
    logo_size = fit_inside(logo.size, logo_box)
    logo = logo.resize(logo_size, Image.LANCZOS)

    shadow = Image.new("RGBA", logo.size, (0, 0, 0, 0))
    shadow.putalpha(logo.getchannel("A"))
    shadow = shadow.filter(ImageFilter.GaussianBlur(radius=max(3, size[0] // 90)))
    shadow_draw = ImageDraw.Draw(shadow)
    shadow_draw.rectangle((0, 0, 0, 0), fill=(0, 0, 0, 0))

    shadow_color = Image.new("RGBA", shadow.size, (12, 24, 42, 210))
    shadow = Image.composite(shadow_color, Image.new("RGBA", shadow.size, (0, 0, 0, 0)), shadow.getchannel("A"))

    paste_center(canvas, shadow, y_offset=4)
    paste_center(canvas, logo)

    if subtitle:
        font = load_font(max(14, size[1] // 8))
        draw = ImageDraw.Draw(canvas)
        bbox = draw.textbbox((0, 0), subtitle, font=font)
        text_w = bbox[2] - bbox[0]
        text_h = bbox[3] - bbox[1]
        x = (size[0] - text_w) // 2
        y = size[1] - text_h - max(6, size[1] // 14)
        draw.text((x + 2, y + 2), subtitle, font=font, fill=(10, 24, 38, 180))
        draw.text((x, y), subtitle, font=font, fill=(255, 245, 230, 235))

    return canvas


def gxtexconv_to_tpl(input_png: Path, output_tpl: Path, colfmt: int, width: int, height: int) -> None:
    if not GX_TEXCONV.exists():
        raise FileNotFoundError(f"gxtexconv.exe not found: {GX_TEXCONV}")
    run_checked(
        [
        str(GX_TEXCONV),
        "-i",
        to_posix_path(input_png),
        "-o",
        to_posix_path(output_tpl),
        f"colfmt={colfmt}",
        f"width={width}",
        f"height={height}",
        ],
        "gxtexconv",
    )


def lz10_decompress(data: bytes) -> bytes:
    if not data or data[0] != 0x10:
        raise ValueError("Unsupported LZ10 stream")
    size = data[1] | (data[2] << 8) | (data[3] << 16)
    pos = 4
    out = bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b1 = data[pos]
                b2 = data[pos + 1]
                pos += 2
                length = (b1 >> 4) + 3
                disp = ((b1 & 0x0F) << 8) | b2
                src = len(out) - disp - 1
                for _ in range(length):
                    out.append(out[src])
                    src += 1
            else:
                out.append(data[pos])
                pos += 1
    return bytes(out)


def lz10_compress(data: bytes) -> bytes:
    size = len(data)
    out = bytearray([0x10, size & 0xFF, (size >> 8) & 0xFF, (size >> 16) & 0xFF])
    positions: dict[bytes, deque[int]] = defaultdict(deque)

    def add_position(index: int) -> None:
        if index + 3 > size:
            return
        key = data[index : index + 3]
        dq = positions[key]
        dq.append(index)

    pos = 0
    while pos < size:
        flag_index = len(out)
        out.append(0)
        flags = 0
        for bit in range(8):
            if pos >= size:
                break

            best_length = 0
            best_disp = 0
            if pos + 3 <= size:
                key = data[pos : pos + 3]
                dq = positions.get(key)
                if dq:
                    window_start = pos - 0x1000
                    while dq and dq[0] < window_start:
                        dq.popleft()
                    for candidate in list(dq)[-64:][::-1]:
                        max_len = min(18, size - pos)
                        match_len = 3
                        while match_len < max_len and data[candidate + match_len] == data[pos + match_len]:
                            match_len += 1
                        if match_len > best_length:
                            best_length = match_len
                            best_disp = pos - candidate
                            if best_length == 18:
                                break

            if best_length >= 3:
                flags |= 0x80 >> bit
                disp_minus_one = best_disp - 1
                out.append(((best_length - 3) << 4) | ((disp_minus_one >> 8) & 0x0F))
                out.append(disp_minus_one & 0xFF)
                start = pos
                pos += best_length
                for item_pos in range(start, pos):
                    add_position(item_pos)
            else:
                out.append(data[pos])
                add_position(pos)
                pos += 1

        out[flag_index] = flags

    return bytes(out)


def read_wave_properties(wav_path: Path) -> tuple[int, int, int]:
    with wave.open(str(wav_path), "rb") as wav_file:
        return wav_file.getframerate(), wav_file.getnchannels(), wav_file.getnframes()


def normalize_wav(input_wav: Path, output_wav: Path, sample_rate: int, channels: int) -> None:
    if not FFMPEG_EXE.exists():
        raise FileNotFoundError(f"ffmpeg.exe not found: {FFMPEG_EXE}")
    run_checked(
        [
            str(FFMPEG_EXE),
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(input_wav),
            "-vn",
            "-acodec",
            "pcm_s16le",
            "-ar",
            str(sample_rate),
            "-ac",
            str(channels),
            str(output_wav),
        ],
        "ffmpeg",
    )


def load_pcm_wave(wav_path: Path) -> tuple[bytes, int, int, int]:
    with wave.open(str(wav_path), "rb") as wav_file:
        channels = wav_file.getnchannels()
        sample_rate = wav_file.getframerate()
        sample_width = wav_file.getsampwidth()
        frame_count = wav_file.getnframes()
        if wav_file.getcomptype() != "NONE":
            raise ValueError(f"{wav_path} must be uncompressed PCM")
        if sample_width != 2:
            raise ValueError(f"{wav_path} must be 16-bit PCM")
        return wav_file.readframes(frame_count), sample_rate, channels, frame_count


class BnsEncoder:
    def __init__(self, channels: int) -> None:
        self.channels = channels
        self.histories = [[0, 0], [0, 0]]

    def find_exponent(self, residual: float) -> int:
        exponent = 0
        while residual > 7.5 or residual < -8.5:
            exponent += 1
            residual /= 2.0
        return exponent

    def determine_exponent(self, channel_index: int, predictor_index: int, samples: list[int]) -> int:
        factor1 = DSP_ADPCM_COEFFICIENTS[predictor_index * 2]
        factor2 = DSP_ADPCM_COEFFICIENTS[predictor_index * 2 + 1]
        older, newer = self.histories[channel_index]
        max_residual = 0.0
        for sample in samples:
            predictor = (newer * factor1 + older * factor2) >> 11
            max_residual = max(max_residual, abs(sample - predictor))
            older, newer = newer, sample
        return self.find_exponent(max_residual)

    def compress_candidate(
        self,
        channel_index: int,
        predictor_index: int,
        samples: list[int],
    ) -> tuple[float, bytes, list[int]]:
        factor1 = DSP_ADPCM_COEFFICIENTS[predictor_index * 2]
        factor2 = DSP_ADPCM_COEFFICIENTS[predictor_index * 2 + 1]
        exponent = self.determine_exponent(channel_index, predictor_index, samples)

        while exponent <= 15:
            encoded = [0] * 8
            encoded[0] = ((predictor_index & 0x0F) << 4) | (exponent & 0x0F)
            older, newer = self.histories[channel_index]
            error = 0.0
            for sample_index, sample in enumerate(samples):
                predictor = (newer * factor1 + older * factor2) >> 11
                residual = (sample - predictor) >> exponent
                if residual > 7 or residual < -8:
                    exponent += 1
                    break

                nibble = clamp(residual, -8, 7)
                packed = nibble & 0x0F
                byte_index = sample_index // 2 + 1
                if sample_index & 1:
                    encoded[byte_index] |= packed
                else:
                    encoded[byte_index] = packed << 4

                predictor += nibble << exponent
                older, newer = newer, clamp(predictor, -32768, 32767)
                error += float((newer - sample) ** 2)
            else:
                return error, bytes(encoded), [older, newer]

        return float("inf"), b"", self.histories[channel_index][:]

    def encode_block(self, channel_index: int, samples: list[int]) -> bytes:
        best_error = float("inf")
        best_encoded = b""
        best_history = self.histories[channel_index][:]
        for predictor_index in range(8):
            error, encoded, history = self.compress_candidate(channel_index, predictor_index, samples)
            if error < best_error:
                best_error = error
                best_encoded = encoded
                best_history = history
        self.histories[channel_index] = best_history
        return best_encoded


def split_pcm_channels(frame_data: bytes, channels: int) -> list[list[int]]:
    total_samples = len(frame_data) // 2
    samples = list(struct.unpack(f"<{total_samples}h", frame_data))
    if channels == 1:
        return [samples]
    if channels == 2:
        return [samples[0::2], samples[1::2]]
    raise ValueError(f"Unsupported channel count for BNS: {channels}")


def build_bns_info(sample_rate: int, channels: int, sample_count: int, channel_data_size: int) -> bytes:
    info = bytearray()
    if channels == 1:
        info += b"INFO"
        info += struct.pack(">I", 0x60)
        info += bytes([0, 0, 1, 0])
        info += struct.pack(">H", sample_rate)
        info += struct.pack(">H", 0)
        info += struct.pack(">I", 0)
        info += struct.pack(">I", sample_count)
        info += struct.pack(">I", 0x18)
        info += struct.pack(">I", 0)
        info += struct.pack(">I", 0x1C)
        info += struct.pack(">I", 0)
        info += struct.pack(">I", 0x28)
        info += struct.pack(">I", 0)
        for coefficient in DSP_ADPCM_COEFFICIENTS:
            info += struct.pack(">h", coefficient)
        info += b"\x00" * 16
        return bytes(info)

    info += b"INFO"
    info += struct.pack(">I", 0xA0)
    info += bytes([0, 0, 2, 0])
    info += struct.pack(">H", sample_rate)
    info += struct.pack(">H", 0)
    info += struct.pack(">I", 0)
    info += struct.pack(">I", sample_count)
    info += struct.pack(">I", 0x18)
    info += struct.pack(">I", 0)
    info += struct.pack(">I", 0x20)
    info += struct.pack(">I", 0x2C)
    info += struct.pack(">I", 0)
    info += struct.pack(">I", 0x38)
    info += struct.pack(">I", 0)
    info += struct.pack(">I", channel_data_size)
    info += struct.pack(">I", 0x68)
    info += struct.pack(">I", 0)
    for coefficient in DSP_ADPCM_COEFFICIENTS:
        info += struct.pack(">h", coefficient)
    info += b"\x00" * 16
    for coefficient in DSP_ADPCM_COEFFICIENTS:
        info += struct.pack(">h", coefficient)
    info += b"\x00" * 16
    return bytes(info)


def build_bns(frame_data: bytes, sample_rate: int, channels: int) -> bytes:
    if channels not in (1, 2):
        raise ValueError("BNS encoder supports only mono or stereo PCM")
    if sample_rate < 32000 or sample_rate > 48000:
        raise ValueError(f"BNS sample rate must be between 32000 and 48000 Hz, got {sample_rate}")

    channel_samples = split_pcm_channels(frame_data, channels)
    sample_count = len(channel_samples[0])
    block_count = (sample_count + 13) // 14

    encoder = BnsEncoder(channels)
    channel_data = [bytearray(block_count * 8) for _ in range(channels)]
    for channel_index, samples in enumerate(channel_samples):
        for block_index in range(block_count):
            start = block_index * 14
            block_samples = samples[start : start + 14]
            if len(block_samples) < 14:
                block_samples = block_samples + [0] * (14 - len(block_samples))
            encoded = encoder.encode_block(channel_index, block_samples)
            offset = block_index * 8
            channel_data[channel_index][offset : offset + 8] = encoded

    data_payload = bytes(channel_data[0]) if channels == 1 else bytes(channel_data[0] + channel_data[1])
    info_chunk = build_bns_info(sample_rate, channels, sample_count, block_count * 8)
    data_chunk = b"DATA" + struct.pack(">I", len(data_payload) + 8) + data_payload

    header = struct.pack(
        ">4sIIHHIIII",
        b"BNS ",
        0xFEFF0100,
        0x20 + len(info_chunk) + len(data_chunk),
        0x20,
        0x0002,
        0x20,
        len(info_chunk),
        0x20 + len(info_chunk),
        len(data_chunk),
    )
    return header + info_chunk + data_chunk


def build_sound_blob_from_wav(source_wav: Path, temp_dir: Path, sample_rate: int, channels: int) -> tuple[bytes, dict[str, float | int | str]]:
    normalized_wav = temp_dir / f"sound_{sample_rate}_{channels}ch.wav"
    normalize_wav(source_wav, normalized_wav, sample_rate=sample_rate, channels=channels)
    frame_data, actual_rate, actual_channels, frame_count = load_pcm_wave(normalized_wav)
    bns_data = build_bns(frame_data, actual_rate, actual_channels)
    sound_blob = rebuild_imd5_blob(bns_data)
    return sound_blob, {
        "source": "sound.wav",
        "format": "BNS",
        "sample_rate": actual_rate,
        "channels": actual_channels,
        "frames": frame_count,
        "seconds": frame_count / actual_rate,
        "packed_bytes": len(sound_blob),
    }


def build_sound_candidates(asset_dir: Path | None, template_sound: bytes, temp_dir: Path) -> list[tuple[str, bytes, dict[str, float | int | str]]]:
    candidates: list[tuple[str, bytes, dict[str, float | int | str]]] = [
        (
            "template sound.bin",
            template_sound,
            {
                "source": "template sound.bin",
                "format": "IMD5/LZ77",
                "packed_bytes": len(template_sound),
            },
        )
    ]
    if asset_dir is None or not asset_dir.exists():
        return candidates

    sound_bin_path = asset_dir / "sound.bin"
    if sound_bin_path.exists():
        custom_sound = sound_bin_path.read_bytes()
        candidates.insert(
            0,
            (
                "custom sound.bin",
                custom_sound,
                {
                    "source": "sound.bin",
                    "format": "IMD5/LZ77",
                    "packed_bytes": len(custom_sound),
                },
            ),
        )
        return candidates

    sound_wav_path = asset_dir / "sound.wav"
    if not sound_wav_path.exists():
        return candidates

    original_rate, original_channels, _ = read_wave_properties(sound_wav_path)
    target_variants: list[tuple[int, int, str]] = []
    seen: set[tuple[int, int]] = set()

    def add_variant(sample_rate: int, channels: int, label: str) -> None:
        bounded_rate = clamp(sample_rate, 32000, 48000)
        key = (bounded_rate, channels)
        if key in seen:
            return
        seen.add(key)
        target_variants.append((bounded_rate, channels, label))

    add_variant(original_rate, min(original_channels, 2), "preserve source rate/channels")
    if original_rate != 32000:
        add_variant(32000, min(original_channels, 2), "resample to 32000 Hz")
    if original_channels > 1:
        add_variant(original_rate, 1, "downmix to mono")
    if original_channels > 1 and original_rate != 32000:
        add_variant(32000, 1, "downmix to mono and resample to 32000 Hz")

    for sample_rate, channels, label in target_variants:
        sound_blob, info = build_sound_blob_from_wav(sound_wav_path, temp_dir, sample_rate=sample_rate, channels=channels)
        candidates.insert(0, (f"{label} ({channels}ch @ {sample_rate} Hz)", sound_blob, info))

    return candidates


def load_custom_images(asset_dir: Path) -> dict[str, Image.Image]:
    images: dict[str, Image.Image] = {}
    for tpl_name, file_name in CUSTOM_ART_FILES.items():
        image_path = asset_dir / file_name
        if not image_path.exists():
            continue
        image = Image.open(image_path)
        expected_size = TITLE_TPL_SPECS[tpl_name]["size"]
        if image.size != expected_size:
            raise ValueError(f"{image_path} must be {expected_size[0]}x{expected_size[1]}, got {image.size[0]}x{image.size[1]}")
        images[tpl_name.replace(".tpl", ".png")] = image.convert("RGB" if TITLE_TPL_SPECS[tpl_name]["format"] == 4 else "RGBA")
    return images


def parse_u8_entries(buf: bytes) -> list[dict[str, int | str]]:
    if buf[:4] != b"U\xaa8-":
        raise ValueError("Not a U8 archive")
    root_offset = read_be32(buf, 4)
    node_count = read_be32(buf, root_offset + 8)
    string_table = root_offset + node_count * 12
    entries: list[dict[str, int | str]] = []
    for index in range(node_count):
        offset = root_offset + index * 12
        type_name = read_be32(buf, offset)
        node_type = type_name >> 24
        name_offset = type_name & 0x00FFFFFF
        data_offset = read_be32(buf, offset + 4)
        size = read_be32(buf, offset + 8)
        if index == 0:
            name = ""
        else:
            start = string_table + name_offset
            end = buf.index(0, start)
            name = buf[start:end].decode("ascii")
        entries.append(
            {
                "index": index,
                "type": node_type,
                "name": name,
                "data_offset": data_offset,
                "size": size,
            }
        )
    return entries


def extract_file_entries(buf: bytes) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for entry in parse_u8_entries(buf):
        if entry["type"] != 0:
            continue
        start = int(entry["data_offset"])
        end = start + int(entry["size"])
        files[str(entry["name"])] = buf[start:end]
    return files


def patch_fixed_size_files(inner_archive: bytes, replacements: dict[str, bytes]) -> bytes:
    patched = bytearray(inner_archive)
    for entry in parse_u8_entries(inner_archive):
        if entry["type"] != 0:
            continue
        name = str(entry["name"])
        if name not in replacements:
            continue
        replacement = replacements[name]
        expected_size = int(entry["size"])
        if len(replacement) != expected_size:
            raise ValueError(f"{name} size mismatch: expected {expected_size}, got {len(replacement)}")
        start = int(entry["data_offset"])
        end = start + expected_size
        patched[start:end] = replacement
    return bytes(patched)


def rebuild_imd5_blob(inner_archive: bytes) -> bytes:
    compressed = lz10_compress(inner_archive)
    payload = b"LZ77" + compressed
    header = bytearray(32)
    header[0:4] = b"IMD5"
    write_be32(header, 4, len(payload))
    header[16:32] = md5(payload)
    return bytes(header) + payload


def decode_imd5_lz77(blob: bytes) -> bytes:
    if blob[:4] != b"IMD5":
        raise ValueError("Unexpected IMD5 header")
    payload = blob[32:]
    if md5(payload) != blob[16:32]:
        raise ValueError("IMD5 checksum mismatch")
    if payload[:4] != b"LZ77":
        raise ValueError("Expected LZ77 wrapper")
    return lz10_decompress(payload[4:])


def build_outer_u8(file_blobs: dict[str, bytes]) -> bytes:
    ordered_names = ["banner.bin", "icon.bin", "sound.bin"]
    strings = b"meta\x00banner.bin\x00icon.bin\x00sound.bin\x00"
    name_offsets = {
        "meta": 0,
        "banner.bin": 5,
        "icon.bin": 16,
        "sound.bin": 25,
    }

    root_offset = 0x20
    node_count = 5
    nodes_size = node_count * 12
    header_size = nodes_size + len(strings) + 1
    data_offset = align(root_offset + header_size, 0x20)

    archive = bytearray()
    archive += struct.pack(">IIII", 0x55AA382D, root_offset, header_size, data_offset)
    archive += b"\x00" * 16

    file_offsets: dict[str, int] = {}
    cursor = data_offset
    for name in ordered_names:
        file_offsets[name] = cursor
        cursor = align(cursor + len(file_blobs[name]), 0x20)

    nodes = [
        (0x01000000, 0, node_count),
        (0x01000000 | name_offsets["meta"], 0, node_count),
        (name_offsets["banner.bin"], file_offsets["banner.bin"], len(file_blobs["banner.bin"])),
        (name_offsets["icon.bin"], file_offsets["icon.bin"], len(file_blobs["icon.bin"])),
        (name_offsets["sound.bin"], file_offsets["sound.bin"], len(file_blobs["sound.bin"])),
    ]

    for type_name, data_value, size_value in nodes:
        archive += struct.pack(">III", type_name, data_value, size_value)
    archive += strings
    archive += b"\x00"
    while len(archive) < data_offset:
        archive += b"\x00"

    for name in ordered_names:
        blob = file_blobs[name]
        archive += blob
        while len(archive) % 0x20:
            archive += b"\x00"

    return bytes(archive)


def patch_imet_titles(header: bytearray, title: str) -> None:
    title_bytes = title.encode("utf-16-be")
    if len(title_bytes) > IMET_TITLE_SPAN:
        raise ValueError("IMET title is too long")
    for slot in range(IMET_TITLE_SLOTS):
        start = IMET_TITLE_OFFSET + slot * IMET_TITLE_SPAN
        header[start : start + IMET_TITLE_SPAN] = b"\x00" * IMET_TITLE_SPAN
        header[start : start + len(title_bytes)] = title_bytes
    header[IMET_MD5_OFFSET : IMET_MD5_OFFSET + 16] = b"\x00" * 16
    header[IMET_MD5_OFFSET : IMET_MD5_OFFSET + 16] = md5(bytes(header[:IMET_HEADER_SIZE]))


def patch_boot_bin(boot_path: Path, id6: str, title: str) -> None:
    boot = bytearray(boot_path.read_bytes())
    if len(id6) != 6:
        raise ValueError("ID6 must be exactly 6 characters")
    title_bytes = title.encode("ascii")
    if len(title_bytes) >= 0x40:
        raise ValueError("boot.bin title must fit inside 63 ASCII bytes")
    boot[0:6] = id6.encode("ascii")
    boot[0x20:0x60] = b"\x00" * 0x40
    boot[0x20 : 0x20 + len(title_bytes)] = title_bytes
    write_be32(boot, 0x18, WII_DISC_MAGIC)
    write_be32(boot, 0x1C, WII_SECOND_MAGIC)
    boot_path.write_bytes(bytes(boot))


def assert_imd5_payload(blob: bytes) -> None:
    payload = blob[32:]
    if md5(payload) != blob[16:32]:
        raise ValueError("IMD5 validation failed")


def build_preview_assets(preview_dir: Path, generated_images: dict[str, Image.Image]) -> None:
    preview_dir.mkdir(parents=True, exist_ok=True)
    for name, image in generated_images.items():
        image.save(preview_dir / name)


def make_generated_images(logo_path: Path, subtitle: str) -> dict[str, Image.Image]:
    return {
        "banner_bg00.png": generate_background((304, 228), alpha=False),
        "icon_bg00.png": generate_background((128, 96), alpha=True),
        "title_00.png": render_logo_layer((400, 176), logo_path, subtitle=subtitle),
        "title_i00.png": render_logo_layer((128, 64), logo_path, subtitle=None),
    }


def convert_generated_tpls(images: dict[str, Image.Image], work_dir: Path) -> dict[str, bytes]:
    outputs: dict[str, bytes] = {}
    for png_name, image in images.items():
        image_path = work_dir / png_name
        tpl_name = png_name.replace(".png", ".tpl")
        tpl_path = work_dir / tpl_name
        spec = TITLE_TPL_SPECS[tpl_name]
        image.save(image_path)
        gxtexconv_to_tpl(image_path, tpl_path, spec["format"], spec["size"][0], spec["size"][1])
        outputs[tpl_name] = tpl_path.read_bytes()
    return outputs


def validate_opening(opening_data: bytes) -> None:
    imet = bytearray(opening_data[:IMET_HEADER_SIZE])
    stored_imet = bytes(imet[IMET_MD5_OFFSET : IMET_MD5_OFFSET + 16])
    imet[IMET_MD5_OFFSET : IMET_MD5_OFFSET + 16] = b"\x00" * 16
    if md5(bytes(imet)) != stored_imet:
        raise ValueError("IMET checksum validation failed")

    outer = opening_data[IMET_HEADER_SIZE:]
    files = extract_file_entries(outer)
    for name in ("banner.bin", "icon.bin", "sound.bin"):
        if name not in files:
            raise ValueError(f"Missing {name} in opening.bnr")
        if name != "sound.bin":
            assert_imd5_payload(files[name])
    if len(opening_data) > MAX_OPENING_BNR_SIZE:
        raise ValueError(f"opening.bnr exceeds 512 KB limit: {len(opening_data)} bytes")


def build_disc_assets(
    template_opening_path: Path,
    target_opening_path: Path,
    target_boot_path: Path,
    logo_path: Path,
    id6: str,
    title: str,
    subtitle: str,
    preview_dir: Path | None,
    asset_dir: Path | None,
) -> dict[str, float | int | str]:
    template_opening = template_opening_path.read_bytes()
    template_header = bytearray(template_opening[:IMET_HEADER_SIZE])
    patch_imet_titles(template_header, title)

    outer_files = extract_file_entries(template_opening[IMET_HEADER_SIZE:])
    banner_inner = decode_imd5_lz77(outer_files["banner.bin"])
    icon_inner = decode_imd5_lz77(outer_files["icon.bin"])

    with tempfile.TemporaryDirectory(prefix="revc_wii_disc_") as temp_dir:
        temp_path = Path(temp_dir)
        generated_images = make_generated_images(logo_path, subtitle)
        if asset_dir is not None and asset_dir.exists():
            generated_images.update(load_custom_images(asset_dir))
        generated_tpls = convert_generated_tpls(generated_images, temp_path)
        sound_candidates = build_sound_candidates(asset_dir, outer_files["sound.bin"], temp_path)

        if preview_dir is not None:
            build_preview_assets(preview_dir, generated_images)

        banner_patched = patch_fixed_size_files(
            banner_inner,
            {
                "banner_bg00.tpl": generated_tpls["banner_bg00.tpl"],
                "title_00.tpl": generated_tpls["title_00.tpl"],
            },
        )
        icon_patched = patch_fixed_size_files(
            icon_inner,
            {
                "icon_bg00.tpl": generated_tpls["icon_bg00.tpl"],
                "title_i00.tpl": generated_tpls["title_i00.tpl"],
            },
        )

        static_files = {
            "banner.bin": rebuild_imd5_blob(banner_patched),
            "icon.bin": rebuild_imd5_blob(icon_patched),
        }

        last_error: Exception | None = None
        chosen_info: dict[str, float | int | str] | None = None
        opening_data: bytes | None = None
        for candidate_label, sound_blob, sound_info in sound_candidates:
            rebuilt_files = {
                "banner.bin": static_files["banner.bin"],
                "icon.bin": static_files["icon.bin"],
                "sound.bin": sound_blob,
            }
            candidate_opening = bytes(template_header) + build_outer_u8(rebuilt_files)
            try:
                validate_opening(candidate_opening)
            except Exception as exc:
                last_error = exc
                continue
            opening_data = candidate_opening
            chosen_info = dict(sound_info)
            chosen_info["strategy"] = candidate_label
            chosen_info["opening_bnr_bytes"] = len(candidate_opening)
            break

        if opening_data is None or chosen_info is None:
            raise RuntimeError(f"Could not fit sound into opening.bnr: {last_error}")

    target_opening_path.parent.mkdir(parents=True, exist_ok=True)
    target_opening_path.write_bytes(opening_data)
    patch_boot_bin(target_boot_path, id6, title)
    return chosen_info


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build Wii disc banner/icon assets for reVC.")
    parser.add_argument("--template-opening", required=True, type=Path)
    parser.add_argument("--target-opening", required=True, type=Path)
    parser.add_argument("--target-boot", required=True, type=Path)
    parser.add_argument("--logo", required=True, type=Path)
    parser.add_argument("--id6", default="REVC02")
    parser.add_argument("--title", default="Grand Theft Auto: Vice City")
    parser.add_argument("--subtitle", default="ROCKSTAR GAMES")
    parser.add_argument("--preview-dir", type=Path)
    parser.add_argument("--asset-dir", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    sound_info = build_disc_assets(
        template_opening_path=args.template_opening,
        target_opening_path=args.target_opening,
        target_boot_path=args.target_boot,
        logo_path=args.logo,
        id6=args.id6,
        title=args.title,
        subtitle=args.subtitle,
        preview_dir=args.preview_dir,
        asset_dir=args.asset_dir,
    )
    print(f"Updated boot.bin: {args.target_boot}")
    print(f"Updated opening.bnr: {args.target_opening}")
    print(f"Disc ID: {args.id6}")
    print(f"Disc title: {args.title}")
    print(f"Sound source: {sound_info['source']}")
    print(f"Sound strategy: {sound_info['strategy']}")
    print(f"Sound format: {sound_info['format']}")
    print(f"Sound packed bytes: {sound_info['packed_bytes']}")
    if "sample_rate" in sound_info:
        print(f"Sound sample rate: {sound_info['sample_rate']} Hz")
    if "channels" in sound_info:
        print(f"Sound channels: {sound_info['channels']}")
    if "seconds" in sound_info:
        print(f"Sound duration: {float(sound_info['seconds']):.3f} s")
    print(f"opening.bnr size: {sound_info['opening_bnr_bytes']} bytes")
    if args.preview_dir:
        print(f"Preview art: {args.preview_dir}")


if __name__ == "__main__":
    main()
