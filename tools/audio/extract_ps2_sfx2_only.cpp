#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../../src/audio/Ps2SfxFormat.h"

namespace {

struct SdtRecord {
    uint32_t rawOffset;
    uint32_t allocatedBytes;
    uint32_t sampleRate;
};

class VagDecoder {
    static constexpr double kCoefficients[5][2] = {
        {0.0, 0.0},
        {60.0 / 64.0, 0.0},
        {115.0 / 64.0, -52.0 / 64.0},
        {98.0 / 64.0, -55.0 / 64.0},
        {122.0 / 64.0, -60.0 / 64.0},
    };

    double history1_ = 0.0;
    double history2_ = 0.0;

    static int16_t Quantize(double sample) {
        int32_t value = static_cast<int32_t>(sample + 0.5);
        if (value < -32768)
            value = -32768;
        else if (value > 32767)
            value = 32767;
        return static_cast<int16_t>(value);
    }

public:
    void DecodeFrame(const uint8_t* frame, int16_t* output) {
        const uint8_t predictorAndShift = frame[0];
        const uint32_t predictor = predictorAndShift >> 4;
        const uint32_t shift = predictorAndShift & 0x0F;
        double samples[PS2_SFX_VAG_SAMPLES_PER_FRAME];

        for (uint32_t i = 0; i < PS2_SFX_VAG_SAMPLES_PER_FRAME; i += 2) {
            const uint8_t packed = frame[2 + i / 2];
            int16_t sample = static_cast<int16_t>((packed & 0x0F) << 12);
            samples[i] = static_cast<double>(sample >> shift);
            sample = static_cast<int16_t>((packed & 0xF0) << 8);
            samples[i + 1] = static_cast<double>(sample >> shift);
        }

        for (uint32_t i = 0; i < PS2_SFX_VAG_SAMPLES_PER_FRAME; ++i) {
            samples[i] += history1_ * kCoefficients[predictor][0] +
                          history2_ * kCoefficients[predictor][1];
            history2_ = history1_;
            history1_ = samples[i];
            output[i] = Quantize(samples[i] + 0.5);
        }
    }
};

constexpr double VagDecoder::kCoefficients[5][2];

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::vector<uint8_t> ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        Fail("Unable to open '" + path.string() + "'");

    const std::streamsize size = input.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > std::numeric_limits<size_t>::max())
        Fail("Invalid file size for '" + path.string() + "'");
    input.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!input || input.gcount() != size)
            Fail("Short read from '" + path.string() + "'");
    }
    return bytes;
}

SdtRecord ReadSdtRecord(const std::vector<uint8_t>& sdt, uint32_t entry) {
    const uint64_t offset = static_cast<uint64_t>(entry) * PS2_SFX_SDT_RECORD_SIZE;
    if (offset + PS2_SFX_SDT_RECORD_SIZE > sdt.size())
        Fail("SDT entry " + std::to_string(entry) + " is out of range");

    const uint8_t* bytes = sdt.data() + static_cast<size_t>(offset);
    return {
        ReadPs2SfxLe32(bytes),
        ReadPs2SfxLe32(bytes + 4),
        ReadPs2SfxLe32(bytes + 8),
    };
}

void WriteLe16(std::ostream& output, uint16_t value) {
    const uint8_t bytes[] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
    };
    output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void WriteLe32(std::ostream& output, uint32_t value) {
    const uint8_t bytes[] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24),
    };
    output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void WriteMonoPcmWav(const std::filesystem::path& path,
                     uint32_t sampleRate,
                     const std::vector<int16_t>& samples) {
    const uint64_t dataSize64 = static_cast<uint64_t>(samples.size()) * sizeof(int16_t);
    if (dataSize64 > std::numeric_limits<uint32_t>::max())
        Fail("Decoded PCM is too large for WAV: '" + path.string() + "'");
    const uint32_t dataSize = static_cast<uint32_t>(dataSize64);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        Fail("Unable to create '" + path.string() + "'");

    output.write("RIFF", 4);
    WriteLe32(output, 36U + dataSize);
    output.write("WAVEfmt ", 8);
    WriteLe32(output, 16);
    WriteLe16(output, 1);
    WriteLe16(output, 1);
    WriteLe32(output, sampleRate);
    WriteLe32(output, sampleRate * sizeof(int16_t));
    WriteLe16(output, sizeof(int16_t));
    WriteLe16(output, 16);
    output.write("data", 4);
    WriteLe32(output, dataSize);
    for (int16_t sample : samples)
        WriteLe16(output, static_cast<uint16_t>(sample));

    if (!output)
        Fail("Failed while writing '" + path.string() + "'");
}

