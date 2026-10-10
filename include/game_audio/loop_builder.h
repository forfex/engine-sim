#ifndef ATG_ENGINE_SIM_GAME_AUDIO_LOOP_BUILDER_H
#define ATG_ENGINE_SIM_GAME_AUDIO_LOOP_BUILDER_H

#include <cstddef>
#include <string>
#include <vector>

namespace game_audio {

    struct LoopParameters {
        // Expected period of the signal in samples (one full engine cycle,
        // i.e. 720 degrees of crank rotation for a four-stroke engine).
        // When <= 0 the period is unknown and the loop length is found by a
        // wide correlation search around targetLength.
        double periodSamples = 0.0;

        // Desired loop length in samples. For periodic signals the actual
        // length is a whole number of periods close to this value.
        size_t targetLength = 0;

        // Length of the seam crossfade in samples. Clamped to a quarter of
        // the loop length.
        size_t crossfadeLength = 0;

        // Search radius (samples) used when periodSamples <= 0.
        size_t searchRadius = 0;

        // Earliest position in the source where the loop may begin.
        size_t startOffset = 0;

        // When > 0, start positions in [startOffset, startOffset +
        // startSearchLength] are also searched and the start/length pair with
        // the best seam is used. Helps with signals that drift slightly, such
        // as an idling engine whose speed hunts around its target.
        size_t startSearchLength = 0;
    };

    struct LoopResult {
        std::vector<float> samples;
        size_t sourceStart = 0;
        size_t length = 0;
        size_t crossfadeLength = 0;

        // Whole periods contained in the loop (0 if the period is unknown).
        int cycles = 0;

        // Normalized cross-correlation between the audio that is faded out
        // and the audio that is faded in at the seam (1 = identical).
        double seamCorrelation = 0.0;
    };

    // Builds a loop that repeats without a click or level dip:
    //   1. The loop length is chosen as a whole number of signal periods and
    //      refined to the sample with the highest normalized cross-correlation
    //      across the seam, so the two sides of the seam are in phase.
    //   2. The first `crossfade` samples blend the loop start with the audio
    //      that follows the loop end. The fade law adapts to the measured
    //      correlation (linear for coherent audio, equal power for
    //      uncorrelated noise) to keep the perceived level constant.
    // The returned buffer can be played back to back with itself.
    bool buildSeamlessLoop(
        const std::vector<float> &source,
        const LoopParameters &params,
        LoopResult *result,
        std::string *error);

    // Number of source samples buildSeamlessLoop may read for these parameters.
    size_t requiredSourceLength(const LoopParameters &params);

    // Pearson correlation of a[0..n) and b[0..n), in [-1, 1]. Returns 0 when
    // either side has no variance.
    double normalizedCrossCorrelation(const float *a, const float *b, size_t n);

    // Adaptive crossfade gains at position t in [0, 1] for correlation rho.
    // Satisfies fadeIn^2 + fadeOut^2 + 2 * rho * fadeIn * fadeOut = 1.
    void crossfadeGains(double t, double rho, double *fadeIn, double *fadeOut);

    // Subtracts the mean. For a loop this removes DC without affecting the seam.
    void removeMean(std::vector<float> &samples);

    // First-order DC blocking high-pass, for non-looping clips.
    void dcBlock(std::vector<float> &samples, double sampleRate, double cutoffHz);

    // Raised-cosine fades at the start and/or end of a clip.
    void applyFadeIn(std::vector<float> &samples, size_t length);
    void applyFadeOut(std::vector<float> &samples, size_t length);

    float peakAbs(const std::vector<float> &samples);
    double rms(const std::vector<float> &samples);
    void applyGain(std::vector<float> &samples, double gain);

    double linearToDb(double linear);
    double dbToLinear(double db);

} /* namespace game_audio */

#endif /* ATG_ENGINE_SIM_GAME_AUDIO_LOOP_BUILDER_H */
