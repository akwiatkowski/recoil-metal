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
                                             std::array<float, 3> builder, float extentX,
                                             float extentY, float extentZ, float progress,
                                             float seconds) noexcept;

/// Cybran's per-bone beam end (`CreateCybranBuildBeams` in retail
/// `lua/EffectUtilities.lua`): `unitBeingBuilt:GetPosition()` plus
/// `Unit.lua:GetRandomOffset` at scalar 1 — the target's `SizeX/SizeY/SizeZ`
/// (elmos here) scaled, three unit draws, rotated by the builder heading with
/// retail's own signs (`x = cos*rx - sin*rz`, `z = sin*rx - cos*rz`), and the
/// `CollisionOffsetY` lift. `draws` are three presentation-Lua uniforms in
/// [0,1]; `headingRadians` is the builder's yaw; `collisionOffsetYElmos` is zero
/// when the blueprint states no offset. Returns the offset from the target pad.
[[nodiscard]] std::array<float, 3> cybranBuildBeamTarget(float sizeXElmos, float sizeYElmos,
                                                         float sizeZElmos, float headingRadians,
                                                         float collisionOffsetYElmos,
                                                         float drawX, float drawY,
                                                         float drawZ) noexcept;

/// Aeon's engineer beam end (`CreateAeonCommanderBuildingEffects` in retail
/// `lua/EffectUtilities.lua`): the `beamEnd` entity is warped to the target's
/// position, so every bone's beam lands at the pad — no sweep, no offset.
[[nodiscard]] std::array<float, 3> aeonBuildBeamEnd(float siteX, float siteY,
                                                    float siteZ) noexcept;



}  // namespace rm
