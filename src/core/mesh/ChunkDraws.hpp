#pragma once

#include "core/mesh/TerrainMesh.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rm {

// How the terrain is actually drawn: which chunks, at which detail level, and
// how many draw calls that works out to.
//
// This lives in core/ rather than in the renderer for the reason milestone 13
// learned the hard way. A renderer issuing 256 draws where one would do renders
// a pixel-identical image, so a broken merge is invisible on screen and costs
// nothing a screenshot can show. It had been broken for a whole milestone. The
// plan is arithmetic over chunk bounds; nothing here needs a GPU, so nothing
// here should need one to be checked.
struct ChunkDraw {
    std::size_t firstIndex = 0;
    std::size_t indexCount = 0;

    [[nodiscard]] friend bool operator==(const ChunkDraw&, const ChunkDraw&) noexcept = default;
};

/// Where level 0 gives way to level 1, and level 1 to level 2, in chunk widths
/// from the viewpoint.
///
/// Measured, on aw04 (1024 squares, 512-elmo chunks), against the two things a
/// threshold trades off — GPU time at a whole-map framing, and how far the image
/// moves away from full detail. Both were measured for real rather than
/// modelled: `--bench-offscreen` at 1920x1080 with 200 units, and the mean
/// per-channel difference of a 1600x1000 `--screenshot` against the same view
/// with detail levels disabled entirely.
///
///   near/far   whole-map GPU   draws   triangles   whole-map diff   focus-60 diff
///   ---------  --------------  -----   ---------   --------------   -------------
///   off             2.598 ms       1   2 097 152        (baseline)      (baseline)
///   16 / 40         1.693 ms       1     524 288      0.247/255       0.000/255
///   12 / 30         1.723 ms       1     524 288      0.247/255       0.000/255
///   8 / 20          1.510 ms       6     235 520      0.385/255       0.000/255
///   6 / 14          1.380 ms       1     131 072      0.452/255       0.000/255
///   4 / 10          1.384 ms       1     131 072      0.452/255       0.258/255
///
/// Two things in that table are worth more than the numbers themselves.
///
/// The DRAW COUNT barely matters. 8/20 issues six draws to 6/14's one and is
/// still 0.13 ms SLOWER, because it draws 80% more triangles — the terrain at a
/// whole-map framing is vertex-bound, and half a dozen extra draw calls do not
/// register against that. Milestone 13 resurrected the cull-and-merge on the
/// theory that a broken merge was costing real time; it was costing a fraction
/// of what the triangles cost. The merge earns its keep by making a threshold
/// choice nearly free to get wrong, not by being fast in itself.
///
/// And 6 is exactly where the near threshold stops being free. At a mid-range
/// working camera (`--focus 60`) 6 renders an image byte-identical to full
/// detail — no pixel differs by more than 3 — while 4 already moves 1.23% of
/// them, by up to 207. Going below 6 buys nothing anyway: 4/10 measured no
/// faster, because both put the whole overview at level 2 already.
///
/// So: the most aggressive pair that leaves a camera anyone works at untouched.
/// Against no detail levels at all this is 47% of the whole-map frame, for a mean
/// difference of 0.45/255 — under a fifth of one step of an 8-bit channel.
///
/// The thresholds are deliberately NOT derived from a projected-error budget,
/// which was the first attempt. A level-1 chord on this map is 13 elmos out at
/// the median chunk and 92 at the worst, which at 800 pixels of viewport works
/// out to a sub-pixel budget demanding a threshold of ~60 chunk widths — four
/// times the width of the whole map. That model says "never use LOD", and the
/// screenshots say the error is invisible, because it hides almost entirely
/// under ground texture and shadow. The corpus wins over the arithmetic.
inline constexpr float kLodNearChunks = 6.0f;
inline constexpr float kLodFarChunks = 14.0f;

/// Detail level for a chunk whose centre is `distance` from the viewpoint.
///
/// The thresholds are in CHUNK WIDTHS rather than elmos, so one pair of numbers
/// holds whatever a chunk works out to on a given map — 512 elmos on a
/// 1024-square map at full stride, and the same 512 on a 4096-square map
/// decimated to fit, which is the same number of pixels for the same reason.
///
/// See kLodNearChunks/kLodFarChunks for where the two numbers come from; they
/// are a measured trade-off between vertices and draw calls, not a taste.
[[nodiscard]] int lodForDistance(float distance, float chunkWidth) noexcept;

/// The draws for one pass: pick a level per visible chunk, then merge
/// consecutive chunks whose index ranges turn out to be adjacent.
///
/// The merge is not a nicety. Without it, culling replaces one draw of the
/// terrain with one per chunk, and whenever the camera keeps them all that is
/// slower than not culling at all. It works because the index buffer is
/// level-major (see buildTerrainMesh): all chunks' level 0, then all level 1 —
/// so neighbouring chunks ARE adjacent at whichever level they share, and never
/// across a change of level.
///
/// A template rather than a `std::function` pair so the renderer keeps paying
/// nothing per chunk for the indirection: the two passes ask different questions
/// (a view frustum, a light box) and both inline.
///
/// `visible(chunk)` decides what is kept; `levelOf(chunk)` returns the level to
/// draw it at, and may return the same level for every chunk — the shadow pass
/// does, since a shadow is seen only as shade and detail there buys nothing.
template <typename Visible, typename LevelOf>
void appendChunkDraws(std::vector<ChunkDraw>& out, std::span<const TerrainChunk> chunks,
                      Visible visible, LevelOf levelOf) {
    ChunkDraw run{};

    for (const TerrainChunk& chunk : chunks) {
        if (!visible(chunk)) {
            // A gap ends the run: the survivors on either side of a culled
            // chunk are not adjacent in the buffer, whatever their level.
            if (run.indexCount > 0) {
                out.push_back(run);
                run.indexCount = 0;
            }
            continue;
        }

        const int level = levelOf(chunk);
        const TerrainChunk::Lod& range = chunk.lods[static_cast<std::size_t>(level)];

        if (run.indexCount > 0 && run.firstIndex + run.indexCount == range.firstIndex) {
            run.indexCount += range.indexCount;
            continue;
        }

        if (run.indexCount > 0) {
            out.push_back(run);
        }
        run = ChunkDraw{range.firstIndex, range.indexCount};
    }

    if (run.indexCount > 0) {
        out.push_back(run);
    }
}

} // namespace rm
