#include "../../include/game_audio/loudness.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

    constexpr double Pi = 3.14159265358979323846;

    struct Biquad {
        double b0, b1, b2, a1, a2;
        double z1 = 0.0, z2 = 0.0;

        double process(double x) {
            // Transposed direct form II
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    // The BS.1770 K-weighting filters are specified as 48 kHz coefficients.
    // These are the analog prototype parameters that reproduce them, which
    // lets the filters be derived for any sample rate with the bilinear
    // transform (same approach as libebur128 / pyloudnorm).
    Biquad highShelf(double fs) {
        const double f0 = 1681.974450955533;
        const double gainDb = 3.999843853973347;
        const double q = 0.7071752369554196;

        const double k = std::tan(Pi * f0 / fs);
        const double vh = std::pow(10.0, gainDb / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;

        Biquad f;
        f.b0 = (vh + vb * k / q + k * k) / a0;
        f.b1 = 2.0 * (k * k - vh) / a0;
        f.b2 = (vh - vb * k / q + k * k) / a0;
        f.a1 = 2.0 * (k * k - 1.0) / a0;
        f.a2 = (1.0 - k / q + k * k) / a0;
        return f;
    }

    Biquad highPass(double fs) {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;

        const double k = std::tan(Pi * f0 / fs);
        const double a0 = 1.0 + k / q + k * k;

        Biquad f;
        f.b0 = 1.0;
        f.b1 = -2.0;
        f.b2 = 1.0;
        f.a1 = 2.0 * (k * k - 1.0) / a0;
        f.a2 = (1.0 - k / q + k * k) / a0;
        return f;
    }

    double toLufs(double meanSquare) {
        return -0.691 + 10.0 * std::log10(meanSquare);
    }

} /* namespace */

double game_audio::integratedLoudness(const std::vector<float> &samples, double sampleRate) {
    if (samples.empty()) return -std::numeric_limits<double>::infinity();

    Biquad shelf = highShelf(sampleRate);
    Biquad hp = highPass(sampleRate);

    std::vector<double> squared(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        const double y = hp.process(shelf.process(samples[i]));
        squared[i] = y * y;
    }

    const size_t blockLength = static_cast<size_t>(std::lround(0.4 * sampleRate));
    const size_t hop = blockLength / 4;

    std::vector<double> blocks;
    if (squared.size() <= blockLength) {
        double sum = 0.0;
        for (const double v : squared) sum += v;
        blocks.push_back(sum / squared.size());
    }
    else {
        // Running sum over the sliding window.
        double sum = 0.0;
        for (size_t i = 0; i < blockLength; ++i) sum += squared[i];
        for (size_t start = 0;; start += hop) {
            blocks.push_back(sum / blockLength);
            if (start + hop + blockLength > squared.size()) break;
            for (size_t i = 0; i < hop; ++i) {
                sum += squared[start + blockLength + i] - squared[start + i];
            }
            sum = std::max(0.0, sum);
        }
    }

    constexpr double AbsoluteGate = -70.0;
    double sum = 0.0;
    size_t count = 0;
    for (const double z : blocks) {
        if (z > 0.0 && toLufs(z) > AbsoluteGate) {
            sum += z;
            ++count;
        }
    }
    if (count == 0) return -std::numeric_limits<double>::infinity();

    const double relativeGate = toLufs(sum / count) - 10.0;
    sum = 0.0;
    count = 0;
    for (const double z : blocks) {
        if (z > 0.0) {
            const double l = toLufs(z);
            if (l > AbsoluteGate && l > relativeGate) {
                sum += z;
                ++count;
            }
        }
    }
    if (count == 0) return -std::numeric_limits<double>::infinity();

    return toLufs(sum / count);
}
