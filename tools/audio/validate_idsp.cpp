#include <algorithm>
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

namespace {

constexpr std::uint32_t kIdspMagic = 0x49445350U; // "IDSP"
constexpr std::uint32_t kIdspHeaderSize = 0x100U;
constexpr std::uint32_t kIdspChannelInfoSize = 0x60U;
constexpr std::uint32_t kIdspStreamInfoSize = 0x40U;
constexpr std::uint32_t kGcAdpcmFrameSize = 8U;
constexpr std::uint32_t kGcAdpcmSamplesPerFrame = 14U;
constexpr std::uint32_t kMaxChannels = 2U;

struct Header {
    std::uint32_t channelCount = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t sampleCount = 0;
    std::uint32_t loopStart = 0;
    std::uint32_t loopEnd = 0;
    std::uint32_t interleaveSize = 0;
    std::uint32_t streamInfoSize = 0;
    std::uint32_t channelInfoSize = 0;
    std::uint32_t headerSize = 0;
    std::uint32_t audioDataSize = 0;
};

struct ChannelInfo {
    std::uint32_t sampleCount = 0;
    std::uint32_t nibbleCount = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t looping = 0;
    std::uint16_t padding = 0;
    std::uint32_t loopStartAddress = 0;
    std::uint32_t loopEndAddress = 0;
    std::uint32_t currentAddress = 0;
    std::int16_t coefs[16] = {};
    std::uint16_t gain = 0;
    std::uint16_t startPredScale = 0;
    std::int16_t initialHist1 = 0;
    std::int16_t initialHist2 = 0;
    std::uint16_t loopPredScale = 0;
    std::int16_t loopHist1 = 0;
    std::int16_t loopHist2 = 0;
};

struct Hist {
    std::int16_t hist1 = 0;
    std::int16_t hist2 = 0;
};

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::uint16_t ReadBE16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

std::uint32_t ReadBE32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::string Hex32(std::uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return stream.str();
}

std::uint64_t FileSizeOrFail(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        Fail("Unable to read file size for '" + path.string() + "': " + ec.message());
    }
    return size;
}

std::vector<std::uint8_t> ReadWholeFileOrFail(const std::filesystem::path& path) {
    const std::uint64_t size = FileSizeOrFail(path);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        Fail("File is too large to load into memory: '" + path.string() + "'");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        Fail("Unable to open '" + path.string() + "' for reading");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
            Fail("Short read while loading '" + path.string() + "'");
        }
    }
    return bytes;
}

std::uint8_t SignNibble(std::uint8_t value) {
    return (value & 0x8U) ? static_cast<std::uint8_t>(value | 0xF0U) : static_cast<std::uint8_t>(value & 0x0FU);
}

std::int16_t ClampToInt16(std::int32_t value) {
    if (value < -32768) {
        return -32768;
    }
    if (value > 32767) {
        return 32767;
    }
    return static_cast<std::int16_t>(value);
}

std::uint32_t GcByteCountToSampleCount(std::uint32_t byteCount) {
    const std::uint32_t nibbleCount = byteCount * 2U;
    const std::uint32_t frames = nibbleCount / 16U;
    const std::uint32_t extraNibbles = nibbleCount % 16U;
    const std::uint32_t extraSamples = extraNibbles < 2U ? 0U : extraNibbles - 2U;
    return frames * kGcAdpcmSamplesPerFrame + extraSamples;
}

