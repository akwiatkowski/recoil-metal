#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rm {

// Little-endian scalar reads over a byte span.
//
// Recoil's own loaders read field-by-field through swab*() helpers that are
// no-ops on little-endian hosts (SMFMapFile.cpp:295-312). Doing the same rather
// than memcpy'ing into a struct buys three things: the on-disk layout needs no
// packing pragma or static_assert to stay honest, alignment is irrelevant
// because only bytes are touched, and every read is explicit about width and
// signedness — which -Wconversion enforces.
//
// Callers are responsible for bounds: each loader checks the section it is
// about to read before walking it.

[[nodiscard]] inline std::uint32_t readU32(std::span<const std::byte> b,
                                           std::size_t off) noexcept {
    return std::to_integer<std::uint32_t>(b[off])
         | (std::to_integer<std::uint32_t>(b[off + 1]) << 8)
         | (std::to_integer<std::uint32_t>(b[off + 2]) << 16)
         | (std::to_integer<std::uint32_t>(b[off + 3]) << 24);
}

[[nodiscard]] inline std::int32_t readI32(std::span<const std::byte> b,
                                          std::size_t off) noexcept {
    return std::bit_cast<std::int32_t>(readU32(b, off));
}

[[nodiscard]] inline float readF32(std::span<const std::byte> b, std::size_t off) noexcept {
    return std::bit_cast<float>(readU32(b, off));
}

[[nodiscard]] inline std::uint16_t readU16(std::span<const std::byte> b,
                                           std::size_t off) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint32_t>(b[off])
                                    | (std::to_integer<std::uint32_t>(b[off + 1]) << 8));
}

} // namespace rm
