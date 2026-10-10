#ifndef ATG_ENGINE_SIM_GAME_AUDIO_EXPORTER_H
#define ATG_ENGINE_SIM_GAME_AUDIO_EXPORTER_H

#include "audio_encoders.h"
#include "engine_session.h"
#include "loop_builder.h"

#include <cstdint>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace game_audio {

    // Clip groups that can be selected with --clips.
    namespace groups {
        constexpr const char *Idle = "idle";
        constexpr const char *OnLoad = "onload";
        constexpr const char *OffLoad = "offload";
        constexpr const char *Limiter = "limiter";
        constexpr const char *Startup = "startup";
        constexpr const char *Shutdown = "shutdown";
        constexpr const char *RevBlip = "revs";
        constexpr const char *Sweeps = "sweeps";
    }

    std::set<std::string> allClipGroups();

    enum class Normalization {
        // Every clip is brought to the same integrated loudness (ITU-R
        // BS.1770, LUFS), limited by the peak ceiling. Layers then crossfade
        // without level jumps and the game controls volume through its own
        // RPM/throttle curves. The clip's natural level is kept in the
        // manifest (naturalLevelDb). Recommended: the raw synthesizer output
        // has an unrealistically wide level range (60 dB or more between low
        // and high RPM loops), which the interactive app hides with an
        // automatic leveler.
        Loudness,

        // One gain for the whole set: the loudest sample of any clip reaches
        // the peak ceiling and raw relative levels are preserved.
        Set,

        // Every clip is peak-normalized to the ceiling on its own.
        PerFile
    };

    struct ExportOptions {
        SessionConfig session;
        std::string outputDirectory = "engine-audio-export";

        bool writeOgg = true;
        bool writeMp3 = true;
        bool writeWav = true;

        // RPM grid for the steady-state loops. 0 = derive from the engine
        // (dyno minimum speed and redline).
        double rpmMin = 0.0;
        double rpmMax = 0.0;
        double rpmStep = 500.0;

        double onLoadThrottle = 1.0;
        double offLoadThrottle = 0.0;

        double loopSeconds = 4.0;
        double crossfadeSeconds = 0.08;
        double settleSeconds = 2.0;
        double sweepSeconds = 12.0;

        std::set<std::string> clipGroups = allClipGroups();

        Normalization normalization = Normalization::Loudness;
        double loudnessLufs = -18.0;
        double peakDb = -1.0;

        OggVorbisOptions ogg;
        Mp3Options mp3;
        int wavBits = 24;

        // Worker threads; 0 = hardware concurrency.
        int jobs = 0;
        uint32_t seed = 1;
    };

    // Returns an empty string when the options are valid.
    std::string validateOptions(const ExportOptions &options);

    // RPM points used for the on/off-load loops.
    std::vector<double> rpmGrid(double rpmMin, double rpmMax, double step);

    // Lower-case, filesystem-safe identifier ("Subaru EJ25" -> "subaru_ej25").
    std::string slugify(const std::string &text);

    // Renders, post-processes, encodes and documents the full sample set.
    // Progress goes to `log`. Returns a process exit code (0 on success).
    int runExport(const ExportOptions &options, std::ostream &log);

} /* namespace game_audio */

#endif /* ATG_ENGINE_SIM_GAME_AUDIO_EXPORTER_H */
