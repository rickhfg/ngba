#pragma once

#include <cstddef>
#include <cstdint>

namespace ngba {

/// Abstract audio output sink for platform-independent PCM delivery.
class AudioSink {
public:
    virtual ~AudioSink() = default;

    /// Submits interleaved 16-bit signed stereo PCM frames (Left, Right).
    /// @param stereo_samples Pointer to interleaved L/R 16-bit PCM samples (size is frames * 2).
    /// @param frame_count Number of stereo frames (pairs of L/R samples).
    virtual void SubmitSamples(const std::int16_t* stereo_samples, std::size_t frame_count) = 0;
};

} // namespace ngba
