#ifndef ATG_ENGINE_SIM_GAME_AUDIO_LOUDNESS_H
#define ATG_ENGINE_SIM_GAME_AUDIO_LOUDNESS_H

#include <vector>

namespace game_audio {

    // Integrated loudness of a mono signal in LUFS according to
    // ITU-R BS.1770-4: K-weighting, 400 ms blocks with 75 % overlap, an
    // absolute gate at -70 LUFS and a relative gate 10 LU below the
    // ungated level. Clips shorter than one block are measured as a single
    // block. Returns -infinity for silence.
    double integratedLoudness(const std::vector<float> &samples, double sampleRate);

} /* namespace game_audio */

#endif /* ATG_ENGINE_SIM_GAME_AUDIO_LOUDNESS_H */
