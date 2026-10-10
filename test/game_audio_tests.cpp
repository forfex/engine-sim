#include <gtest/gtest.h>

#include "../include/game_audio/audio_encoders.h"
#include "../include/game_audio/engine_session.h"
#include "../include/game_audio/exporter.h"
#include "../include/game_audio/json_writer.h"
#include "../include/game_audio/loop_builder.h"
#include "../include/game_audio/loudness.h"
#include "../include/game_audio/wav_file.h"

#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using namespace game_audio;

namespace {

    constexpr double Pi = 3.14159265358979323846;

    // A harmonically rich periodic signal (like an engine firing pattern)
    // with a non-integer period, plus optional broadband noise.
    std::vector<float> periodicSignal(size_t n, double period, double noise, unsigned seed = 7) {
        std::mt19937 rng(seed);
        std::normal_distribution<double> gauss(0.0, 1.0);

        std::vector<float> out(n);
        for (size_t i = 0; i < n; ++i) {
            const double phase = 2 * Pi * i / period;
            double v = 0.5 * std::sin(phase)
                + 0.25 * std::sin(2 * phase + 0.3)
                + 0.15 * std::sin(4 * phase + 1.1)
                + 0.08 * std::sin(7 * phase + 2.0);
            v += noise * gauss(rng);
            out[i] = static_cast<float>(v);
        }
        return out;
    }

    double maxAbsStep(const std::vector<float> &x, size_t begin, size_t end) {
        double m = 0.0;
        for (size_t i = begin + 1; i < end; ++i) {
            m = std::max(m, std::abs(static_cast<double>(x[i]) - x[i - 1]));
        }
        return m;
    }

    fs::path tempDir() {
        const fs::path dir = fs::temp_directory_path() / "engine_sim_game_audio_tests";
        fs::create_directories(dir);
        return dir;
    }

