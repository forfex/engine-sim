#include "../../include/game_audio/wav_file.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace {

    constexpr uint16_t WaveFormatPcm = 0x0001;
    constexpr uint16_t WaveFormatExtensible = 0xFFFE;

    uint16_t readU16(const uint8_t *p) {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    uint32_t readU32(const uint8_t *p) {
        return static_cast<uint32_t>(p[0])
            | (static_cast<uint32_t>(p[1]) << 8)
            | (static_cast<uint32_t>(p[2]) << 16)
            | (static_cast<uint32_t>(p[3]) << 24);
    }

    void putU16(std::vector<uint8_t> &out, uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }

    void putU32(std::vector<uint8_t> &out, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }

    void putTag(std::vector<uint8_t> &out, const char *tag) {
        out.insert(out.end(), tag, tag + 4);
    }

    // xorshift32; small, fast and deterministic for dither generation.
    class DitherNoise {
    public:
        explicit DitherNoise(uint32_t seed) : m_state(seed != 0 ? seed : 1u) {}

        // Uniform in [-0.5, 0.5)
        double uniform() {
            m_state ^= m_state << 13;
            m_state ^= m_state >> 17;
            m_state ^= m_state << 5;
            return (m_state / 4294967296.0) - 0.5;
        }

        // Triangular PDF in (-1, 1) LSB
        double tpdf() { return uniform() + uniform(); }

    private:
        uint32_t m_state;
    };

} /* namespace */

bool game_audio::readWavPcm16Mono(
    const std::string &path,
    std::vector<int16_t> *samples,
    int *sampleRate,
    std::string *error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = "cannot open '" + path + "'";
        return false;
    }

    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    if (data.size() < 12
        || std::memcmp(data.data(), "RIFF", 4) != 0
        || std::memcmp(data.data() + 8, "WAVE", 4) != 0)
    {
        *error = "'" + path + "' is not a RIFF/WAVE file";
        return false;
    }

    bool haveFormat = false;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bitsPerSample = 0;
    const uint8_t *pcm = nullptr;
    size_t pcmBytes = 0;

    size_t offset = 12;
    while (offset + 8 <= data.size()) {
        const uint8_t *chunk = data.data() + offset;
        const uint32_t chunkSize = readU32(chunk + 4);
        const size_t bodyOffset = offset + 8;
        const size_t available = data.size() - bodyOffset;
        const size_t bodySize = std::min<size_t>(chunkSize, available);

        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (bodySize < 16) {
                *error = "'" + path + "' has a truncated fmt chunk";
                return false;
            }

            const uint8_t *fmt = data.data() + bodyOffset;
            uint16_t formatTag = readU16(fmt);
            channels = readU16(fmt + 2);
            rate = readU32(fmt + 4);
            bitsPerSample = readU16(fmt + 14);

            if (formatTag == WaveFormatExtensible) {
                if (bodySize < 40) {
                    *error = "'" + path + "' has a truncated WAVE_FORMAT_EXTENSIBLE header";
                    return false;
                }

                // The first two bytes of the sub-format GUID hold the format tag.
                formatTag = readU16(fmt + 24);
            }

            if (formatTag != WaveFormatPcm) {
                *error = "'" + path + "' is not PCM encoded";
                return false;
            }

            haveFormat = true;
        }
        else if (std::memcmp(chunk, "data", 4) == 0) {
            pcm = data.data() + bodyOffset;
            pcmBytes = bodySize;
        }

        // Chunks are padded to an even size.
        offset = bodyOffset + static_cast<size_t>(chunkSize) + (chunkSize & 1u);
    }

    if (!haveFormat || pcm == nullptr) {
        *error = "'" + path + "' is missing its fmt or data chunk";
        return false;
    }

    if (bitsPerSample != 16) {
        *error = "'" + path + "' is " + std::to_string(bitsPerSample)
            + "-bit; only 16-bit PCM is supported";
        return false;
    }

    if (channels == 0 || rate == 0) {
        *error = "'" + path + "' has an invalid channel count or sample rate";
        return false;
    }

    const size_t frameBytes = static_cast<size_t>(channels) * 2;
    const size_t frames = pcmBytes / frameBytes;

    samples->resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        (*samples)[i] = static_cast<int16_t>(readU16(pcm + i * frameBytes));
    }

    *sampleRate = static_cast<int>(rate);
    return true;
}

