#pragma once

#include "core/map/HeightField.hpp"

namespace rm {

// A smooth two-axis sine field spanning the full uint16 domain.
//
// This exists so the app has something to render before a real map is on disk,
// and so the loader tests have a fixture with actual relief. It lives in core/
// rather than in the test support code because both need it and duplicating a
// height generator is how the two quietly drift apart.
//
// squaresX/squaresZ are square counts, so the grid produced is
// (squaresX + 1) x (squaresZ + 1) — the same corner convention as SMF.
[[nodiscard]] HeightField makeSineHills(int squaresX, int squaresZ,
                                        float minHeight, float maxHeight);

} // namespace rm
