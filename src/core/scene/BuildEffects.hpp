#pragma once

#include <array>

namespace rm {

/// The two moving endpoints of a UEF construction beam pair.
struct BuildBeamEnds {
    std::array<float, 3> first{};
    std::array<float, 3> second{};
};

/// Places the two UEF beam ends on the nearest edge of the build cube and
/// sweeps them past each other every 0.6 seconds. Extents are the blueprint's
/// full MeshExtents values.
[[nodiscard]] BuildBeamEnds uefBuildBeamEnds(std::array<float, 3> site,
                                             std::array<float, 3> builder,
                                             float extentX, float extentY,
                                             float extentZ, float progress,
                                             float seconds) noexcept;

}  // namespace rm
