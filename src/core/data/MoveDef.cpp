#include "core/data/MoveDef.hpp"

namespace rm::data {

MoveDef moveDefFor(unitdef::MotionType motion) noexcept {
    using unitdef::MotionType;

    // Recoil's own MoveDef defaults where the two games agree, ours where they do not — each
    // line says which. See the header for the citations.
    switch (motion) {
    case MotionType::None:
        // Immobile. No MoveDef, and `usesGroundGrid` is false so a caller that asks anyway gets
        // "cannot route" rather than a grid it will misuse.
        return MoveDef{.maxSlopeDegrees = 0.0f, .maxWaterDepthElmos = 0.0f,
                       .usesGroundGrid = false};

    case MotionType::Land:
        // 60 degrees is Recoil's Tank/KBot default (`MoveDefHandler.cpp:84-95`). The DEPTH is
        // ours: Recoil defaults tanks to 1e6 elmos — any depth — which is right for a game whose
        // tanks ford rivers and wrong for this one, where `RULEUMT_Land` means dry land and
        // amphibious is a separate class the corpus uses heavily. Zero is what `Land` MEANS here.
        return MoveDef{.maxSlopeDegrees = 60.0f, .maxWaterDepthElmos = 0.0f};

    case MotionType::Hover:
        // 15 degrees is Recoil's Hover default (`MoveDefHandler.cpp:84-95`) — a hovercraft
        // cannot climb what a tank can. Depth is irrelevant: it is over the surface, not in it,
        // so any depth passes.
        return MoveDef{.maxSlopeDegrees = 15.0f, .maxWaterDepthElmos = 1.0e6f};

    case MotionType::Amphibious:
        // Walks the seabed. Recoil states no amphibious default because the class does not exist
        // there; the slope matches Land because the same legs climb the same hills, and the depth
        // is unlimited because walking under water is the whole point.
        //
        // ALL FOUR FACTIONS' ACUs ARE THIS (`14 §9.1`), so it is a fixed cost rather than a
        // later feature — the first unit of every match needs it.
        return MoveDef{.maxSlopeDegrees = 60.0f, .maxWaterDepthElmos = 1.0e6f};

    case MotionType::AmphibiousFloating:
        // Floats across rather than walking under. Same reach, different animation — and the
        // difference is presentation until buoyancy exists.
        return MoveDef{.maxSlopeDegrees = 60.0f, .maxWaterDepthElmos = 1.0e6f};

    case MotionType::Air:
        // No ground involved at all.
        return MoveDef{.maxSlopeDegrees = 0.0f, .maxWaterDepthElmos = 0.0f,
                       .usesGroundGrid = false};

    case MotionType::Water:
    case MotionType::SurfacingSub:
        // These need the INVERSE of the ground grid — water deep enough rather than shallow
        // enough — which this engine cannot yet express. Refused rather than approximated:
        // routing a ship over the ground grid would path it across dry land.
        return MoveDef{.maxSlopeDegrees = 0.0f, .maxWaterDepthElmos = 0.0f,
                       .usesGroundGrid = false};
    }

    return MoveDef{.maxSlopeDegrees = 0.0f, .maxWaterDepthElmos = 0.0f,
                   .usesGroundGrid = false};
}

MoveDef moveDefFor(const unitdef::UnitDef& def) noexcept {
    // THE CORRECTION. The unit's own `maxSlopeDegrees` and `maxWaterDepthElmos` are not read —
    // they govern building placement (`01 §3.2`), and reading them for routing was asking a
    // question about foundations and using the answer for legs.
    return moveDefFor(def.motion);
}

bool canCrossWater(const unitdef::UnitDef& def) noexcept {
    const MoveDef move = moveDefFor(def);
    return move.usesGroundGrid && move.maxWaterDepthElmos > 0.0f;
}

} // namespace rm::data
