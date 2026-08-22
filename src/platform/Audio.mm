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
        // AVFAudio THROWS Objective-C exceptions from graph calls rather than returning
        // errors — connecting an unsupported format is an NSException, not an NSError —
        // and an audio problem must never take the match down. Everything in here is
        // guarded; any throw degrades to the same silence a missing device does.
        @try {
            impl_->engine = [[AVAudioEngine alloc] init];

            // TWO formats, deliberately. The render block writes what `Mixer::render`
            // produces — interleaved stereo — so the SOURCE is created with that. The
            // GRAPH connection uses the standard (deinterleaved) format, because the
            // engine's mixer inputs reject interleaved outright (-10868,
            // kAudioUnitErr_FormatNotSupported, thrown as an exception at connect); the
            // source node owns the conversion between its block's format and its output.
            AVAudioFormat* blockFormat =
                [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                 sampleRate:kSampleRate
                                                   channels:2
                                                interleaved:YES];
            AVAudioFormat* graphFormat =
                [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kSampleRate
                                                               channels:2];

            Mixer* pull = &mixer;
            impl_->source = [[AVAudioSourceNode alloc]
                initWithFormat:blockFormat
                   renderBlock:^OSStatus(BOOL* /*isSilence*/,
                                         const AudioTimeStamp* /*timestamp*/,
                                         AVAudioFrameCount frameCount,
                                         AudioBufferList* output) {
                     // ONE buffer, interleaved — blockFormat promises it. The mixer fills
                     // or writes silence; either way the callback never blocks on anything
                     // but the mixer's microsecond lock.
                     auto* samples = static_cast<float*>(output->mBuffers[0].mData);
                     pull->render(samples, frameCount);
                     return noErr;
                   }];

            [impl_->engine attachNode:impl_->source];
            [impl_->engine connect:impl_->source
                                to:impl_->engine.mainMixerNode
                            format:graphFormat];

            NSError* error = nil;
            if (![impl_->engine startAndReturnError:&error]) {
                std::fprintf(stderr,
                             "audio: device declined (%s) — the match plays silent\n",
                             error != nil ? error.localizedDescription.UTF8String : "?");
                impl_->engine = nil;
                impl_->source = nil;
                return false;
            }
            return true;
        } @catch (NSException* exception) {
            std::fprintf(stderr, "audio: engine threw (%s) — the match plays silent\n",
                         exception.reason != nil ? exception.reason.UTF8String : "?");
            impl_->engine = nil;
            impl_->source = nil;
            return false;
        }
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
