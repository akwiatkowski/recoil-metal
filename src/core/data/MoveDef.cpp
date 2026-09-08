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
        // Surface ships use the inverse domain: every square under a path cell must be below
        // the waterline. They do not care about seabed slope because they float above it.
        // ponytail: SurfacingSub shares this grid until layer-specific depth footprints exist.
        return MoveDef{.maxSlopeDegrees = 0.0f, .maxWaterDepthElmos = 0.0f,
                        .usesGroundGrid = false, .usesSurfaceWaterGrid = true};

    }

    return MoveDef{.maxSlopeDegrees = 0.0f, .maxWaterDepthElmos = 0.0f,
                   .usesGroundGrid = false};
}

MoveDef moveDefFor(const unitdef::UnitDef& def) noexcept {
    // Naval factories are immobile in the blueprint, but their foundation is the same water
    // domain their products use. Treating MotionType::None literally here leaves placement with
    // no grid and lets an amphibious builder found the yard on land.
    if (def.motion == unitdef::MotionType::None && def.hasCategory("NAVAL")
        && def.hasCategory("FACTORY")) {
        return moveDefFor(unitdef::MotionType::Water);
    }
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
