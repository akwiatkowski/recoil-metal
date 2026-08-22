#import <AVFoundation/AVFoundation.h>

#include "platform/Audio.hpp"

#include "core/audio/Mixer.hpp"

#include <cstdio>

namespace rm::audio {

struct Output::Impl {
    AVAudioEngine* engine = nil;
    AVAudioSourceNode* source = nil;
};

Output::Output() : impl_(std::make_unique<Impl>()) {}

Output::~Output() { stop(); }

bool Output::start(Mixer& mixer) {
    @autoreleasepool {
        impl_->engine = [[AVAudioEngine alloc] init];

        // The mixer's own format; the engine resamples to the device from here. Interleaved
        // is what `Mixer::render` writes, and AVAudioSourceNode accepts it directly.
        AVAudioFormat* format =
            [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                             sampleRate:kSampleRate
                                               channels:2
                                            interleaved:YES];

        Mixer* pull = &mixer;
        impl_->source = [[AVAudioSourceNode alloc]
            initWithFormat:format
               renderBlock:^OSStatus(BOOL* /*isSilence*/, const AudioTimeStamp* /*timestamp*/,
                                     AVAudioFrameCount frameCount, AudioBufferList* output) {
                 // ONE buffer, interleaved — the format above promises it. The mixer fills
                 // or writes silence; either way the callback never blocks on anything but
                 // the mixer's microsecond lock.
                 auto* samples = static_cast<float*>(output->mBuffers[0].mData);
                 pull->render(samples, frameCount);
                 return noErr;
               }];

        [impl_->engine attachNode:impl_->source];
        [impl_->engine connect:impl_->source
                            to:impl_->engine.mainMixerNode
                        format:format];

        NSError* error = nil;
        if (![impl_->engine startAndReturnError:&error]) {
            std::fprintf(stderr, "audio: device declined (%s) — the match plays silent\n",
                         error != nil ? error.localizedDescription.UTF8String : "?");
            impl_->engine = nil;
            impl_->source = nil;
            return false;
        }
        return true;
    }
}

void Output::stop() {
    @autoreleasepool {
        if (impl_->engine != nil) {
            [impl_->engine stop];
            impl_->engine = nil;
            impl_->source = nil;
        }
    }
}

} // namespace rm::audio
