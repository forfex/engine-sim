#include "../../include/game_audio/loop_builder.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

    constexpr double Pi = 3.14159265358979323846;

    struct SearchWindow {
        size_t minLength;
        size_t maxLength;
        int cycles;
    };

    size_t effectiveCrossfade(const game_audio::LoopParameters &params, size_t loopLength) {
        return std::min(params.crossfadeLength, loopLength / 4);
    }

    SearchWindow searchWindow(const game_audio::LoopParameters &params) {
        SearchWindow w{ 0, 0, 0 };

        if (params.periodSamples > 0.0) {
            const double period = params.periodSamples;
            const int cycles = std::max(1, static_cast<int>(std::lround(params.targetLength / period)));
            const double nominal = cycles * period;

            // Half a period either side contains exactly one in-phase candidate.
            const double radius = std::max(1.0, std::floor(period / 2.0));
            w.minLength = static_cast<size_t>(std::max(1.0, std::floor(nominal - radius)));
            w.maxLength = static_cast<size_t>(std::ceil(nominal + radius));
            w.cycles = cycles;
        }
        else {
            const size_t radius = std::min(params.searchRadius, params.targetLength / 2);
            w.minLength = std::max<size_t>(1, params.targetLength - radius);
            w.maxLength = params.targetLength + radius;
            w.cycles = 0;
        }

        return w;
    }

} /* namespace */

size_t game_audio::requiredSourceLength(const LoopParameters &params) {
    const SearchWindow w = searchWindow(params);
    return params.startOffset + params.startSearchLength
        + w.maxLength + effectiveCrossfade(params, w.maxLength);
}

double game_audio::normalizedCrossCorrelation(const float *a, const float *b, size_t n) {
    if (n == 0) return 0.0;

    double meanA = 0.0, meanB = 0.0;
    for (size_t i = 0; i < n; ++i) {
        meanA += a[i];
        meanB += b[i];
    }
    meanA /= n;
    meanB /= n;

    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        ab += da * db;
        aa += da * da;
        bb += db * db;
    }

    if (aa <= 0.0 || bb <= 0.0) return 0.0;
    return ab / std::sqrt(aa * bb);
}

void game_audio::crossfadeGains(double t, double rho, double *fadeIn, double *fadeOut) {
    t = std::max(0.0, std::min(1.0, t));
    rho = std::max(0.0, std::min(1.0, rho));

    // Smooth (raised-cosine) shape, normalized so the expected power of the
    // mix stays constant for signals with correlation rho.
    const double s = 0.5 - 0.5 * std::cos(Pi * t);
    const double a = s;
    const double b = 1.0 - s;
    const double norm = std::sqrt(a * a + b * b + 2.0 * rho * a * b);

    *fadeIn = a / norm;
    *fadeOut = b / norm;
}

