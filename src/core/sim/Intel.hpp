#pragma once

#include "core/Types.hpp"
#include "core/sim/Fx.hpp"
#include "core/sim/UnitCatalog.hpp"
#include "core/sim/IdPool.hpp"
#include "core/sim/TickRate.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rm::sim {

class Terrain;
class UnitStore;
class UnitCatalog;
struct Army;

// What an alliance can see, and by what means (ADR-037).
//
// WHY THIS EXISTS. Nothing in this engine knew: `nearestTarget` picked from the whole unit
// store filtered by hostility and range, so every unit shot at things it could not possibly
// see, and the minimap said "no fog of war" in a comment because there was none to draw.
//
// THE TWO ENGINES DISAGREE ABOUT WHAT THE FEATURE IS, which is the fact that shaped this
// file. Recoil raycasts terrain for sight and radar (`LosMap.cpp:525`, an angle/horizon scan)
// and takes circles for everything else. Supreme Commander does not consider terrain at all:
// `effects/vision.fx:35` is `radius * vertex.xz + position.xy`, a flat disc with no height
// input and no heightmap sample anywhere in the file. So the SHAPE is a parameter here and
// the grid underneath it is common — see `VisionStyle`.
//
// THE STRUCTURE IS RECOIL'S AND THE IMPORTANT PART IS SMALLER THAN THE RAYCAST: a square
// holds a **reference count, not a flag** (`ILosType`, LosHandler.h:84-88). Withdrawal is
// most of what an intel system does — units move every tick — and with a flag one unit's
// sight cannot be taken back without re-deriving every other unit's. With a count, add and
// remove are +1 and -1 over the same squares and each unit's contribution is independent.
//
// PER ALLIANCE, not per army, because `Army.hpp` already named `AllianceIndex` as "who wins
// together, and later who shares vision". This is later.

/// The senses this engine models, and the order they are stored in.
///
/// FOUR. `UnitDef` also carries a water vision radius, which Forged Alliance treats as its
/// own sense — but nothing in this sim is submerged yet, so a fifth grid would have no
/// target to answer about and no test that could tell it from an empty one. It joins these
/// when there is something under the water to see.
///
/// Recoil's other four types — air LOS, seismic, and the two jammers — are absent for
/// related reasons: air LOS needs a distinction between flying and grounded units that the
/// coverage query does not yet make, and a jammer needs the contact rules of ADR-037's
/// deferred half. FA's stealth FIELDS are absent for a third reason worth keeping separate:
/// they act on OTHER units, so they are a second kind of grid — "who is hidden here" rather
/// than "who can see here" — and every grid in this file answers the second question.
enum class IntelKind : std::uint8_t {
    Vision,
    Radar,
    Sonar,

    /// Sees everything in its radius, cloaked and stealthed alike. 17 units declare a radius.
    ///
    /// A GRID OF ITS OWN rather than a flag on the vision grid, because it answers a
    /// different question at contact time: vision asks "is this square lit", omni asks "is
    /// this square lit by something nothing can hide from". Merging them would mean either
    /// stealth defeats omni or stealth defeats nothing.
    Omni,
};

inline constexpr std::size_t kIntelKindCount = 4;

/// The second kind of grid ADR-037's second pass named: "who is HIDDEN here", not "who can
/// see here". A stealth field acts on OTHER units — everything of its own alliance standing
/// inside it is absent from the named sense — so the grid is keyed by the FIELD OWNER'S
/// alliance and consulted about a unit's position, where every `IntelKind` grid is keyed by
/// the viewer's. Seven retail units declare each radius (the B4203 stealth generators);
/// CloakField appears zero times in retail and so has no entry here.
enum class HiddenKind : std::uint8_t {
    RadarField,
    SonarField,
};

inline constexpr std::size_t kHiddenKindCount = 2;

/// Whether terrain blocks sight, which is a question about which game this is.
enum class VisionStyle : std::uint8_t {
    /// Flat discs for everything, as `vision.fx` stamps them. You see over mountains.
    ForgedAlliance,

    /// Sight and radar are raycast against the ground; sonar stays a disc, as it is in
    /// `LosHandler.cpp:92`. Hills block, and high ground is worth taking.
    Recoil,
};