    std::vector<uint8_t> readFile(const fs::path &p) {
        std::ifstream f(p, std::ios::binary);
        return std::vector<uint8_t>(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    uint32_t le32(const uint8_t *p) {
        return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    uint32_t be32(const uint8_t *p) {
        return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    }

    const uint8_t *findBytes(const std::vector<uint8_t> &data, const char *needle, size_t limit) {
        const size_t n = std::strlen(needle);
        const size_t end = std::min(limit, data.size());
        for (size_t i = 0; i + n <= end; ++i) {
            if (std::memcmp(data.data() + i, needle, n) == 0) return data.data() + i;
        }
        return nullptr;
    }

} /* namespace */

// -------------------------------------------------------------------------
// Loop builder

TEST(LoopBuilderTests, CrossfadeGainsPreservePower) {
    for (const double rho : { 0.0, 0.3, 0.7, 1.0 }) {
        for (int i = 0; i <= 20; ++i) {
            const double t = i / 20.0;
            double a, b;
            crossfadeGains(t, rho, &a, &b);
            EXPECT_NEAR(a * a + b * b + 2 * rho * a * b, 1.0, 1e-9) << "rho=" << rho << " t=" << t;
        }

        double a, b;
        crossfadeGains(0.0, rho, &a, &b);
        EXPECT_NEAR(a, 0.0, 1e-12);
        EXPECT_NEAR(b, 1.0, 1e-12);
        crossfadeGains(1.0, rho, &a, &b);
        EXPECT_NEAR(a, 1.0, 1e-12);
        EXPECT_NEAR(b, 0.0, 1e-12);
    }
}

TEST(LoopBuilderTests, PeriodicLoopIsWholeCyclesAndSeamless) {
    const double period = 1234.567; // ~4286 rpm cycle at 44.1 kHz
    const std::vector<float> source = periodicSignal(44100 * 6, period, 0.01);

    LoopParameters p;
    p.periodSamples = period;
    p.targetLength = 44100 * 4;
    p.crossfadeLength = 3528;
    p.startOffset = 1000;

    LoopResult loop;
    std::string error;
    ASSERT_TRUE(buildSeamlessLoop(source, p, &loop, &error)) << error;

    const int expectedCycles = static_cast<int>(std::lround(p.targetLength / period));
    EXPECT_EQ(loop.cycles, expectedCycles);
    EXPECT_NEAR(static_cast<double>(loop.length), expectedCycles * period, 1.0);
    EXPECT_EQ(loop.samples.size(), loop.length);
    EXPECT_GT(loop.seamCorrelation, 0.99);

    // Play the loop twice in a row: the step across the seam must look like
    // any other step in the waveform.
    std::vector<float> twice = loop.samples;
    twice.insert(twice.end(), loop.samples.begin(), loop.samples.end());
    const double body = maxAbsStep(twice, 0, loop.length);
    const double seam = std::abs(static_cast<double>(twice[loop.length]) - twice[loop.length - 1]);
    EXPECT_LE(seam, body);
}

TEST(LoopBuilderTests, UnknownPeriodFindsAPeriodMultiple) {
    const double period = 777.3;
    const std::vector<float> source = periodicSignal(44100 * 4, period, 0.0);

    LoopParameters p;
    p.periodSamples = 0.0;
    p.targetLength = 44100 * 2;
    p.crossfadeLength = 2000;
    p.searchRadius = 2000;

    LoopResult loop;
    std::string error;
    ASSERT_TRUE(buildSeamlessLoop(source, p, &loop, &error)) << error;

    const double cycles = loop.length / period;
    EXPECT_NEAR(cycles, std::round(cycles), 0.01);
    EXPECT_GT(loop.seamCorrelation, 0.999);
}

TEST(LoopBuilderTests, StartSearchAvoidsADisturbedRegion) {
    const double period = 600.0;
    std::vector<float> source = periodicSignal(44100 * 4, period, 0.0);

    // Corrupt the first 0.2 s; with a start search the loop must begin
    // after it and still be perfectly periodic.
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> junk(-1.0f, 1.0f);
    for (size_t i = 0; i < 8820; ++i) source[i] = junk(rng);

    LoopParameters p;
    p.periodSamples = period;
    p.targetLength = 44100;
    p.crossfadeLength = 1200;
    p.startSearchLength = 22050;

    LoopResult loop;
    std::string error;
    ASSERT_TRUE(buildSeamlessLoop(source, p, &loop, &error)) << error;
    EXPECT_GE(loop.sourceStart, 8820u);
    EXPECT_GT(loop.seamCorrelation, 0.999);
}

TEST(LoopBuilderTests, RejectsShortSource) {
    LoopParameters p;
    p.periodSamples = 500.0;
    p.targetLength = 10000;
    p.crossfadeLength = 1000;

    const std::vector<float> source(requiredSourceLength(p) - 1, 0.5f);
    LoopResult loop;
    std::string error;
    EXPECT_FALSE(buildSeamlessLoop(source, p, &loop, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LoopBuilderTests, DcRemoval) {
    std::vector<float> loop = periodicSignal(10000, 100.0, 0.0);
    for (float &s : loop) s += 0.3f;
    removeMean(loop);

    double mean = 0.0;
    for (const float s : loop) mean += s;
    EXPECT_NEAR(mean / loop.size(), 0.0, 1e-6);

    std::vector<float> oneShot(44100, 0.5f);
    dcBlock(oneShot, 44100.0, 5.0);
    EXPECT_LT(std::abs(oneShot.back()), 1e-3);
}

TEST(LoopBuilderTests, FadesAndLevels) {
    std::vector<float> x(1000, 1.0f);
    applyFadeIn(x, 100);
    applyFadeOut(x, 100);
    EXPECT_FLOAT_EQ(x.front(), 0.0f);
    EXPECT_NEAR(x.back(), 0.0f, 1e-6);
    EXPECT_FLOAT_EQ(x[500], 1.0f);

    EXPECT_NEAR(linearToDb(dbToLinear(-6.0)), -6.0, 1e-9);
    EXPECT_FLOAT_EQ(peakAbs({ 0.1f, -0.7f, 0.3f }), 0.7f);
}

// -------------------------------------------------------------------------
// Loudness (ITU-R BS.1770)

TEST(LoudnessTests, FullScaleSineReadsMinus3Lufs) {
    // BS.1770 reference: a 0 dBFS 997 Hz sine in one channel = -3.01 LUFS.
    for (const double fs : { 44100.0, 48000.0 }) {
        std::vector<float> sine(static_cast<size_t>(fs * 5));
        for (size_t i = 0; i < sine.size(); ++i) {
            sine[i] = static_cast<float>(std::sin(2 * Pi * 997.0 * i / fs));
        }
        EXPECT_NEAR(integratedLoudness(sine, fs), -3.01, 0.05) << "fs=" << fs;
    }
}

TEST(LoudnessTests, GatingIgnoresSilenceAndScalesWithGain) {
    std::vector<float> tone(44100 * 3);
    for (size_t i = 0; i < tone.size(); ++i) {
        tone[i] = static_cast<float>(0.5 * std::sin(2 * Pi * 997.0 * i / 44100.0));
    }
    const double reference = integratedLoudness(tone, 44100.0);

    // Half of this clip is silence. Ungated it would read ~3 LU lower; the
    // gates drop the silent blocks. The few blocks straddling the boundary
    // pass the relative gate (as BS.1770 specifies) and lower the result by
    // ~0.2 LU.
    std::vector<float> padded(44100 * 3, 0.0f);
    padded.insert(padded.end(), tone.begin(), tone.end());
    EXPECT_NEAR(integratedLoudness(padded, 44100.0), reference, 0.3);

    std::vector<float> quieter = tone;
    applyGain(quieter, dbToLinear(-12.0));
    EXPECT_NEAR(integratedLoudness(quieter, 44100.0), reference - 12.0, 0.01);

    EXPECT_FALSE(std::isfinite(integratedLoudness(std::vector<float>(44100, 0.0f), 44100.0)));
}

// -------------------------------------------------------------------------
// WAV

TEST(WavTests, Pcm16RoundTripWithLoopChunk) {
    const fs::path path = tempDir() / "roundtrip16.wav";

    std::vector<float> samples(5000);
    for (size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(0.8 * std::sin(i * 0.01));

    WavWriteOptions options;
    options.bitsPerSample = 16;
    options.hasLoop = true;
    options.loopStart = 0;
    options.loopLength = static_cast<uint32_t>(samples.size());

    std::string error;
    ASSERT_TRUE(writeWavMono(path.string(), samples, 44100, options, &error)) << error;

    std::vector<int16_t> read;
    int rate = 0;
    ASSERT_TRUE(readWavPcm16Mono(path.string(), &read, &rate, &error)) << error;
    EXPECT_EQ(rate, 44100);
    ASSERT_EQ(read.size(), samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        // TPDF dither adds at most +-1 LSB, rounding another half.
        EXPECT_NEAR(read[i] / 32767.0, samples[i], 2.0 / 32767.0);
    }

    const std::vector<uint8_t> bytes = readFile(path);
    const uint8_t *smpl = findBytes(bytes, "smpl", bytes.size());
    ASSERT_NE(smpl, nullptr);
    EXPECT_EQ(le32(smpl + 8 + 28), 1u); // loop count
    EXPECT_EQ(le32(smpl + 8 + 36 + 8), 0u); // loop start
    EXPECT_EQ(le32(smpl + 8 + 36 + 12), samples.size() - 1); // inclusive end
}

TEST(WavTests, Pcm24HasExpectedSize) {
    const fs::path path = tempDir() / "size24.wav";
    const std::vector<float> samples(1001, 0.25f);

    WavWriteOptions options;
    options.bitsPerSample = 24;

    std::string error;
    ASSERT_TRUE(writeWavMono(path.string(), samples, 48000, options, &error)) << error;

    const std::vector<uint8_t> bytes = readFile(path);
    const uint8_t *data = findBytes(bytes, "data", bytes.size());
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(le32(data + 4), 1001u * 3);
    EXPECT_EQ(le32(bytes.data() + 4), bytes.size() - 8); // RIFF size incl. pad byte

    // Sample value 0.25 -> 0x200000 (little endian)
    EXPECT_EQ(data[8], 0x00);
    EXPECT_EQ(data[9], 0x00);
    EXPECT_EQ(data[10], 0x20);
}

TEST(WavTests, RejectsInvalidLoopAndBadFiles) {
    std::string error;
    WavWriteOptions options;
    options.hasLoop = true;
    options.loopStart = 10;
    options.loopLength = 100;
    EXPECT_FALSE(writeWavMono((tempDir() / "bad.wav").string(), std::vector<float>(50), 44100, options, &error));

    const fs::path junk = tempDir() / "junk.wav";
    std::ofstream(junk, std::ios::binary) << "not a wave file";
    std::vector<int16_t> samples;
    int rate = 0;
    EXPECT_FALSE(readWavPcm16Mono(junk.string(), &samples, &rate, &error));
    EXPECT_FALSE(readWavPcm16Mono((tempDir() / "missing.wav").string(), &samples, &rate, &error));
}

// -------------------------------------------------------------------------
// Encoders

TEST(EncoderTests, OggVorbisIsSampleAccurateAndTagged) {
    const fs::path path = tempDir() / "loop.ogg";
    const std::vector<float> samples = periodicSignal(44100 * 2 + 123, 500.0, 0.01);

    OggVorbisOptions options;
    options.comments.push_back({ "LOOPSTART", "0" });
    options.comments.push_back({ "LOOPLENGTH", std::to_string(samples.size()) });

    std::string error;
    ASSERT_TRUE(encodeOggVorbis(path.string(), samples, 44100, options, &error)) << error;

    OggVorbis_File vf;
    ASSERT_EQ(ov_fopen(path.string().c_str(), &vf), 0);

    EXPECT_EQ(ov_pcm_total(&vf, -1), static_cast<ogg_int64_t>(samples.size()));
    EXPECT_EQ(ov_info(&vf, -1)->channels, 1);
    EXPECT_EQ(ov_info(&vf, -1)->rate, 44100);

    const char *loopLength = vorbis_comment_query(ov_comment(&vf, -1), "LOOPLENGTH", 0);
    ASSERT_NE(loopLength, nullptr);
    EXPECT_EQ(std::string(loopLength), std::to_string(samples.size()));

    // Decoded audio closely matches the input (lossy, but high quality).
    std::vector<float> decoded;
    for (;;) {
        float **pcm = nullptr;
        int section = 0;
        const long n = ov_read_float(&vf, &pcm, 4096, &section);
        if (n <= 0) break;
        decoded.insert(decoded.end(), pcm[0], pcm[0] + n);
    }
    ov_clear(&vf);

    ASSERT_EQ(decoded.size(), samples.size());
    EXPECT_GT(normalizedCrossCorrelation(decoded.data(), samples.data(), samples.size()), 0.99);
}

TEST(EncoderTests, Mp3HasGaplessInfoTag) {
    const fs::path path = tempDir() / "loop.mp3";
    const std::vector<float> samples = periodicSignal(44100 * 2 + 777, 500.0, 0.01);

    std::string error;
    ASSERT_TRUE(encodeMp3(path.string(), samples, 44100, Mp3Options(), &error)) << error;

    const std::vector<uint8_t> bytes = readFile(path);
    ASSERT_GT(bytes.size(), 1000u);

    // First frame is the Xing/Info frame (VBR -> "Xing").
    ASSERT_EQ(bytes[0], 0xFF);
    ASSERT_EQ(bytes[1] & 0xE0, 0xE0);

    const uint8_t *xing = findBytes(bytes, "Xing", 2048);
    if (xing == nullptr) xing = findBytes(bytes, "Info", 2048);
    ASSERT_NE(xing, nullptr);

    const uint32_t flags = be32(xing + 4);
    ASSERT_TRUE(flags & 1u) << "frame count missing";
    const uint32_t frames = be32(xing + 8);

    const uint8_t *lame = findBytes(bytes, "LAME", 2048);
    ASSERT_NE(lame, nullptr);
    const uint32_t delay = (lame[21] << 4) | (lame[22] >> 4);
    const uint32_t padding = ((lame[22] & 0x0F) << 8) | lame[23];

    // Gapless decoders trim delay and padding to recover the exact length.
    EXPECT_EQ(static_cast<uint64_t>(frames) * 1152 - delay - padding, samples.size());
}

// -------------------------------------------------------------------------
// JSON, plan and option handling

TEST(JsonWriterTests, ProducesNestedEscapedJson) {
    JsonWriter json;
    json.beginObject();
    json.key("name").value("a \"quoted\"\\path\n");
    json.key("list").beginArray().value(1).value(2.5).value(true).null().endArray();
    json.key("empty").beginObject().endObject();
    json.key("nan").value(std::nan(""));
    json.endObject();

    EXPECT_EQ(json.str(),
        "{\n"
        "  \"name\": \"a \\\"quoted\\\"\\\\path\\n\",\n"
        "  \"list\": [\n"
        "    1,\n"
        "    2.5,\n"
        "    true,\n"
        "    null\n"
        "  ],\n"
        "  \"empty\": {},\n"
        "  \"nan\": null\n"
        "}");

    EXPECT_EQ(JsonWriter::escape(std::string("\x01", 1)), "\\u0001");
}

TEST(ExporterTests, RpmGrid) {
    EXPECT_EQ(rpmGrid(1000, 3000, 500), (std::vector<double>{ 1000, 1500, 2000, 2500, 3000 }));
    EXPECT_EQ(rpmGrid(1000, 3200, 500), (std::vector<double>{ 1000, 1500, 2000, 2500, 3000, 3200 }));
    EXPECT_EQ(rpmGrid(1000, 3100, 500), (std::vector<double>{ 1000, 1500, 2000, 2500, 3000 }));
    EXPECT_TRUE(rpmGrid(3000, 1000, 500).empty());
}

TEST(ExporterTests, Slugify) {
    EXPECT_EQ(slugify("Subaru EJ25 (EH)"), "subaru_ej25_eh");
    EXPECT_EQ(slugify("  --  "), "engine");
    EXPECT_EQ(slugify("V8 - 5.7L"), "v8_5_7l");
}

TEST(ExporterTests, ValidateOptions) {
    ExportOptions o;
    o.session.scriptPath = "main.mr";
    EXPECT_EQ(validateOptions(o), "");

    ExportOptions bad = o;
    bad.writeOgg = bad.writeMp3 = bad.writeWav = false;
    EXPECT_NE(validateOptions(bad), "");

    bad = o;
    bad.clipGroups = { "turbo" };
    EXPECT_NE(validateOptions(bad), "");

    bad = o;
    bad.crossfadeSeconds = 2.0;
    EXPECT_NE(validateOptions(bad), "");

    bad = o;
    bad.rpmMin = 4000;
    bad.rpmMax = 3000;
    EXPECT_NE(validateOptions(bad), "");
}

// -------------------------------------------------------------------------
// Engine integration (uses the bundled default engine)

namespace {

    SessionConfig defaultEngineConfig() {
        const fs::path root = ENGINE_SIM_SOURCE_DIR;
        SessionConfig config;
        config.scriptPath = (root / "assets" / "main.mr").generic_string();
        config.searchPaths.push_back((root / "es").generic_string() + "/");
        return config;
    }

    std::vector<float> renderHeldRpm(double rpm, double throttle, double seconds) {
        std::string error;
        std::unique_ptr<EngineSession> session = EngineSession::create(defaultEngineConfig(), &error);
        EXPECT_TRUE(session) << error;
        if (!session) return {};

        session->setIgnition(true);
        session->setThrottleImmediate(throttle);
        for (int i = 0; i <= 50; ++i) {
            session->setDynoHold(rpm * i / 50.0);
            session->advance(0.01, nullptr, nullptr);
        }
        session->advance(1.0, nullptr, nullptr);

        std::vector<float> audio;
        session->advance(seconds, &audio, nullptr);
        EXPECT_NEAR(session->rpm(), rpm, rpm * 0.03);
        return audio;
    }

} /* namespace */

TEST(EngineSessionTests, HeldRpmProducesAudio) {
    std::srand(1);
    const std::vector<float> audio = renderHeldRpm(3000, 1.0, 0.5);

    // 44.1 kHz output for the simulated duration.
    EXPECT_NEAR(static_cast<double>(audio.size()), 0.5 * 44100, 44100 * 0.01);
    EXPECT_GT(rms(audio), 0.0);
    for (const float s : audio) ASSERT_TRUE(std::isfinite(s));
}

TEST(EngineSessionTests, RendersAreDeterministicForASeed) {
    std::srand(42);
    const std::vector<float> a = renderHeldRpm(2000, 0.5, 0.25);
    std::srand(42);
    const std::vector<float> b = renderHeldRpm(2000, 0.5, 0.25);
    EXPECT_EQ(a, b);
}

TEST(EngineSessionTests, OnLoadIsLouderThanOffLoad) {
    std::srand(3);
    const std::vector<float> on = renderHeldRpm(3000, 1.0, 0.5);
    std::srand(3);
    const std::vector<float> off = renderHeldRpm(3000, 0.0, 0.5);
    EXPECT_GT(rms(on), rms(off));
}

TEST(EngineSessionTests, MissingScriptReportsError) {
    SessionConfig config = defaultEngineConfig();
    config.scriptPath = "this/does/not/exist.mr";
    std::string error;
    EXPECT_FALSE(EngineSession::create(config, &error));
    EXPECT_FALSE(error.empty());
}
