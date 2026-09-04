#pragma once

namespace rm::mps {

/// Metal-cpp and the Objective-C Metal headers redeclare the same framework symbols, so the
/// concrete MPS kernel lives behind raw Objective-C object pointers in MpsBlur.mm.
[[nodiscard]] void* createGaussianBlur(void* device, float sigma) noexcept;
void destroyGaussianBlur(void* kernel) noexcept;
void encodeGaussianBlur(void* kernel, void* commandBuffer, void* source,
                        void* destination) noexcept;

} // namespace rm::mps