/// Elmos across one square at mip 0 — Recoil's `SQUARE_SIZE`, and the resolution its
/// heightmap is stated in. A grid's own square is this shifted left by its mip level, the
/// same `mipDiv = SQUARE_SIZE * (1 << mipLevel)` (`LosHandler.cpp:87`).
inline constexpr std::int32_t kElmosPerSquare = 8;

// One alliance's coverage of one sense: a reference count per square.
//
// SIZED IN SQUARES, NOT ELMOS, and every operation below works in square coordinates. That
// is Recoil's choice too and the reason is arithmetic rather than memory: a circle
// rasterised in integer squares is the same circle on every machine, whereas one rasterised
// against fixed-point elmos would depend on where the rounding fell.
class IntelGrid {
public:
    /// What `squareAt` answers for a position that is not on the map.
    ///
    /// Not the nearest square. Clamping would give a unit standing past the border sight
    /// inside it, and "off the map" is a real state during a spawn.
    static constexpr std::int32_t kNoSquare = -1;

    IntelGrid() = default;

    /// `mipLevel` coarsens the grid: 0 is one square per 8 elmos, 2 is one per 32.
    ///
    /// Recoil keeps sight and radar at different levels (`modInfo.losMipLevel` against
    /// `radarMipLevel`) because radar radii are an order larger — the widest in the retail
    /// corpus is 4800 elmos against the widest sight of 800 — and rasterising that at sight's
    /// resolution costs 36x the squares for a sense whose edge nobody can see.
    IntelGrid(Fx widthElmos, Fx depthElmos, int mipLevel);

    [[nodiscard]] int squaresX() const noexcept { return squaresX_; }
    [[nodiscard]] int squaresZ() const noexcept { return squaresZ_; }
    [[nodiscard]] Fx squareElmos() const noexcept { return Fx::fromInt(squareElmos_); }

    /// The square under a world position, or `kNoSquare` off the map.
    [[nodiscard]] std::int32_t squareAt(Fx x, Fx z) const noexcept;

    /// +1 and -1 over every square of a coverage shape.
    ///
    /// The shape is a caller-owned list of square indices — `circleSquares` or
    /// `raycastSquares` fills one — rather than something this class computes, because the
    /// same grid serves both algorithms and because a caller that has to hold the shape
    /// anyway is a caller that can withdraw exactly what it added.
    void add(std::span<const std::int32_t> squares) noexcept;
    void remove(std::span<const std::int32_t> squares) noexcept;

    [[nodiscard]] bool covered(std::int32_t square) const noexcept {
        return square != kNoSquare && counts_[static_cast<std::size_t>(square)] != 0;
    }

    [[nodiscard]] bool covered(Fx x, Fx z) const noexcept { return covered(squareAt(x, z)); }

    /// How many emitters count this square. For tests and for the hash; a caller asking
    /// "can I see this" wants `covered`.
    [[nodiscard]] std::uint16_t count(std::int32_t square) const noexcept {
        return square == kNoSquare ? 0 : counts_[static_cast<std::size_t>(square)];
    }

    /// The whole grid, in row-major square order. What `StateHash` feeds and what a fog
    /// renderer uploads.
    [[nodiscard]] std::span<const std::uint16_t> counts() const noexcept { return counts_; }

private:
    int squaresX_ = 0;
    int squaresZ_ = 0;
    std::int32_t squareElmos_ = kElmosPerSquare;

    /// A REFERENCE COUNT, sized to hold every unit in a match standing on one square.
    /// `uint16` is 65,535 of them; the largest match this engine targets is thousands.
    std::vector<std::uint16_t> counts_;
};

/// The squares a flat disc of `radius` about (x, z) covers, appended to `squares`.
///
/// Forged Alliance's whole vision model, and Recoil's for the senses it does not raycast.
/// The rasteriser is Recoil's midpoint circle (`LosMap.cpp:74-103`) — integer arithmetic
/// only, and it emits each row exactly once, which matters because a duplicated row would
/// double every count in it. Clipped at the map edge rather than wrapped.
///
/// A radius under one square still covers the square the emitter stands on: 8 blueprints
/// state a sight radius that small, and a unit that cannot see its own feet is a worse
/// reading of the file than one that sees a single square.
void circleSquares(const IntelGrid& grid, Fx x, Fx z, Fx radius,
                   std::vector<std::int32_t>& squares);