bool game_audio::buildSeamlessLoop(
    const std::vector<float> &source,
    const LoopParameters &params,
    LoopResult *result,
    std::string *error)
{
    if (params.targetLength < 8) {
        *error = "loop target length is too short";
        return false;
    }

    const size_t required = requiredSourceLength(params);
    if (source.size() < required) {
        *error = "not enough audio to build the loop (have "
            + std::to_string(source.size()) + " samples, need "
            + std::to_string(required) + ")";
        return false;
    }

    const SearchWindow w = searchWindow(params);

    // The comparison window used to score candidate lengths. Use at least a
    // full period (when known) so the score reflects waveform shape rather
    // than a few samples near a zero crossing.
    size_t window = effectiveCrossfade(params, w.minLength);
    if (params.periodSamples > 0.0) {
        window = std::max(window, static_cast<size_t>(std::ceil(params.periodSamples)));
    }
    window = std::max<size_t>(window, 64);
    window = std::min(window, w.minLength / 2);

    // Candidate start positions. Stepping by half the comparison window keeps
    // neighbouring candidates overlapping while bounding the search cost.
    const size_t startStep = std::max<size_t>(1, window / 2);
    const size_t lastStart = params.startOffset + params.startSearchLength;

    size_t bestStart = params.startOffset;
    size_t bestLength = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    for (size_t start = params.startOffset; start <= lastStart; start += startStep) {
        // Each candidate must have `window` samples after its end.
        const size_t maxCandidate = std::min(w.maxLength, source.size() - start - window);
        for (size_t length = w.minLength; length <= maxCandidate; ++length) {
            const double score = normalizedCrossCorrelation(
                source.data() + start, source.data() + start + length, window);
            if (score > bestScore) {
                bestScore = score;
                bestStart = start;
                bestLength = length;
            }
        }
    }

    if (bestLength == 0) {
        *error = "loop search window is empty";
        return false;
    }

    const size_t s = bestStart;
    const size_t length = bestLength;
    const size_t crossfade = effectiveCrossfade(params, length);

    const double seamCorrelation = (crossfade > 0)
        ? normalizedCrossCorrelation(source.data() + s, source.data() + s + length, crossfade)
        : bestScore;

    result->samples.assign(source.begin() + s, source.begin() + s + length);

    // Blend the audio that would follow the loop end into the loop start, so
    // playback flows from sample length-1 into sample 0 continuously.
    for (size_t i = 0; i < crossfade; ++i) {
        double fadeIn, fadeOut;
        crossfadeGains(static_cast<double>(i) / crossfade, seamCorrelation, &fadeIn, &fadeOut);
        result->samples[i] = static_cast<float>(
            fadeIn * source[s + i] + fadeOut * source[s + length + i]);
    }

    result->sourceStart = s;
    result->length = length;
    result->crossfadeLength = crossfade;
    result->cycles = w.cycles;
    result->seamCorrelation = seamCorrelation;

    return true;
}

void game_audio::removeMean(std::vector<float> &samples) {
    if (samples.empty()) return;

    double mean = 0.0;
    for (const float s : samples) mean += s;
    mean /= samples.size();

    for (float &s : samples) s = static_cast<float>(s - mean);
}

void game_audio::dcBlock(std::vector<float> &samples, double sampleRate, double cutoffHz) {
    if (samples.empty()) return;

    // y[n] = x[n] - x[n-1] + R * y[n-1]
    const double r = std::exp(-2.0 * Pi * cutoffHz / sampleRate);

    // Start from the first sample to avoid a step at the beginning.
    double prevX = samples[0];
    double prevY = 0.0;
    for (float &s : samples) {
        const double x = s;
        const double y = x - prevX + r * prevY;
        prevX = x;
        prevY = y;
        s = static_cast<float>(y);
    }
}

void game_audio::applyFadeIn(std::vector<float> &samples, size_t length) {
    length = std::min(length, samples.size());
    for (size_t i = 0; i < length; ++i) {
        const double g = 0.5 - 0.5 * std::cos(Pi * i / length);
        samples[i] = static_cast<float>(samples[i] * g);
    }
}

void game_audio::applyFadeOut(std::vector<float> &samples, size_t length) {
    length = std::min(length, samples.size());
    const size_t start = samples.size() - length;
    for (size_t i = 0; i < length; ++i) {
        const double g = 0.5 + 0.5 * std::cos(Pi * (i + 1) / length);
        samples[start + i] = static_cast<float>(samples[start + i] * g);
    }
}

float game_audio::peakAbs(const std::vector<float> &samples) {
    float peak = 0.0f;
    for (const float s : samples) peak = std::max(peak, std::abs(s));
    return peak;
}

double game_audio::rms(const std::vector<float> &samples) {
    if (samples.empty()) return 0.0;

    double sum = 0.0;
    for (const float s : samples) sum += static_cast<double>(s) * s;
    return std::sqrt(sum / samples.size());
}

void game_audio::applyGain(std::vector<float> &samples, double gain) {
    for (float &s : samples) s = static_cast<float>(s * gain);
}

double game_audio::linearToDb(double linear) {
    if (linear <= 0.0) return -std::numeric_limits<double>::infinity();
    return 20.0 * std::log10(linear);
}

double game_audio::dbToLinear(double db) {
    return std::pow(10.0, db / 20.0);
}
