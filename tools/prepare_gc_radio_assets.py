import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(r"F:\GameCube Work")
EXTRACT_DIR = ROOT / "\u63d0\u53d6\u6d4b\u8bd5"
VGAUDIO_CLI = (
    ROOT
    / "tools"
    / "audio"
    / "VGAudio-master"
    / "VGAudioCli"
    / "net451_standalone"
    / "VGAudioCli.exe"
)
SOURCE_WAV_DIR = (
    EXTRACT_DIR
    / "ps2 1.4"
    / "audio"
    / "Music"
    / "GTA PS2 Radio VB to WAV"
    / "work"
    / "new"
)
DEST_DIRS = [
    EXTRACT_DIR / "reVC" / "files" / "Audio" / "Music",
]

# First pass focuses on the music stations where we already have clean
# 32 kHz WAV masters from the PS2 extraction pipeline.
MUSIC_STATIONS = {
    "WILD": "wild.wav",
    "FLASH": "Flash.wav",
    "FEVER": "fever.wav",
    "VROCK": "vrock.wav",
    "ESPANT": "espant.wav",
    "EMOTION": "emotion.wav",
    "WAVE": "wave.wav",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Encode PS2 radio WAV masters to GameCube DSP ADPCM "
            "(IDSP container, stored with a .idsp extension)."
        )
    )
    parser.add_argument(
        "--stations",
        nargs="*",
        default=list(MUSIC_STATIONS),
        help="Station codes to convert. Default: all available music stations.",
    )
    parser.add_argument(
        "--keep-temp",
        action="store_true",
        help="Keep intermediate .idsp files in the temporary directory.",
    )
    return parser.parse_args()


def ensure_prereqs() -> None:
    if not VGAUDIO_CLI.exists():
        raise SystemExit(f"Missing VGAudio CLI: {VGAUDIO_CLI}")
    if not SOURCE_WAV_DIR.exists():
        raise SystemExit(f"Missing WAV source directory: {SOURCE_WAV_DIR}")
    for dest_dir in DEST_DIRS:
        dest_dir.mkdir(parents=True, exist_ok=True)


def convert_station(station: str, wav_name: str, temp_dir: Path) -> Path:
    src = SOURCE_WAV_DIR / wav_name
    if not src.exists():
        raise FileNotFoundError(src)

    temp_idsp = temp_dir / f"{station}.idsp"
    cmd = [str(VGAUDIO_CLI), str(src), str(temp_idsp)]
    print(f"[encode] {station}: {src.name}")
    subprocess.run(cmd, check=True)
    print(f"[done]   {station}: {temp_idsp.stat().st_size} bytes")
    return temp_idsp


def publish_station(station: str, temp_idsp: Path) -> None:
    for dest_dir in DEST_DIRS:
        dest = dest_dir / f"{station}.idsp"
        shutil.copy2(temp_idsp, dest)
        print(f"[copy]   {station}: {dest}")


def main() -> None:
    args = parse_args()
    ensure_prereqs()

    requested = [station.upper() for station in args.stations]
    unknown = [station for station in requested if station not in MUSIC_STATIONS]
    if unknown:
        raise SystemExit(
            "Unknown station code(s): "
            + ", ".join(unknown)
            + ". Available: "
            + ", ".join(MUSIC_STATIONS)
        )

    with tempfile.TemporaryDirectory(prefix="gc_radio_") as temp_root:
        temp_dir = Path(temp_root)
        for station in requested:
            temp_idsp = convert_station(station, MUSIC_STATIONS[station], temp_dir)
            publish_station(station, temp_idsp)

        if args.keep_temp:
            keep_dir = ROOT / "_tmp_gc_radio_idsp"
            keep_dir.mkdir(parents=True, exist_ok=True)
            for temp_idsp in temp_dir.glob("*.idsp"):
                shutil.copy2(temp_idsp, keep_dir / temp_idsp.name)
            print(f"[keep]   copied temporary IDSP files to {keep_dir}")


if __name__ == "__main__":
    main()