/// The squares an emitter at `eyeHeight` can see over the ground, appended to `squares`.
///
/// Recoil's LOS, and the shape of it is simpler than its reputation: **start from the disc
/// and subtract what the ground hides.** Rays are cast outward from the centre; along each
/// one a running maximum angle rises as the ray climbs, and a square whose own angle falls
/// below that maximum is behind a crest and gets struck off.
///
/// Transcribed from `LosMap.cpp` — `CastLos` at 525, the angle definition at 654, the ray
/// construction at 181-309 — with four differences worth stating rather than hiding:
///
///   - **Heights are sampled, not mipped.** Recoil reads a pre-reduced heightmap at the
///     grid's own mip level; we sample the real terrain at each square's centre. Identical
///     at mip 0 and an approximation above it, in the direction of detail rather than away.
///   - **No angle table and no instance cache.** Both are Recoil making the same
///     computation cheap across thousands of units; ours runs on `SlowUpdate`'s cadence and
///     only for emitters that moved.
///   - **The eye height is not bucketed.** Recoil rounds it into buckets so two units at
///     similar heights can share one cached instance. We share nothing, so rounding the
///     input would lose accuracy and buy nothing.
///   - **Fixed point throughout**, where Recoil uses floats and a reciprocal-square-root
///     table. `fxSqrt` is exact on every machine and a `libm` call is not (`Fx.hpp`).
///
/// The units of the angle are mixed on purpose, exactly as the original's are: a height
/// difference in ELMOS over a distance in SQUARES. Every comparison happens within one
/// grid, so the scale cancels — but it does mean the sight bonus below is worth more on a
/// coarse grid than a fine one, which is Recoil's behaviour too.
void raycastSquares(const IntelGrid& grid, const Terrain& terrain, Fx x, Fx z, Fx radius,
                    Fx eyeHeight, std::vector<std::int32_t>& squares);

/// What `kind` covers under `style`: the dispatch ADR-037's setting comes down to.
///
/// Recoil raycasts sight and radar and leaves sonar a disc (`LosHandler.cpp:92`); Forged
/// Alliance stamps discs for all three. A null `terrain` is a scene with no ground to
/// consult — a `--units` crowd on procedural terrain — and falls back to the disc, which is
/// the honest answer rather than a raycast against a heightmap that is not there.
void intelSquares(const IntelGrid& grid, const Terrain* terrain, VisionStyle style,
                  IntelKind kind, Fx x, Fx z, Fx radius, Fx eyeHeight,
                  std::vector<std::int32_t>& squares);

/// Sight's grid resolution, and radar's — Recoil's `losMipLevel` and `radarMipLevel`
/// defaults (`ModInfo.cpp:117-119`), which are 1 and 2.
///
/// Radar is coarser because radar radii are an order larger: the widest in the retail
/// corpus is 4800 elmos against the widest sight of 800, and rasterising that at sight's
/// resolution would cost sixteen times the squares to place an edge nobody can see.
inline constexpr int kVisionMipLevel = 1;
inline constexpr int kRadarMipLevel = 2;

/// A radar source's last confirmed position for one viewing alliance.
///
/// This belongs to Intel rather than the source unit: once the source dies, the viewer still
/// has a blip but cannot know whether that blip is a wreck or a live unit. No expiry policy is
/// defined yet, so entries remain until a future recon rule explicitly reaps them.
struct RetainedRadarContact {
    UnitId unit;
    Fx x{};
    Fx z{};
    bool maybeDead = false;
};

// The pass: every unit's coverage, kept up to date as the match moves.
//
// WHAT IT OWNS: one grid per alliance per sense, and one record per unit slot of what that
// unit last contributed. The record is the whole trick — a unit's sight is withdrawn by
// subtracting the exact squares it added, so nothing has to be re-derived from its
// neighbours and nothing drifts over a long match.
//
// WHEN A UNIT RE-STAMPS: when the SQUARE it stands on changes, or when it dies or spawns.
// Not every tick — a unit crossing a 16-elmo square at 27 elmos a second re-stamps about
// twice a second, and stamping every tick would do the same work ten times over. Recoil
// makes the same call (`CLosHandler::Update` processes a queue of units that moved) and
// the resolution of the answer is the grid's own square either way.
//
// AND NOT ON `SlowUpdate`'s CADENCE, which is what this file's plan item asked for. A
// staggered pass would leave a unit's sight up to a second stale — long enough for a scout
// to cross two squares — and the check that decides whether to re-stamp is one integer
// comparison per slot. The cost being paid is the stamping, and the stagger would not have
// saved any of it.
class Intel {
public:
    Intel() = default;

