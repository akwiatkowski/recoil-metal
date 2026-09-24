#include "core/audio/Curve.hpp"

#include <algorithm>
#include <cmath>

namespace rm::audio {

float Curve::evaluate(float param, float defaultValue) const noexcept {
    if (points.empty()) {
        return defaultValue;
    }
    // The clamp-at-endpoints shape: a curve is constant below its first point and
    // above its last. Authored silence at the far end stays silence past the curve,
    // exactly what XACT's RPC does for a distance→volume run that ends in -9600.
    if (param <= points.front().first) {
        return points.front().second;
    }
    if (param >= points.back().first) {
        return points.back().second;
    }
    // Find the segment whose x brackets `param`. Points are sorted ascending; a linear
    // scan is fine because authored curves sit at 2..16 points — XACT's RPC table.
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto [x0, y0] = points[i];
        const auto [x1, y1] = points[i + 1];
        if (param >= x0 && param <= x1) {
            const float span = x1 - x0;
            if (span <= 0.0f) {
                return y0;
            }
            const float frac = (param - x0) / span;
            return y0 + frac * (y1 - y0);
        }
    }
    return points.back().second;
}

float volumeDbToAmplitude(float hundredthsOfDb) noexcept {
    // XACT's wire unit is hundredths of a dB (SupCom.xgs' RPC curves end at -9600 = silence).
    // 10^(dB / 20) is the linear amplitude from dB; dB = hundredths / 100.
    return std::pow(10.0f, hundredthsOfDb / 2000.0f);
}

float filterIdentityHz(float sampleRate) noexcept {
    // No cutoff is "above Nyquist", which for the mixer's 44.1 kHz bus is 22050 Hz.
    // The one-pole filter with cutoff >= Nyquist is the identity transform.
    return sampleRate * 0.5f;
}

} // namespace rm::audio