void DecodeGcAdpcmFrame(const std::uint8_t* frame,
                        const std::int16_t* coefs,
                        Hist& hist,
                        std::int16_t* outbuf,
                        std::uint32_t sampleCount) {
    const std::uint8_t predictorScale = frame[0];
    const int scale = (1 << (predictorScale & 0x0F)) * 2048;
    const int predictor = predictorScale >> 4;
    const std::int16_t coef1 = coefs[predictor * 2 + 0];
    const std::int16_t coef2 = coefs[predictor * 2 + 1];

    for (std::uint32_t i = 0; i < sampleCount; ++i) {
        const std::uint8_t byte = frame[1 + (i >> 1)];
        const int adpcmSample = (i & 1U) == 0U ? SignNibble(byte >> 4) : SignNibble(byte & 0x0FU);
        const int distance = scale * adpcmSample;
        const int predicted = coef1 * hist.hist1 + coef2 * hist.hist2;
        const int corrected = predicted + distance;
        const int scaled = (corrected + 1024) >> 11;
        const std::int16_t decoded = ClampToInt16(scaled);

        hist.hist2 = hist.hist1;
        hist.hist1 = decoded;
        outbuf[i] = decoded;
    }
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const bool lsb = (crc & 1U) != 0U;
            crc >>= 1U;
            if (lsb) {
                crc ^= 0xEDB88320U;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

void AppendPcm16Le(std::vector<std::uint8_t>& bytes, std::int16_t sample) {
    const auto value = static_cast<std::uint16_t>(sample);
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

Header ParseHeader(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kIdspHeaderSize) {
        Fail("File is too small to be an IDSP stream");
    }
    if (ReadBE32(bytes.data()) != kIdspMagic) {
        Fail("Missing IDSP magic");
    }

    Header header;
    header.channelCount = ReadBE32(bytes.data() + 0x08);
    header.sampleRate = ReadBE32(bytes.data() + 0x0C);
    header.sampleCount = ReadBE32(bytes.data() + 0x10);
    header.loopStart = ReadBE32(bytes.data() + 0x14);
    header.loopEnd = ReadBE32(bytes.data() + 0x18);
    header.interleaveSize = ReadBE32(bytes.data() + 0x1C);
    header.streamInfoSize = ReadBE32(bytes.data() + 0x20);
    header.channelInfoSize = ReadBE32(bytes.data() + 0x24);
    header.headerSize = ReadBE32(bytes.data() + 0x28);
    header.audioDataSize = ReadBE32(bytes.data() + 0x2C);
    return header;
}

ChannelInfo ParseChannelInfo(const std::vector<std::uint8_t>& bytes, std::uint32_t channelIndex) {
    const std::size_t offset = 0x40U + static_cast<std::size_t>(channelIndex) * kIdspChannelInfoSize;
    if (offset + kIdspChannelInfoSize > bytes.size()) {
        Fail("Truncated channel metadata for channel " + std::to_string(channelIndex));
    }

    ChannelInfo info;
    const std::uint8_t* p = bytes.data() + offset;
    info.sampleCount = ReadBE32(p + 0x00);
    info.nibbleCount = ReadBE32(p + 0x04);
    info.sampleRate = ReadBE32(p + 0x08);
    info.looping = ReadBE16(p + 0x0C);
    info.padding = ReadBE16(p + 0x0E);
    info.loopStartAddress = ReadBE32(p + 0x10);
    info.loopEndAddress = ReadBE32(p + 0x14);
    info.currentAddress = ReadBE32(p + 0x18);
    for (int i = 0; i < 16; ++i) {
        info.coefs[i] = static_cast<std::int16_t>(ReadBE16(p + 0x1C + (i * 2)));
    }
    info.gain = ReadBE16(p + 0x3C);
    info.startPredScale = ReadBE16(p + 0x3E);
    info.initialHist1 = static_cast<std::int16_t>(ReadBE16(p + 0x40));
    info.initialHist2 = static_cast<std::int16_t>(ReadBE16(p + 0x42));
    info.loopPredScale = ReadBE16(p + 0x44);
    info.loopHist1 = static_cast<std::int16_t>(ReadBE16(p + 0x46));
    info.loopHist2 = static_cast<std::int16_t>(ReadBE16(p + 0x48));
    return info;
}

void ValidateStructure(const Header& header, const std::vector<std::uint8_t>& bytes) {
    if (header.channelCount == 0 || header.channelCount > kMaxChannels) {
        Fail("Unsupported channel count: " + std::to_string(header.channelCount));
    }
    if (header.sampleRate == 0) {
        Fail("Sample rate is zero");
    }
    if (header.sampleCount == 0) {
        Fail("Sample count is zero");
    }
    if (header.interleaveSize == 0 || (header.interleaveSize % kGcAdpcmFrameSize) != 0) {
        Fail("Interleave size must be a positive multiple of 8");
    }
    if (header.headerSize < kIdspHeaderSize) {
        Fail("Header size is smaller than 0x100");
    }
    if (header.streamInfoSize != kIdspStreamInfoSize) {
        Fail("Unexpected stream info size: " + std::to_string(header.streamInfoSize));
    }
    if (header.channelInfoSize != kIdspChannelInfoSize) {
        Fail("Unexpected channel info size: " + std::to_string(header.channelInfoSize));
    }
    if (header.headerSize < 0x40U + (header.channelCount * kIdspChannelInfoSize)) {
        Fail("Header does not contain enough channel metadata");
    }
    if (header.audioDataSize == 0 || (header.audioDataSize % header.interleaveSize) != 0) {
        Fail("Audio data size must be a non-zero multiple of interleave size");
    }

    const std::uint64_t audioBytes = static_cast<std::uint64_t>(header.audioDataSize) * header.channelCount;
    const std::uint64_t expectedSize = static_cast<std::uint64_t>(header.headerSize) + audioBytes;
    if (expectedSize > bytes.size()) {
        Fail("Truncated file: expected at least " + std::to_string(expectedSize) +
             " bytes but only found " + std::to_string(bytes.size()));
    }

    if (header.loopEnd != 0 && header.loopEnd < header.loopStart) {
        Fail("Loop end precedes loop start");
    }
}

std::vector<std::filesystem::path> CollectInputs(const std::vector<std::filesystem::path>& args) {
    std::vector<std::filesystem::path> inputs;
    for (const auto& path : args) {
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec || !exists) {
            Fail("Input path does not exist: '" + path.string() + "'");
        }

        if (std::filesystem::is_directory(path, ec)) {
            if (ec) {
                Fail("Unable to inspect directory '" + path.string() + "': " + ec.message());
            }
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    const auto ext = entry.path().extension().string();
                    if (ext == ".idsp" || ext == ".IDSP") {
                        inputs.push_back(entry.path());
                    }
                }
            }
        } else {
            inputs.push_back(path);
        }
    }

    std::sort(inputs.begin(), inputs.end());
    return inputs;
}

