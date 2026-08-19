#include "core/map/Scmap.hpp"

#include "core/map/ByteReader.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

namespace {

// A cursor over the file, mirroring the reader the format was decoded with.
//
// Sequential by construction: there is no seek, so `at` is always the truth and
// a wrong field width shows up as the final array missing EOF rather than as
// plausible garbage read from the right offset by luck. Every read that would
// run past the end sets `ok` false and returns a zero — callers check `ok` in
// loop conditions so a corrupt count cannot spin for 2^31 iterations.
class Cursor {
public:
    explicit Cursor(std::span<const std::byte> bytes) noexcept : bytes_{bytes} {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t at() const noexcept { return at_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    [[nodiscard]] bool has(std::size_t count) const noexcept {
        return ok_ && count <= bytes_.size() - std::min(at_, bytes_.size());
    }

    [[nodiscard]] std::uint8_t u8() noexcept {
        if (!has(1)) { ok_ = false; return 0; }
        const auto value = std::to_integer<std::uint8_t>(bytes_[at_]);
        at_ += 1;
        return value;
    }

    [[nodiscard]] std::int32_t i32() noexcept {
        if (!has(4)) { ok_ = false; return 0; }
        const std::int32_t value = rm::readI32(bytes_, at_);
        at_ += 4;
        return value;
    }

    [[nodiscard]] float f32() noexcept {
        if (!has(4)) { ok_ = false; return 0.0f; }
        const float value = rm::readF32(bytes_, at_);
        at_ += 4;
        return value;
    }

    /// Advances over n floats without decoding them — used for the many blocks
    /// whose fields are known but irrelevant to rendering geometry.
    void skipFloats(std::size_t n) noexcept { skip(n * 4); }

    void skip(std::size_t n) noexcept {
        if (!has(n)) { ok_ = false; return; }
        at_ += n;
    }

    /// NUL-terminated string. Every string in the format is one of these except
    /// decal paths, which are length-prefixed and NOT terminated.
    void skipCString() noexcept {
        while (ok_) {
            if (u8() == 0) {
                return;
            }
        }
    }

    /// The same, keeping the text.
    [[nodiscard]] std::string cstring() {
        std::string text;
        while (ok_) {
            const std::uint8_t c = u8();
            if (c == 0) {
                break;
            }
            text.push_back(static_cast<char>(c));
        }
        return text;
    }

    /// uint32-length-prefixed, not NUL-terminated (decal paths only).
    void skipLengthString() noexcept {
        const std::int32_t length = i32();
        if (length < 0) { ok_ = false; return; }
        skip(static_cast<std::size_t>(length));
    }

    /// A block whose length is stated by the int32 immediately before it — how
    /// every embedded DDS is stored.
    void skipSizedBlock() noexcept {
        const std::int32_t length = i32();
        if (length < 0) { ok_ = false; return; }
        skip(static_cast<std::size_t>(length));
    }

    /// The same, keeping the bytes.
    [[nodiscard]] std::vector<std::byte> sizedBlock() {
        const std::int32_t length = i32();
        if (length < 0) { ok_ = false; return {}; }
        const std::span<const std::byte> view = raw(static_cast<std::size_t>(length));
        return std::vector<std::byte>{view.begin(), view.end()};
    }

    /// A view of the next n bytes, advancing past them.
    [[nodiscard]] std::span<const std::byte> raw(std::size_t n) noexcept {
        if (!has(n)) { ok_ = false; return {}; }
        const auto view = bytes_.subspan(at_, n);
        at_ += n;
        return view;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

[[nodiscard]] std::unexpected<rm::MapError> fail(rm::MapError::Code code, std::string message) {
    return std::unexpected{rm::MapError{code, std::move(message)}};
}

/// Reads the sections between the heightmap and the terrain-type array.
///
/// Most of it is still walked rather than kept, but all of it must be walked
/// exactly: the terrain-type array sits behind every one of these, and the whole
/// point of a sequential reader is that there is no offset to jump to. Field
/// widths are from docs/recoil-metal/scmap-v60-format.md, validated to EOF on 60
/// maps.
void readToTerrainType(Cursor& cursor, rm::scmap::Map& map) {
    // --- shader name and environment paths ---------------------------------
    (void)cursor.u8();  // one unexplained byte, 0 on every map checked
    cursor.skipCString();  // shader
    cursor.skipCString();  // background
    cursor.skipCString();  // skycube
    for (std::int32_t i = 0, n = cursor.i32(); cursor.ok() && i < n; ++i) {
        cursor.skipCString();  // envcube name
        cursor.skipCString();  // envcube path
    }

    // --- lighting block ----------------------------------------------------
    // Read rather than skipped: the fog colour is what the sky fades to, and
    // the sun's direction and colour are the map's, not the renderer's.
    const auto vec3 = [&cursor] {
        std::array<float, 3> v{};
        v[0] = cursor.f32();
        v[1] = cursor.f32();
        v[2] = cursor.f32();
        return v;
    };

    map.lighting.multiplier = cursor.f32();
    map.lighting.sunDirection = vec3();
    map.lighting.sunAmbience = vec3();
    map.lighting.sunColour = vec3();
    map.lighting.shadowFill = vec3();
    for (float& channel : map.lighting.specularColour) {
        channel = cursor.f32();
    }
    map.lighting.bloom = cursor.f32();
    map.lighting.fogColour = vec3();
    map.lighting.fogStart = cursor.f32();
    map.lighting.fogEnd = cursor.f32();

    // --- water block -------------------------------------------------------
    map.hasWater = cursor.u8() != 0;
    map.waterElevation = cursor.f32() * rm::scmap::kElmosPerOgrid;
    cursor.skipFloats(2);  // elevation deep, elevation abyss
    // The uniforms effects/water2.fx reads, in the order the file stores them.
    map.water.surfaceColour = vec3();
    map.water.colourLerp[0] = cursor.f32();
    map.water.colourLerp[1] = cursor.f32();
    map.water.refractionScale = cursor.f32();
    map.water.fresnelBias = cursor.f32();
    map.water.fresnelPower = cursor.f32();
    map.water.unitReflection = cursor.f32();
    map.water.skyReflection = cursor.f32();
    map.water.sunShininess = cursor.f32();
    map.water.sunStrength = cursor.f32();
    map.water.sunDirection = vec3();
    map.water.sunColour = vec3();
    map.water.sunReflection = cursor.f32();
    map.water.sunGlow = cursor.f32();
    cursor.skipCString();  // cubemap
    cursor.skipCString();  // water ramp
    // All four wave-repeat scalars come first, THEN four (movement, path)
    // records — not one interleaved struct, which is easy to get backwards.
    cursor.skipFloats(4);
    for (int i = 0; i < 4 && cursor.ok(); ++i) {
        cursor.skipFloats(2);  // movement
        cursor.skipCString();  // texture path
    }

    // --- wave generators ---------------------------------------------------
    for (std::int32_t i = 0, n = cursor.i32(); cursor.ok() && i < n; ++i) {
        cursor.skipCString();  // texture
        cursor.skipCString();  // ramp
        // position (3), rotation, velocity (3), then ten more: lifetime, period,
        // scale, frame count, frame rate, strip count.
        cursor.skipFloats(3 + 1 + 3 + 10);
    }

    // A fixed 28-byte block sits here. It LOOKS length-prefixed — the leading
    // int32 is 24 or 20 depending on the map — but the block is 28 bytes either
    // way, so that value counts something inside the payload rather than the
    // payload itself. The 24 bytes read as RGBA words with alpha 0xff, most
    // likely minimap or terrain-type colours.
    (void)cursor.i32();
    cursor.skip(24);

    // --- stratum table -----------------------------------------------------
    // Fixed-length, NOT count-prefixed: always 10 albedo entries (base + 8
    // strata + macrotexture) then 9 normal entries. Unused slots hold an empty
    // string and a float, so all 19 must be read.
    for (std::size_t i = 0; i < rm::scmap::kAlbedoSlots && cursor.ok(); ++i) {
        map.albedo[i].path = cursor.cstring();
        map.albedo[i].scale = cursor.f32();
    }
    for (std::size_t i = 0; i < rm::scmap::kNormalSlots && cursor.ok(); ++i) {
        map.normals[i].path = cursor.cstring();
        map.normals[i].scale = cursor.f32();
    }
    cursor.skip(8);  // two unidentified int32s

    // --- decals ------------------------------------------------------------
    for (std::int32_t i = 0, n = cursor.i32(); cursor.ok() && i < n; ++i) {
        cursor.skip(8);  // id, type
        for (std::int32_t p = 0, paths = cursor.i32(); cursor.ok() && p < paths; ++p) {
            cursor.skipLengthString();  // the format's only non-terminated strings
        }
        cursor.skipFloats(3 + 3 + 3);  // scale, position, rotation
        cursor.skipFloats(2);          // cutoff LOD, near cutoff LOD
        cursor.skip(4);                // owner army
    }

    // --- decal groups ------------------------------------------------------
    for (std::int32_t i = 0, n = cursor.i32(); cursor.ok() && i < n; ++i) {
        cursor.skip(4);
        cursor.skipCString();
        for (std::int32_t m = 0, members = cursor.i32(); cursor.ok() && m < members; ++m) {
            cursor.skip(4);
        }
    }

    // --- normal map, masks, water map --------------------------------------
    cursor.skip(8);  // dimensions, repeated
    // TILED: the three 4096-square maps store four 2048 tiles here and every
    // other size stores one. That single count is what a naive parser trips on.
    for (std::int32_t i = 0, n = cursor.i32(); cursor.ok() && i < n; ++i) {
        map.normalMapTiles.push_back(cursor.sizedBlock());
    }
    map.maskA = cursor.sizedBlock();  // strata 1-4
    map.maskB = cursor.sizedBlock();  // strata 5-8
    cursor.skip(4);           // an int32 that is 1 on all 60 maps
    cursor.skipSizedBlock();  // water map — present even on maps with no water
}

} // namespace

namespace rm::scmap {

bool looksLikeScmap(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(kMagic)) {
        return false;
    }
    return std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0;
}

std::expected<Map, MapError> load(std::span<const std::byte> bytes) {
    if (!looksLikeScmap(bytes)) {
        return fail(MapError::Code::NotScmap,
                    "not a Supreme Commander map: expected the 4-byte magic \"Map\\x1a\"");
    }

    Cursor cursor{bytes};
    cursor.skip(sizeof(kMagic));

    // --- header ------------------------------------------------------------
    const std::int32_t versionMajor = cursor.i32();
    cursor.skip(4);        // sentinel
    cursor.skip(4);        // unknown
    cursor.skipFloats(2);  // map size as floats, restated authoritatively below
    cursor.skip(4);        // unknown
    cursor.skip(2);        // unknown int16

    cursor.skipSizedBlock();  // preview image, always 256x256 RGBA8

    // Checked here rather than after the version: a file cut inside the preview
    // leaves every later read returning zero, and reporting that as "version 0
    // unsupported" would send anyone debugging it in the wrong direction.
    if (!cursor.ok()) {
        return fail(MapError::Code::Truncated,
                    "file ends inside the .scmap preview image");
    }

    const std::int32_t versionMinor = cursor.i32();
    if (versionMinor != kVersionMinor) {
        return fail(MapError::Code::BadHeader,
                    "unsupported .scmap version " + std::to_string(versionMajor) + "."
                        + std::to_string(versionMinor) + " (this reader decodes retail v"
                        + std::to_string(kVersionMinor) + ")");
    }

    // --- heightmap ---------------------------------------------------------
    const std::int32_t squaresX = cursor.i32();
    const std::int32_t squaresZ = cursor.i32();
    const float heightScale = cursor.f32();

    if (!cursor.ok()) {
        return fail(MapError::Code::Truncated, "file ends inside the .scmap header");
    }
    if (squaresX <= 0 || squaresZ <= 0) {
        return fail(MapError::Code::BadGeometry,
                    "nonsensical heightmap dimensions " + std::to_string(squaresX) + " x "
                        + std::to_string(squaresZ));
    }
    if (squaresX > kMaxSquares || squaresZ > kMaxSquares) {
        return fail(MapError::Code::TooLarge,
                    std::to_string(squaresX) + " x " + std::to_string(squaresZ)
                        + " squares exceeds the " + std::to_string(kMaxSquares)
                        + "-square limit: its heightfield alone would need roughly "
                        + std::to_string((static_cast<std::size_t>(squaresX + 1)
                                          * static_cast<std::size_t>(squaresZ + 1) * 2)
                                         / (1024 * 1024))
                        + " MiB of vertices alone, and there is no LOD yet");
    }

    Map map;
    map.field.squaresX = squaresX;
    map.field.squaresZ = squaresZ;
    // No mapinfo.lua equivalent exists and no stock map varies it, so unlike SMF
    // there is nothing that can override this later — see HeightField.hpp on why
    // setVerticalRange is not used here.
    map.field.baseHeight = 0.0f;
    map.field.heightScale = heightScale * kElmosPerOgrid;

    const std::size_t samples = map.field.sampleCount();
    const std::span<const std::byte> raw = cursor.raw(samples * 2);
    if (raw.empty()) {
        return fail(MapError::Code::Truncated,
                    "heightmap wants " + std::to_string(samples)
                        + " samples but the file ends first");
    }

    map.field.raw.resize(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        map.field.raw[i] = readU16(raw, i * 2);
    }

    // --- everything between, walked exactly --------------------------------
    readToTerrainType(cursor, map);

    // Three uint8 masks at half resolution: foam, flatness, depth bias.
    const auto halfX = static_cast<std::size_t>(squaresX / 2);
    const auto halfZ = static_cast<std::size_t>(squaresZ / 2);
    cursor.skip(halfX * halfZ * 3);

    // --- terrain type ------------------------------------------------------
    map.typesX = squaresX;
    map.typesZ = squaresZ;
    const std::size_t typeCount =
        static_cast<std::size_t>(squaresX) * static_cast<std::size_t>(squaresZ);
    const std::span<const std::byte> types = cursor.raw(typeCount);
    if (types.empty()) {
        return fail(MapError::Code::Truncated,
                    "file ends before the terrain-type array; a section above it was "
                    "decoded at the wrong width");
    }

    map.terrainType.resize(typeCount);
    for (std::size_t i = 0; i < typeCount; ++i) {
        map.terrainType[i] = std::to_integer<std::uint8_t>(types[i]);
    }

    // --- skybox -------------------------------------------------------------
    // Most of this is geometry for a sky dome this renderer does not build, and is
    // walked rather than read — which is also what makes endsExactlyAtEof mean
    // something, since a wrong field width here shifts everything after it.
    //
    // Two colours out of it are worth having, because without them every map's sky
    // is the same pair of constants.
    cursor.skipFloats(3 + 1 + 1 + 1);  // position, horizon height, scale, sub height
    cursor.skip(8);                    // subdivision counts
    cursor.skipFloats(1 + 7);          // mid radius, seven unidentified floats
    cursor.skipCString();              // albedo
    cursor.skipCString();              // glow
    for (std::int32_t i = 0, n = cursor.i32(); cursor.ok() && i < n; ++i) {
        cursor.skipFloats(10);  // planet: position, rotation, size, uv
    }

    // The mid colour, and the one place in this file a colour is BYTES rather than
    // floats — four of them, of which the fourth is unused.
    // THREE bytes, not four, and the difference was hiding in plain sight: the walk
    // that stood here read four and still landed on EOF, because the extra byte was
    // eaten by the cirrus texture path that follows — a C string swallows a leading
    // byte and still terminates at the same NUL, so the total came out right while
    // every field in between was one byte out. The floats after it read as 1e23 and
    // the colour as black on all 60 maps, which is what finally gave it away.
    const std::span<const std::byte> midColour = cursor.raw(3);
    if (midColour.size() == 3) {
        map.sky.midColour = {std::to_integer<int>(midColour[0]) / 255.0f,
                             std::to_integer<int>(midColour[1]) / 255.0f,
                             std::to_integer<int>(midColour[2]) / 255.0f};
    }

    map.sky.cirrusMultiplier = cursor.f32();
    for (float& channel : map.sky.cirrusColour) {
        channel = cursor.f32();
    }
    map.sky.present = cursor.ok();

    cursor.skipCString();      // cirrus texture
    for (std::int32_t i = 0, n = cursor.i32(); cursor.ok() && i < n; ++i) {
        cursor.skipFloats(5);  // frequency x2, speed, direction x2
    }
    cursor.skipFloats(1);

    // --- props --------------------------------------------------------------
    // Trees, rocks and wrecks. Kept rather than skipped: this is the section
    // that decides whether a map looks inhabited, and there is nothing else in
    // either format that carries it.
    //
    // The count is trusted only as far as the file goes — `cursor.ok()` stops
    // the loop at EOF, so a corrupt count reads what is there and reports the
    // shortfall through endsExactlyAtEof rather than allocating on a bad number.
    const std::int32_t propCount = cursor.i32();
    if (propCount > 0 && cursor.ok()) {
        map.props.reserve(static_cast<std::size_t>(propCount));
    }
    for (std::int32_t i = 0; cursor.ok() && i < propCount; ++i) {
        Prop prop;
        prop.blueprint = cursor.cstring();

        // Positions are ogrids like every other coordinate in the file, and the
        // heightmap has already been scaled to elmos — a prop left in ogrids
        // lands at an eighth of its distance from the origin, which reads as
        // every tree on the map bunched into one corner.
        for (float& axis : prop.position) {
            axis = cursor.f32() * kElmosPerOgrid;
        }
        for (std::array<float, 3>* basis : {&prop.rotationX, &prop.rotationY, &prop.rotationZ}) {
            for (float& component : *basis) {
                component = cursor.f32();
            }
        }
        // Scale is a ratio, so it does NOT take the ogrid conversion. Applying
        // it here as well would make every prop eight times its size — which
        // looks like a mesh-units problem and is not one.
        for (float& axis : prop.scale) {
            axis = cursor.f32();
        }

        if (cursor.ok()) {
            map.props.push_back(std::move(prop));
        }
    }

    map.endsExactlyAtEof = cursor.ok() && cursor.at() == cursor.size();
    return map;
}

std::expected<Map, MapError> loadFile(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return fail(MapError::Code::Truncated, "could not open \"" + path.string() + "\"");
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return fail(MapError::Code::Truncated, "\"" + path.string() + "\" is empty");
    }
    file.seekg(0);

    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    // reinterpret_cast to char*: istream::read has no std::byte overload, and
    // char is the one type allowed to alias any object representation.
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) {
        return fail(MapError::Code::Truncated, "short read on \"" + path.string() + "\"");
    }

    return load(bytes);
}

} // namespace rm::scmap