std::vector<int16_t> DecodeRecord(uint32_t entry,
                                  const SdtRecord& record,
                                  uint64_t rawSize,
                                  std::ifstream& raw) {
    if (record.allocatedBytes == 0 || record.sampleRate == 0 ||
        (record.rawOffset % PS2_SFX_VAG_FRAME_SIZE) != 0 ||
        (record.allocatedBytes % PS2_SFX_VAG_FRAME_SIZE) != 0 ||
        static_cast<uint64_t>(record.rawOffset) + record.allocatedBytes > rawSize) {
        Fail("Invalid SDT metadata for entry " + std::to_string(entry));
    }

    std::vector<uint8_t> encoded(record.allocatedBytes);
    raw.clear();
    raw.seekg(record.rawOffset, std::ios::beg);
    raw.read(reinterpret_cast<char*>(encoded.data()), encoded.size());
    if (!raw || raw.gcount() != static_cast<std::streamsize>(encoded.size()))
        Fail("Short RAW read for entry " + std::to_string(entry));

    VagDecoder decoder;
    std::vector<int16_t> pcm;
    pcm.reserve((record.allocatedBytes / PS2_SFX_VAG_FRAME_SIZE) *
                PS2_SFX_VAG_SAMPLES_PER_FRAME);
    int16_t decoded[PS2_SFX_VAG_SAMPLES_PER_FRAME];
    bool terminalFound = false;

    for (uint32_t offset = 0; offset < record.allocatedBytes;
         offset += PS2_SFX_VAG_FRAME_SIZE) {
        const uint8_t* frame = encoded.data() + offset;
        if (!IsSupportedPs2MissionVagFlags(frame[1]) ||
            !IsSupportedPs2VagPredictor(frame[0])) {
            Fail("Unsupported VAG frame in entry " + std::to_string(entry));
        }
        decoder.DecodeFrame(frame, decoded);
        pcm.insert(pcm.end(), decoded,
                   decoded + PS2_SFX_VAG_SAMPLES_PER_FRAME);
        if (IsPs2VagEndFrame(frame[1])) {
            terminalFound = true;
            break;
        }
    }

    if (!terminalFound)
        Fail("No terminal VAG frame in entry " + std::to_string(entry));
    return pcm;
}

const char* NeighborGroup(uint32_t entry) {
    if (entry <= 9538)
        return entry == 9536 ? "BNK4_3M_to_BNK4_3O" : "BNK4_3O_to_BNK4_3P";
    if (entry <= 9612)
        return "BNK4_51_to_BNK4_94";
    if (entry <= 10067)
        return "ROK1_1B_to_ROK1_5";
    if (entry <= 10422)
        return "BJM1_5_to_MERC_39";
    return "MERC_39_to_MONO_1";
}

const char* ConfirmedCharacter(uint32_t entry) {
    return entry < 10061 ? "Hilary" : "Mercedes";
}

const char* ContentGroup(uint32_t entry) {
    if (entry == 9536)
        return "hilary_9536";
    if (entry == 9538)
        return "hilary_9538";
    if (entry <= 9612)
        return "hilary_repeat_9535_9537";
    return "mercedes_repeat_10061";
}

std::string OutputFileName(uint32_t entry) {
    std::ostringstream name;
    name << "sfx2_" << std::setfill('0') << std::setw(5) << entry
         << "_ps2_only.wav";
    return name.str();
}

std::string BustLabel(uint32_t ordinal) {
    std::ostringstream label;
    label << "BUST_" << std::setfill('0') << std::setw(2) << ordinal + 1;
    return label.str();
}

std::string BustOutputFileName(uint32_t entry, uint32_t ordinal) {
    std::ostringstream name;
    name << "sfx2_" << std::setfill('0') << std::setw(5) << entry << '_'
         << BustLabel(ordinal) << ".wav";
    return name.str();
}