    /// Sizes the grids for a map and an alliance count. Idempotent, and clears everything:
    /// a reconfigure is a new match, not an adjustment to one in progress.
    void configure(std::size_t alliances, Fx widthElmos, Fx depthElmos, VisionStyle style);

    /// Whether anything has been configured. False is a scene with no intel at all — a
    /// `--units` crowd — and every query then answers "seen", which is what an engine with
    /// no fog of war did before this existed.
    [[nodiscard]] bool active() const noexcept { return !grids_.empty(); }

    [[nodiscard]] VisionStyle style() const noexcept { return style_; }

    /// Brings every unit's contribution up to date. Call after movement and before
    /// anything reads visibility.
    ///
    /// `terrain` may be null, which forces discs — see `intelSquares`.
    void update(const UnitStore& store, const UnitCatalog& catalog,
                std::span<const Army> armies, const Terrain* terrain);

    /// Whether `alliance` covers this position with this sense.
    ///
    /// TRUE WHEN INACTIVE. A caller that has not configured intel is a caller with no fog
    /// of war, and answering "no" there would blind every unit in every test and every
    /// scene that predates this file.
    [[nodiscard]] bool sees(int alliance, IntelKind kind, Fx x, Fx z) const noexcept;

    /// The grid itself, for the hash and for the fog renderer.
    [[nodiscard]] const IntelGrid& grid(int alliance, IntelKind kind) const noexcept;

    /// Whether `ownerAlliance`'s stealth field of this kind covers a position — asked about
    /// a UNIT'S OWN location at contact time, which is the direction these grids point.
    /// False when inactive: no intel means no fog, and no fog means nothing to hide in.
    [[nodiscard]] bool hiddenBy(int ownerAlliance, HiddenKind kind, Fx x, Fx z) const noexcept;

    /// The hidden-field grid, for the hash — the sense grids' sibling.
    [[nodiscard]] const IntelGrid& hiddenGrid(int alliance, HiddenKind kind) const noexcept;

    [[nodiscard]] std::size_t alliances() const noexcept { return grids_.size() / kIntelKindCount; }

    /// Retained radar knowledge for the state hash and contact projection.
    [[nodiscard]] std::span<const RetainedRadarContact> retainedRadarContacts(
        int alliance) const noexcept;

    /// Whether this alliance has visually identified this exact unit generation. Retail's
    /// `RECON_LOSEver` lets a radar return participate in acquisition but withholds authored
    /// category priorities until this latch is set.
    [[nodiscard]] bool hasSeenEver(int alliance, UnitId unit) const noexcept;

    /// Per-slot identity latches for the state hash. A stored generation makes a retired slot
    /// harmless when its index is reused by a different unit.
    [[nodiscard]] std::span<const UnitId> seenEver(int alliance) const noexcept;

private:
    /// What one unit last contributed, one entry per sense.
    struct Emitter {
        /// The squares added, so exactly those can be taken back.
        std::vector<std::int32_t> squares;
    };

    /// Per slot: which alliance it stamped for, which square it stood on, and whether it
    /// stamped at all. `kNoSquare` means "contributing nothing", which is both a fresh slot
    /// and a dead one.
    struct Placement {
        std::int32_t square = IntelGrid::kNoSquare;
        int alliance = 0;
    };

    void withdraw(UnitIndex slot);

    /// `[alliance * kIntelKindCount + kind]`, so one vector holds them all and the index
    /// arithmetic is in one place.
    std::vector<IntelGrid> grids_;

    /// `[alliance * kHiddenKindCount + kind]` — the "hidden here" family, kept apart from
    /// `grids_` because the two are indexed by different parties (owner against viewer) and
    /// one flat vector would invite exactly the off-by-a-kind bug the assertion in
    /// `configure` records.
    std::vector<IntelGrid> hiddenGrids_;

    /// Last radar positions, per viewing alliance. These are authoritative recon knowledge:
    /// `contactsFor` emits an entry after its source is known dead as an uncertain blip.
    std::vector<std::vector<RetainedRadarContact>> retainedRadarContacts_;

