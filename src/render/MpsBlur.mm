#import <MetalPerformanceShaders/MetalPerformanceShaders.h>

#include "render/MpsBlur.hpp"

namespace rm::mps {

void* createGaussianBlur(void* device, float sigma) noexcept {
    auto* blur = [[MPSImageGaussianBlur alloc]
        initWithDevice:(__bridge id<MTLDevice>)device
                sigma:sigma];
    blur.edgeMode = MPSImageEdgeModeClamp;
    return const_cast<void*>(CFBridgingRetain(blur));
}

void destroyGaussianBlur(void* kernel) noexcept {
    if (kernel != nullptr) {
        (void)CFBridgingRelease(kernel);
    }
}

void encodeGaussianBlur(void* kernel, void* commandBuffer, void* source,
                        void* destination) noexcept {
    MPSImageGaussianBlur* blur = (__bridge MPSImageGaussianBlur*)kernel;
    [blur encodeToCommandBuffer:(__bridge id<MTLCommandBuffer>)commandBuffer
                  sourceTexture:(__bridge id<MTLTexture>)source
             destinationTexture:(__bridge id<MTLTexture>)destination];
}

} // namespace rm::mps