void PrintUsage(const char* argv0) {
    std::cout << "Usage:\n  \"" << argv0
              << "\" <sfx2.SDT> <sfx2.RAW> <output-directory> [--bust]\n"
              << "\nWithout --bust, extracts the 25 extra Hilary/Mercedes records.\n"
              << "With --bust, extracts BUST_01..BUST_28.\n";
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
        if (argc < 4 || argc > 5) {
            PrintUsage(argv[0]);
            return 1;
        }

        const bool extractBust = argc == 5 && std::string_view(argv[4]) == "--bust";
        if (argc == 5 && !extractBust)
            Fail("Unknown extraction mode '" + std::string(argv[4]) + "'");

        static_assert(sizeof(Ps2SfxMissionSkippedSdtEntries) /
                          sizeof(Ps2SfxMissionSkippedSdtEntries[0]) ==
                      25,
                      "PS2 extra-record extraction list changed");

        const std::filesystem::path sdtPath = argv[1];
        const std::filesystem::path rawPath = argv[2];
        const std::filesystem::path outputDirectory = argv[3];
        const std::vector<uint8_t> sdt = ReadWholeFile(sdtPath);

        std::error_code error;
        const uint64_t rawSize = std::filesystem::file_size(rawPath, error);
        if (error)
            Fail("Unable to read RAW size: " + error.message());
        std::filesystem::create_directories(outputDirectory, error);
        if (error)
            Fail("Unable to create output directory: " + error.message());

        std::ifstream raw(rawPath, std::ios::binary);
        if (!raw)
            Fail("Unable to open '" + rawPath.string() + "'");

        std::ofstream manifest(outputDirectory / "manifest.csv",
                               std::ios::trunc);
        if (!manifest)
            Fail("Unable to create manifest.csv");
        manifest << "entry,filename,label,confirmed_character,content_group,"
                    "neighbor_group,raw_offset,allocated_bytes,sample_rate,"
                    "decoded_samples,duration_ms\n";

        uint32_t extracted = 0;
        auto extractRecord = [&](uint32_t entry, const std::string& fileName,
                                 const char* label, const char* character,
                                 const char* contentGroup,
                                 const char* neighborGroup) {
            const SdtRecord record = ReadSdtRecord(sdt, entry);
            const std::vector<int16_t> pcm =
                DecodeRecord(entry, record, rawSize, raw);
            WriteMonoPcmWav(outputDirectory / fileName,
                            record.sampleRate, pcm);

            const double durationMs =
                static_cast<double>(pcm.size()) * 1000.0 / record.sampleRate;
            manifest << entry << ',' << fileName << ',' << label << ','
                     << character << ',' << contentGroup << ',' << neighborGroup
                     << ',' << record.rawOffset << ',' << record.allocatedBytes
                     << ',' << record.sampleRate << ',' << pcm.size() << ',' << std::fixed
                     << std::setprecision(3) << durationMs << '\n';
            std::cout << entry << " -> " << fileName << " ("
                      << record.sampleRate << " Hz, " << std::fixed
                      << std::setprecision(1) << durationMs << " ms)\n";
            ++extracted;
        };

        if (extractBust) {
            for (uint32_t ordinal = 0; ordinal < PS2_SFX_BUST_ENTRY_COUNT;
                 ++ordinal) {
                const uint32_t entry = PS2_SFX_BUST_FIRST_SDT_ENTRY + ordinal;
                const std::string label = BustLabel(ordinal);
                extractRecord(entry, BustOutputFileName(entry, ordinal),
                              label.c_str(), "", "busted_dialogue",
                              "post_arrest_respawn");
            }
        } else {
            for (uint16_t entry16 : Ps2SfxMissionSkippedSdtEntries) {
                const uint32_t entry = entry16;
                extractRecord(entry, OutputFileName(entry), "",
                              ConfirmedCharacter(entry), ContentGroup(entry),
                              NeighborGroup(entry));
            }
        }

        if (!manifest)
            Fail("Failed while writing manifest.csv");
        std::cout << "Extracted " << extracted
                  << (extractBust ? " BUST records to '" : " extra PS2 records to '")
                  << outputDirectory.string() << "'.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Extraction failed: " << exception.what() << '\n';
        return 1;
    }
}