    /// `[alliance][slot]` is the unit generation that alliance has seen visually. This is not
    /// presentation history: C-158 uses it to decide whether a current radar contact may match
    /// the weapon's target-priority categories.
    std::vector<std::vector<UnitId>> seenEver_;
    std::vector<Placement> placements_;
    std::vector<std::array<Emitter, kIntelKindCount>> emitters_;
    std::vector<std::array<Emitter, kHiddenKindCount>> hiddenEmitters_;
    // FLAT DISCS UNLESS TOLD OTHERWISE. This engine reads Forged Alliance's content, and that
    // game's own sight is `radius * vertex.xz + position.xy` — a circle, with no heightmap
    // sample anywhere in `effects/vision.fx`. `configure` is what a real scene calls; this
    // default is what a scene that never configures itself gets, and it should agree.
    VisionStyle style_ = VisionStyle::ForgedAlliance;

    /// Scratch, reused across emitters so a stamp is not an allocation.
    std::vector<std::int32_t> scratch_;
};

/// How an alliance knows about a unit — and how much it knows.
enum class ContactKind : std::uint8_t {
    /// Seen. Position exact, identity known.
    Seen,

    /// A radar return. A position, and NOT an identity — the whole point of the distinction.
    Radar,

    /// A sonar return. Same terms as radar, different sense.
    Sonar,
};

/// What one alliance knows about one unit this tick.
struct Contact {
    /// Who it really is. **A caller must not show this for a blip.** It is here because a
    /// renderer needs to key a marker to something stable across ticks, and because the sim
    /// is the wrong layer to enforce a UI rule — but a blip that renders as a named unit
    /// with a health bar is the bug this type exists to make avoidable, not to cause.
    UnitId unit;

    /// Where the alliance believes it is. Exact for `Seen`; offset for a blip.
    Fx x{};
    Fx z{};

    ContactKind kind = ContactKind::Seen;

    [[nodiscard]] bool isBlip() const noexcept { return kind != ContactKind::Seen; }
};

/// Classifies one real unit with the same cloak, stealth-field, omni and free-intel rules used
/// by the contact list. Automatic targeting accepts `Seen` and `Radar`: an unidentified radar
/// return competes at the worst priority rank, while sonar remains anonymous and untargetable.
[[nodiscard]] std::optional<ContactKind> contactKindForUnit(
    int alliance, UnitIndex target, const UnitStore& store, const UnitCatalog& catalog,
    std::span<const Army> armies, const Intel& intel) noexcept;

/// How far a radar contact can be from the truth — Recoil's `defBaseRadarErrorSize`
/// (`LosHandler.h:322`), 96 elmos.
inline constexpr std::int32_t kRadarErrorElmos = 96;

/// How long a blip's error direction holds before it drifts to the next one.
///
/// IN SECONDS, and the first draft of this wrote 15 ticks — which is Recoil's
/// `UNIT_SLOWUPDATE_RATE`, a count of ITS frames at ITS 30 Hz. At our 10 Hz the same number
/// would mean 1.5 seconds, three times the wander it describes, and the comment beside it
/// claimed to have avoided exactly that. `check_no_tick_literals.sh` caught it, which is
/// the whole reason PLAN2 §5.1 asks for a script rather than for care.
inline constexpr Seconds kBlipDriftPeriod = Seconds{0.5f};

/// Everything `alliance` knows about right now, appended to `contacts` in slot order.
///
/// Own and allied units are always `Seen` at their true position. A hostile unit is `Seen`
/// where sight covers it, a blip where only radar or sonar does, and absent otherwise —
/// which is the difference between an intel system and a filter on the draw call.
///
/// THE BLIP ERROR IS DERIVED, NOT STORED. Recoil keeps a `posErrorVector` per unit, drifting
/// toward a fresh random direction every slow update (`Unit.cpp:577-601`). That needs a
/// synced RNG and a per-unit vector in the state hash; ours hashes the unit's identity and
/// the tick bucket into an angle and interpolates between consecutive buckets, which drifts
/// the same way, costs no state at all, and cannot desync because there is nothing to keep
/// in sync. The visible difference is that ours revisits the same wander given the same
/// unit and tick, which for a thing whose whole purpose is to be untrustworthy is not a
/// property anyone can exploit.
void contactsFor(int alliance, const UnitStore& store, const UnitCatalog& catalog,
                 std::span<const Army> armies,
                 const Intel& intel, TickIndex tick, std::vector<Contact>& contacts,
                 TickRate rate = TickRate{});

} // namespace rm::sim
