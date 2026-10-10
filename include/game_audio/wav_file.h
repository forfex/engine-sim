#ifndef ATG_ENGINE_SIM_GAME_AUDIO_WAV_FILE_H
#define ATG_ENGINE_SIM_GAME_AUDIO_WAV_FILE_H

#include <cstdint>
#include <string>
#include <vector>

namespace game_audio {

    // Reads an uncompressed 16-bit PCM RIFF/WAVE file (plain PCM or
    // WAVE_FORMAT_EXTENSIBLE with a PCM sub-format). Multi-channel files are
    // reduced to their first channel, which matches how engine-sim treats
    // impulse responses.
    bool readWavPcm16Mono(
        const std::string &path,
        std::vector<int16_t> *samples,
        int *sampleRate,
        std::string *error);

    struct WavWriteOptions {
        // 16 or 24. 16-bit output is TPDF-dithered.
        int bitsPerSample = 24;

        // When set, a 'smpl' chunk with one forward loop is written. Unity,
        // Unreal, FMOD, Wwise and most samplers read loop points from it.
        bool hasLoop = false;
        uint32_t loopStart = 0;
        uint32_t loopLength = 0;

        // Seed for the dither noise so that exports are reproducible.
        uint32_t ditherSeed = 0x2545F491u;
    };

    // Writes mono float samples (nominal range [-1, 1], clipped) as PCM WAV.
    bool writeWavMono(
        const std::string &path,
        const std::vector<float> &samples,
        int sampleRate,
        const WavWriteOptions &options,
        std::string *error);

} /* namespace game_audio */

#endif /* ATG_ENGINE_SIM_GAME_AUDIO_WAV_FILE_H */
