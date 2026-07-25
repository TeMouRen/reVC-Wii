#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../../src/audio/Ps2SfxFormat.h"

namespace {

constexpr uint32_t kSdtRecordSize = PS2_SFX_SDT_RECORD_SIZE;
constexpr uint32_t kVagFrameSize = PS2_SFX_VAG_FRAME_SIZE;
constexpr uint32_t kSamplesPerVagFrame = PS2_SFX_VAG_SAMPLES_PER_FRAME;
constexpr uint32_t kFirstMissionTrackId = PS2_SFX_MISSION_FIRST_TRACK_ID;
constexpr uint32_t kLastMissionTrackId = PS2_SFX_MISSION_LAST_TRACK_ID;
constexpr uint32_t kFirstMissionEntry = PS2_SFX_MISSION_FIRST_SDT_ENTRY;
constexpr uint32_t kLastMissionEntry = PS2_SFX_MISSION_LAST_SDT_ENTRY;
constexpr uint32_t kMissionEntryCount = PS2_SFX_MISSION_ENTRY_COUNT;
constexpr uint32_t kCompiledMissionTrackCount = kLastMissionTrackId - kFirstMissionTrackId + 1;
constexpr uint32_t kFirstBustedTrackId = PS2_SFX_BUST_FIRST_TRACK_ID;
constexpr uint32_t kLastBustedTrackId = PS2_SFX_BUST_LAST_TRACK_ID;
constexpr uint32_t kFirstBustedEntry = PS2_SFX_BUST_FIRST_SDT_ENTRY;
constexpr uint32_t kLastBustedEntry = PS2_SFX_BUST_LAST_SDT_ENTRY;
constexpr uint32_t kBustedEntryCount = PS2_SFX_BUST_ENTRY_COUNT;

static_assert(kCompiledMissionTrackCount == 1121, "Mission track range must cover 1121 entries");
static_assert(kMissionEntryCount == 1121, "Mission SDT range must cover 1121 entries");
static_assert(kBustedEntryCount == 28, "Busted dialogue range must cover 28 entries");

struct SdtRecord {
    uint32_t rawOffset = 0;
    uint32_t allocatedBytes = 0;
    uint32_t sampleRate = 0;
};

struct MissionValidationResult {
    uint32_t totalFrames = 0;
    uint32_t endFrameIndex = 0;
    uint64_t actualSamples = 0;
};

std::string Hex32(uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

uint64_t FileSizeOrFail(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        Fail("Unable to read file size for '" + path.string() + "': " + error.message());
    }
    return size;
}

std::vector<uint8_t> ReadWholeFileOrFail(const std::filesystem::path& path) {
    const uint64_t size = FileSizeOrFail(path);
    if (size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        Fail("File is too large to load into memory: '" + path.string() + "'");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        Fail("Unable to open '" + path.string() + "' for reading");
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
            Fail("Short read while loading '" + path.string() + "'");
        }
    }

    return bytes;
}

std::vector<SdtRecord> ParseSdtRecords(const std::vector<uint8_t>& sdtBytes) {
    if ((sdtBytes.size() % kSdtRecordSize) != 0) {
        Fail("SDT size is not divisible by 12 bytes: " + std::to_string(sdtBytes.size()));
    }

    const size_t recordCount = sdtBytes.size() / kSdtRecordSize;
    if (recordCount < (static_cast<size_t>(kLastMissionEntry) + 1)) {
        Fail("SDT only contains " + std::to_string(recordCount) +
             " records; at least " + std::to_string(kLastMissionEntry + 1) + " are required");
    }

    std::vector<SdtRecord> records;
    records.reserve(recordCount);
    for (size_t i = 0; i < recordCount; ++i) {
        const uint8_t* recordBytes = sdtBytes.data() + (i * kSdtRecordSize);
        records.push_back(SdtRecord{
            ReadPs2SfxLe32(recordBytes + 0),
            ReadPs2SfxLe32(recordBytes + 4),
            ReadPs2SfxLe32(recordBytes + 8),
        });
    }
    return records;
}

