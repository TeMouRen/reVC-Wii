# Audio Tools

## `validate_idsp.cpp`

Read-only offline validator for Wii/GameCube `IDSP` streams.

Checks performed:

- IDSP magic, header size, channel count, sample rate, and sample count.
- 8-byte GC ADPCM frame alignment and per-channel metadata bounds.
- Per-channel coefficient and history blocks.
- Deterministic PCM decode with a CRC32 over little-endian 16-bit samples.
- Optional sample dumps for the first few decoded frames.

### Build on Windows

Build it from the repository root with a C++17-capable MSVC toolchain.

```powershell
cl /std:c++17 /EHsc /W4 /Fe:tools\audio\validate_idsp.exe tools\audio\validate_idsp.cpp
```

### Run on Windows

```powershell
.\tools\audio\validate_idsp.exe --dump-samples 3 .\tools\audio\fixtures\minimal_mono.idsp
.\tools\audio\validate_idsp.exe "<game-files>\Audio\CUTSCENE\ASS\ASS_1.idsp"
```

## Wii runtime backend status

- Wii IDSP streams use the dedicated DSP decode task, with the CPU path kept
  for validation and recovery when the DSP task is unavailable.
- The PS2 `Audio/SFX/SetN/sfx.SDT` + `sfx.RAW` layout is the currently supported
  Wii SFX backend. Mission and pedestrian dialogue use the PS2 `sfx2` bank.
- PC `Audio/SFX.SDT` detection and parsing remain in place, but the accompanying
  PC WAV/resource path is not complete yet.

## `validate_ps2_sfx2.cpp`

Read-only offline validator for the PS2 `sfx2.SDT` + `sfx2.RAW` mission dialogue bank.

Checks performed:

- SDT record size is 12 bytes and the file contains at least 10563 records.
- The 1121 mission tracks map into `9417..10562`, skipping the 25 extra records defined in `Ps2SfxFormat.h`.
- The 28 post-arrest `BUST_01..BUST_28` dialogue tracks map contiguously into `10535..10562`.
- Each mission record stays within RAW bounds.
- Every mission entry uses 16-byte aligned offsets and allocated sizes.
- Mission entries may use their native PS2 sample rates instead of being forced to 12 kHz.
- Every mission slice contains a terminal VAG frame; post-terminal frame flags may only be 0 or 7. Their unused payload bytes are not interpreted.
- The validator never extracts or rewrites audio.

### Build on Windows

The tool is standalone and is not part of the normal game build. Build it from the repository root with any C++17-capable compiler.

MSVC Developer PowerShell:

```powershell
cl /std:c++17 /EHsc /W4 /Fe:tools\audio\validate_ps2_sfx2.exe tools\audio\validate_ps2_sfx2.cpp
```

MinGW or LLVM:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -pedantic -o tools/audio/validate_ps2_sfx2.exe tools/audio/validate_ps2_sfx2.cpp
```

### Run on Windows

Quote both paths because the workspace or asset directories may contain spaces.

```powershell
.\tools\audio\validate_ps2_sfx2.exe "F:\Dumped PS2 Assets\Audio\SFX\Set0\sfx2.SDT" "F:\Dumped PS2 Assets\Audio\SFX\Set0\sfx2.RAW"
```

Expected success output includes:

- `First mapping: MOBR1 -> 9417`
- `Last mapping: BUST_28 -> 10562`
- `Busted dialogue: BUST_01..BUST_28 -> 10535..10562 (28 records validated)`
- `Validation OK: read-only checks passed.`

## `extract_ps2_sfx2_only.cpp`

Extracts the 25 extra PS2 records skipped by the PC mission-audio enum into
standard mono 16-bit PCM WAV files. It stops at each record's native VAG end
frame, preserves the SDT sample rate, and writes `manifest.csv` with offsets,
durations, confirmed characters, content groups, and neighboring known
mission-audio groups.

These records are separate from `BUST_01..BUST_28`. Because the SDT records do
not contain filename or character labels, the extractor deliberately keeps the
SDT entry number in each WAV filename; neighboring labels in the manifest are
context for manual identification, not names assigned to the extracted audio.

Manual listening identifies entries `9536`, `9538`, and `9609..9612` as Hilary;
all remaining extra entries are Mercedes. A full-bank encoded-data comparison
also shows that `9609..9612` repeat mapped entries `9535` and `9537`, while the
19 Mercedes entries all repeat the payload anchored at entry `10061`. These are
extra SDT records, not 25 unique PS2-only dialogue lines.

Build it with the same host C++17 compiler used for the validators:

```powershell
cl /std:c++17 /EHsc /W4 /Fe:tools\audio\extract_ps2_sfx2_only.exe tools\audio\extract_ps2_sfx2_only.cpp
```

Run it with a new or existing output directory:

```powershell
.\tools\audio\extract_ps2_sfx2_only.exe <sfx2.SDT> <sfx2.RAW> <output-directory>
```

To extract the actual post-arrest dialogue selected by `GameLogic.cpp`, use
`--bust`. This writes the mapped records `10535..10562` as
`BUST_01..BUST_28` instead of extracting the unrelated extra records.

```powershell
.\tools\audio\extract_ps2_sfx2_only.exe <sfx2.SDT> <sfx2.RAW> <output-directory> --bust
```