std::string FormatSamples(const std::vector<std::int16_t>& pcm, std::uint32_t channelCount, std::uint32_t sampleLimit) {
    std::ostringstream out;
    const std::uint32_t totalFrames = static_cast<std::uint32_t>(pcm.size() / channelCount);
    const std::uint32_t framesToShow = std::min(sampleLimit, totalFrames);

    for (std::uint32_t frame = 0; frame < framesToShow; ++frame) {
        out << "  frame[" << frame << "]=";
        if (channelCount == 1) {
            out << pcm[frame];
        } else {
            out << "(" << pcm[frame * 2] << ", " << pcm[frame * 2 + 1] << ")";
        }
        out << "\n";
    }

    return out.str();
}

void ValidateAndReport(const std::filesystem::path& path, std::uint32_t sampleDumpCount) {
    const std::vector<std::uint8_t> bytes = ReadWholeFileOrFail(path);
    const Header header = ParseHeader(bytes);
    ValidateStructure(header, bytes);

    std::vector<ChannelInfo> channels;
    channels.reserve(header.channelCount);
    for (std::uint32_t ch = 0; ch < header.channelCount; ++ch) {
        channels.push_back(ParseChannelInfo(bytes, ch));
        if (channels.back().sampleCount != header.sampleCount) {
            Fail("Channel " + std::to_string(ch) + " sample count does not match stream header");
        }
        if (channels.back().sampleRate != header.sampleRate) {
            Fail("Channel " + std::to_string(ch) + " sample rate does not match stream header");
        }
    }

    const std::uint32_t decodableSamples = GcByteCountToSampleCount(header.audioDataSize);
    if (header.sampleCount > decodableSamples) {
        Fail("Stream sample count exceeds decoded capacity");
    }

    const std::uint32_t framesPerBlock = header.interleaveSize / kGcAdpcmFrameSize;
    const std::uint32_t blockCount = header.audioDataSize / header.interleaveSize;
    const std::size_t pcmFrames = static_cast<std::size_t>(header.sampleCount);
    std::vector<std::int16_t> pcm(pcmFrames * header.channelCount);
    std::vector<Hist> history(header.channelCount);
    for (std::uint32_t ch = 0; ch < header.channelCount; ++ch) {
        history[ch].hist1 = channels[ch].initialHist1;
        history[ch].hist2 = channels[ch].initialHist2;
    }

    std::uint32_t produced = 0;
    for (std::uint32_t block = 0; block < blockCount && produced < header.sampleCount; ++block) {
        for (std::uint32_t frame = 0; frame < framesPerBlock && produced < header.sampleCount; ++frame) {
            const std::uint32_t samplesThisFrame = std::min(kGcAdpcmSamplesPerFrame, header.sampleCount - produced);
            for (std::uint32_t ch = 0; ch < header.channelCount; ++ch) {
                const std::size_t base = static_cast<std::size_t>(header.headerSize) +
                    static_cast<std::size_t>(block) * header.interleaveSize * header.channelCount +
                    static_cast<std::size_t>(ch) * header.interleaveSize +
                    static_cast<std::size_t>(frame) * kGcAdpcmFrameSize;
                if (base + kGcAdpcmFrameSize > bytes.size()) {
                    Fail("Truncated frame data at block " + std::to_string(block) +
                         ", frame " + std::to_string(frame) +
                         ", channel " + std::to_string(ch));
                }

                std::int16_t decoded[kGcAdpcmSamplesPerFrame] = {};
                DecodeGcAdpcmFrame(bytes.data() + base, channels[ch].coefs, history[ch], decoded, samplesThisFrame);
                for (std::uint32_t i = 0; i < samplesThisFrame; ++i) {
                    pcm[(static_cast<std::size_t>(produced + i) * header.channelCount) + ch] = decoded[i];
                }
            }
            produced += samplesThisFrame;
        }
    }

    std::vector<std::uint8_t> pcmBytes;
    pcmBytes.reserve(pcm.size() * sizeof(std::int16_t));
    for (std::int16_t sample : pcm) {
        AppendPcm16Le(pcmBytes, sample);
    }

    const std::uint32_t crc = Crc32(pcmBytes.data(), pcmBytes.size());
    std::cout << path.string() << "\n";
    std::cout << "  channels=" << header.channelCount
              << " sample_rate=" << header.sampleRate
              << " sample_count=" << header.sampleCount
              << " interleave=" << header.interleaveSize
              << " audio_bytes=" << header.audioDataSize << "\n";
    std::cout << "  pcm_crc32=" << Hex32(crc) << "\n";
    if (sampleDumpCount != 0) {
        std::cout << FormatSamples(pcm, header.channelCount, sampleDumpCount);
    }
}

void PrintUsage(const char* argv0) {
    std::cout
        << "Usage:\n"
        << "  \"" << argv0 << "\" [--dump-samples N] <path-or-dir> [more paths...]\n\n"
        << "Validates one or more Wii/GameCube IDSP files in read-only mode.\n"
        << "Directories are searched recursively for *.idsp files.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::uint32_t sampleDumpCount = 0;
        std::vector<std::filesystem::path> args;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                PrintUsage(argv[0]);
                return 0;
            }
            if (arg == "--dump-samples") {
                if (i + 1 >= argc) {
                    Fail("--dump-samples expects a count");
                }
                sampleDumpCount = static_cast<std::uint32_t>(std::stoul(argv[++i]));
                continue;
            }
            args.emplace_back(argv[i]);
        }

        if (args.empty()) {
            PrintUsage(argv[0]);
            return 1;
        }

        const std::vector<std::filesystem::path> inputs = CollectInputs(args);
        if (inputs.empty()) {
            Fail("No .idsp files were found");
        }

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            ValidateAndReport(inputs[i], sampleDumpCount);
            if (i + 1 != inputs.size()) {
                std::cout << "\n";
            }
        }

        std::cout << "Validation OK: read-only IDSP checks passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