MissionValidationResult ValidateMissionRecord(uint32_t entryIndex,
                                              const SdtRecord& record,
                                              uint64_t rawSize,
                                              std::ifstream& rawStream) {
    if (record.allocatedBytes == 0) {
        Fail("Mission entry " + std::to_string(entryIndex) + " has zero allocated bytes");
    }
    if ((record.rawOffset % kVagFrameSize) != 0) {
        Fail("Mission entry " + std::to_string(entryIndex) + " has unaligned RAW offset " +
             Hex32(record.rawOffset));
    }
    if ((record.allocatedBytes % kVagFrameSize) != 0) {
        Fail("Mission entry " + std::to_string(entryIndex) + " has unaligned size " +
             std::to_string(record.allocatedBytes));
    }
    if (record.sampleRate == 0) {
        Fail("Mission entry " + std::to_string(entryIndex) + " has a zero sample rate");
    }

    const uint64_t sliceEnd = static_cast<uint64_t>(record.rawOffset) + record.allocatedBytes;
    if (sliceEnd > rawSize) {
        Fail("Mission entry " + std::to_string(entryIndex) + " exceeds RAW bounds: offset=" +
             Hex32(record.rawOffset) + " size=" + std::to_string(record.allocatedBytes) +
             " rawSize=" + std::to_string(rawSize));
    }

    std::vector<uint8_t> slice(record.allocatedBytes);
    rawStream.clear();
    rawStream.seekg(static_cast<std::streamoff>(record.rawOffset), std::ios::beg);
    if (!rawStream) {
        Fail("Seek failed for mission entry " + std::to_string(entryIndex));
    }
    rawStream.read(reinterpret_cast<char*>(slice.data()), static_cast<std::streamsize>(slice.size()));
    if (!rawStream || rawStream.gcount() != static_cast<std::streamsize>(slice.size())) {
        Fail("Short read while scanning mission entry " + std::to_string(entryIndex));
    }

    const uint32_t totalFrames = record.allocatedBytes / kVagFrameSize;
    bool terminalFound = false;
    uint32_t endFrameIndex = 0;

    for (uint32_t frameIndex = 0; frameIndex < totalFrames; ++frameIndex) {
        const uint8_t predictorAndShift = slice[frameIndex * kVagFrameSize];
        const uint8_t flags = slice[(frameIndex * kVagFrameSize) + 1];
        if (!IsSupportedPs2VagPredictor(predictorAndShift)) {
            Fail("Mission entry " + std::to_string(entryIndex) + " frame " +
                 std::to_string(frameIndex) + " uses unsupported predictor " +
                 std::to_string(predictorAndShift >> 4));
        }
        if (!IsSupportedPs2MissionVagFlags(flags)) {
            Fail("Mission entry " + std::to_string(entryIndex) + " frame " +
                 std::to_string(frameIndex) + " uses unsupported flags value " +
                 std::to_string(flags));
        }
        if (terminalFound) {
            if (!IsPs2VagPostEndPadding(flags)) {
                Fail("Mission entry " + std::to_string(entryIndex) + " frame " +
                     std::to_string(frameIndex) + " uses non-padding flags " +
                     std::to_string(flags) + " after the terminal frame");
            }
        } else if (IsPs2VagEndFrame(flags)) {
            terminalFound = true;
            endFrameIndex = frameIndex;
        }
    }

    if (!terminalFound) {
        Fail("Mission entry " + std::to_string(entryIndex) + " has no terminal frame");
    }

    return MissionValidationResult{
        totalFrames,
        endFrameIndex,
        static_cast<uint64_t>(endFrameIndex + 1) * kSamplesPerVagFrame,
    };
}

