#pragma once

// Authored curves, evaluator and shapes — XACT RPC's stock piecewise-linear style.
//
// The retail banks describe two curves per cue's sound: a distance→volume curve
// (Xsb.hpp:14-19, measured flat across retail weapon banks, but the shape matters
// when authored) and a low-pass filter cutoff curve (XACT's `Filter` parameter).
// Both share XACT's stock shape: piecewise linear over a 0..1 parameter, the X axis
// a user-meaningful quantity (elmos for distance, a runtime variable for filter) and
// the Y axis linear-in-dB for volume or Hz for cutoff.
//
// An empty curve is the identity: evaluate(0.5) on an empty curve returns 1.0 for a
// volume curve and the full bandwidth for a filter curve. That keeps an author who
// wrote no curve at unity, the same default the unsized byte block has on retail.

#include <cstddef>
#include <vector>

namespace rm::audio {

/// A piecewise-linear curve over [0, 1], linearly interpolated. `points` is sorted by x.
struct Curve {
    /// (x, y) pairs, sorted ascending in x. x in [0, 1]; y is unitless until a
    /// consumer multiplies it (dB for volume, Hz for filter).
    std::vector<std::pair<float, float>> points;

    /// Evaluates the curve at `param` in [0, 1]. Empty curve returns `defaultValue`.
    /// Below the first point the curve holds the first point's y; above the last, the
    /// last point's — XACT's clamp-at-endpoints, so a curve that ends in -9600 yields
    /// -9600 outside its domain, exactly the silence the author wanted.
    [[nodiscard]] float evaluate(float param, float defaultValue) const noexcept;
};

/// A volume curve: y is hundredths of a dB (XACT's wire unit), the public surface
/// returns a linear amplitude multiplier in [0, 1]. Identity = 0 dB (multiplier 1.0).
[[nodiscard]] float volumeDbToAmplitude(float hundredthsOfDb) noexcept;

/// A filter curve: y is the low-pass cutoff in Hz. Identity = the mixer's kSampleRate/2
/// (no cutoff above Nyquist is a no-op filter).
[[nodiscard]] float filterIdentityHz(float sampleRate) noexcept;

} // namespace rm::audio