bool game_audio::writeWavMono(
    const std::string &path,
    const std::vector<float> &samples,
    int sampleRate,
    const WavWriteOptions &options,
    std::string *error)
{
    if (options.bitsPerSample != 16 && options.bitsPerSample != 24) {
        *error = "unsupported WAV bit depth " + std::to_string(options.bitsPerSample);
        return false;
    }

    if (options.hasLoop
        && (options.loopLength == 0
            || static_cast<uint64_t>(options.loopStart) + options.loopLength > samples.size()))
    {
        *error = "loop region is outside the sample data";
        return false;
    }

    const uint16_t bytesPerSample = static_cast<uint16_t>(options.bitsPerSample / 8);
    const uint64_t dataBytes64 = static_cast<uint64_t>(samples.size()) * bytesPerSample;
    if (dataBytes64 > 0xFFFFFF00ull) {
        *error = "audio is too long for a RIFF/WAVE file";
        return false;
    }

    const uint32_t dataBytes = static_cast<uint32_t>(dataBytes64);

    std::vector<uint8_t> out;
    out.reserve(static_cast<size_t>(dataBytes) + 128);

    putTag(out, "RIFF");
    putU32(out, 0); // patched below
    putTag(out, "WAVE");

    putTag(out, "fmt ");
    putU32(out, 16);
    putU16(out, WaveFormatPcm);
    putU16(out, 1);
    putU32(out, static_cast<uint32_t>(sampleRate));
    putU32(out, static_cast<uint32_t>(sampleRate) * bytesPerSample);
    putU16(out, bytesPerSample);
    putU16(out, static_cast<uint16_t>(options.bitsPerSample));

    if (options.hasLoop) {
        // 'smpl' chunk: 36 byte header + one 24 byte loop record.
        putTag(out, "smpl");
        putU32(out, 36 + 24);
        putU32(out, 0); // manufacturer
        putU32(out, 0); // product
        putU32(out, static_cast<uint32_t>(std::lround(1e9 / sampleRate))); // sample period (ns)
        putU32(out, 60); // MIDI unity note
        putU32(out, 0); // MIDI pitch fraction
        putU32(out, 0); // SMPTE format
        putU32(out, 0); // SMPTE offset
        putU32(out, 1); // loop count
        putU32(out, 0); // sampler data

        putU32(out, 0); // cue point id
        putU32(out, 0); // type: forward loop
        putU32(out, options.loopStart);
        putU32(out, options.loopStart + options.loopLength - 1); // inclusive end
        putU32(out, 0); // fraction
        putU32(out, 0); // play count: infinite
    }

    putTag(out, "data");
    putU32(out, dataBytes);

    DitherNoise noise(options.ditherSeed);
    for (const float sample : samples) {
        const double s = std::isfinite(sample) ? static_cast<double>(sample) : 0.0;

        if (options.bitsPerSample == 16) {
            const double scaled = s * 32767.0 + noise.tpdf();
            const long v = std::lround(std::max(-32768.0, std::min(32767.0, scaled)));
            putU16(out, static_cast<uint16_t>(static_cast<int16_t>(v)));
        }
        else {
            const double scaled = s * 8388607.0;
            const long v = std::lround(std::max(-8388608.0, std::min(8388607.0, scaled)));
            const uint32_t u = static_cast<uint32_t>(v) & 0xFFFFFFu;
            out.push_back(static_cast<uint8_t>(u & 0xFF));
            out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
        }
    }

    if (dataBytes & 1u) {
        out.push_back(0);
    }

    const uint32_t riffSize = static_cast<uint32_t>(out.size() - 8);
    out[4] = static_cast<uint8_t>(riffSize & 0xFF);
    out[5] = static_cast<uint8_t>((riffSize >> 8) & 0xFF);
    out[6] = static_cast<uint8_t>((riffSize >> 16) & 0xFF);
    out[7] = static_cast<uint8_t>((riffSize >> 24) & 0xFF);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        *error = "cannot create '" + path + "'";
        return false;
    }

    file.write(reinterpret_cast<const char *>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!file) {
        *error = "failed writing '" + path + "'";
        return false;
    }

    return true;
}