void PrintUsage(const char* argv0) {
    std::cout
        << "Usage:\n"
        << "  \"" << argv0 << "\" <path-to-sfx2.SDT> <path-to-sfx2.RAW>\n\n"
        << "Validates the PS2 sfx2 mission dialogue bank in read-only mode.\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const bool helpRequested =
            argc >= 2 && (std::string_view(argv[1]) == "--help" ||
                          std::string_view(argv[1]) == "-h");
        if (helpRequested) {
            PrintUsage(argv[0]);
            return 0;
        }
        if (argc != 3) {
            PrintUsage(argv[0]);
            return 1;
        }

        const std::filesystem::path sdtPath = argv[1];
        const std::filesystem::path rawPath = argv[2];

        const std::vector<uint8_t> sdtBytes = ReadWholeFileOrFail(sdtPath);
        const std::vector<SdtRecord> records = ParseSdtRecords(sdtBytes);
        const uint64_t rawSize = FileSizeOrFail(rawPath);

        std::ifstream rawStream(rawPath, std::ios::binary);
        if (!rawStream) {
            Fail("Unable to open '" + rawPath.string() + "' for reading");
        }

        std::cout << "Validating " << kMissionEntryCount << " mapped mission records in "
                  << kFirstMissionEntry << ".." << kLastMissionEntry
                  << " against '" << rawPath.string() << "'\n";
        std::cout << "Shared Ps2SfxFormat.h: yes\n";

        if (Ps2SfxMissionOrdinalToSdtEntry(0) != 9417U ||
            Ps2SfxMissionOrdinalToSdtEntry(334) != 9757U ||
            Ps2SfxMissionOrdinalToSdtEntry(335) != 9758U ||
            Ps2SfxMissionOrdinalToSdtEntry(679) != 10109U ||
            Ps2SfxMissionOrdinalToSdtEntry(944) != 10374U ||
            Ps2SfxMissionOrdinalToSdtEntry(950) != 10380U ||
            Ps2SfxMissionOrdinalToSdtEntry(kMissionEntryCount - 1) != 10562U) {
            Fail("Known PS2 mission SDT mapping anchors changed");
        }

        std::map<uint32_t, uint32_t> sampleRateCounts;
        uint32_t bustedRecordsValidated = 0;
        uint32_t previousEntry = PS2_SFX_INVALID_SDT_ENTRY;
        for (uint32_t ordinal = 0; ordinal < kMissionEntryCount; ++ordinal) {
            const uint32_t entry = Ps2SfxMissionOrdinalToSdtEntry(ordinal);
            if (entry == PS2_SFX_INVALID_SDT_ENTRY) {
                Fail("Missing SDT mapping for mission ordinal " + std::to_string(ordinal));
            }
            if (previousEntry != PS2_SFX_INVALID_SDT_ENTRY && entry <= previousEntry) {
                Fail("Mission SDT mapping is not strictly increasing at ordinal " +
                     std::to_string(ordinal));
            }
            previousEntry = entry;
            const MissionValidationResult result =
                ValidateMissionRecord(entry, records[entry], rawSize, rawStream);
            ++sampleRateCounts[records[entry].sampleRate];

            const uint32_t trackId = kFirstMissionTrackId + ordinal;
            if (trackId >= kFirstBustedTrackId && trackId <= kLastBustedTrackId) {
                const uint32_t bustedOrdinal = trackId - kFirstBustedTrackId;
                const uint32_t expectedEntry = kFirstBustedEntry + bustedOrdinal;
                if (entry != expectedEntry) {
                    Fail("BUST_" + std::to_string(bustedOrdinal + 1) +
                         " maps to SDT entry " + std::to_string(entry) +
                         " instead of " + std::to_string(expectedEntry));
                }
                ++bustedRecordsValidated;
            }

            if (ordinal == 0 || ordinal == kMissionEntryCount - 1 ||
                ordinal == 334 || ordinal == 335 || ordinal == 679 ||
                ordinal == 944 || ordinal == 950) {
                std::cout << "Mapping " << trackId << " -> " << entry
                          << ", rate=" << records[entry].sampleRate
                          << ", frames=" << result.totalFrames
                          << ", endFrame=" << result.endFrameIndex
                          << ", actualSamples=" << result.actualSamples << "\n";
            }
        }

        if (bustedRecordsValidated != kBustedEntryCount) {
            Fail("Validated " + std::to_string(bustedRecordsValidated) +
                 " busted dialogue records instead of " + std::to_string(kBustedEntryCount));
        }

        std::cout << "Mission track count: " << kCompiledMissionTrackCount << "\n";
        std::cout << "Mission entry count: " << kMissionEntryCount << "\n";
        std::cout << "SDT records: " << records.size() << "\n";
        std::cout << "RAW size: " << rawSize << " bytes\n";
        std::cout << "Sample rates:";
        for (const auto& [sampleRate, count] : sampleRateCounts) {
            std::cout << " " << sampleRate << "Hz=" << count;
        }
        std::cout << "\n";
        std::cout << "First mapping: MOBR1 -> " << kFirstMissionEntry << "\n";
        std::cout << "Last mapping: BUST_28 -> " << kLastMissionEntry << "\n";
        std::cout << "Busted dialogue: BUST_01..BUST_28 -> "
                  << kFirstBustedEntry << ".." << kLastBustedEntry
                  << " (" << bustedRecordsValidated << " records validated)\n";
        std::cout << "Validation OK: read-only checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Validation failed: " << error.what() << "\n";
        return 1;
    }
}
