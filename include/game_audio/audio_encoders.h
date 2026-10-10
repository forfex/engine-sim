#ifndef ATG_ENGINE_SIM_GAME_AUDIO_AUDIO_ENCODERS_H
#define ATG_ENGINE_SIM_GAME_AUDIO_AUDIO_ENCODERS_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace game_audio {

    using TagList = std::vector<std::pair<std::string, std::string>>;

    struct OggVorbisOptions {
        // libvorbis VBR quality, -1 (smallest) to 10 (best). 6 is ~192 kbps
        // stereo-equivalent and is transparent for engine audio.
        float quality = 6.0f;

        // Ogg logical stream serial number. Fixed for reproducible output.
        int serialNumber = 0x45534d31;

        // Vorbis comments. For loops, LOOPSTART/LOOPLENGTH (in samples) are
        // read by Godot, GameMaker, RPG Maker and many other engines.
        TagList comments;
    };

    // Encodes mono float samples in [-1, 1] as Ogg Vorbis. Vorbis stores the
    // exact sample count in the stream (granule position), so decoders return
    // exactly samples.size() samples and loops stay sample accurate.
    bool encodeOggVorbis(
        const std::string &path,
        const std::vector<float> &samples,
        int sampleRate,
        const OggVorbisOptions &options,
        std::string *error);

    struct Mp3Options {
        // When true, VBR is used with `vbrQuality` (0 = best, 9 = smallest);
        // otherwise constant bitrate `cbrKbps` is used.
        bool vbr = true;
        int vbrQuality = 2;
        int cbrKbps = 192;

        // Algorithm quality, 0 (best, slowest) to 9.
        int algorithmQuality = 2;
    };

    // Encodes mono float samples in [-1, 1] as MP3 with a Xing/LAME info
    // frame. The info frame records encoder delay and padding, which is the
    // only way MP3 can describe gapless playback; whether a decoder honours it
    // is decoder specific (see docs/game-audio-export.md).
    bool encodeMp3(
        const std::string &path,
        const std::vector<float> &samples,
        int sampleRate,
        const Mp3Options &options,
        std::string *error);

} /* namespace game_audio */

#endif /* ATG_ENGINE_SIM_GAME_AUDIO_AUDIO_ENCODERS_H */
