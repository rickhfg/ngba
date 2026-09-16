#pragma once

#include "ngba/audio.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ngba {

class Apu {
public:
    static constexpr std::uint32_t kSampleRate = 44100;
    static constexpr std::uint64_t kCyclesPerSecond = 16777216;

    Apu() noexcept;

    void Reset() noexcept;
    void SetAudioSink(AudioSink* sink) noexcept { sink_ = sink; }
    AudioSink* GetAudioSink() const noexcept { return sink_; }

    std::uint8_t Read8(std::uint32_t offset) const noexcept;
    std::uint16_t Read16(std::uint32_t offset) const noexcept;

    void Write8(std::uint32_t offset, std::uint8_t value) noexcept;
    void Write16(std::uint32_t offset, std::uint16_t value) noexcept;
    void Write32(std::uint32_t offset, std::uint32_t value) noexcept;

    unsigned StepTimer(unsigned timer_index) noexcept;
    void AdvanceTo(std::uint64_t cycles) noexcept;
    void FlushFrame() noexcept;

    // DirectSound FIFO inspection (for testing)
    std::size_t FifoCountA() const noexcept { return fifo_a_.count; }
    std::size_t FifoCountB() const noexcept { return fifo_b_.count; }
    std::int8_t CurrentSampleA() const noexcept { return fifo_a_.current_sample; }
    std::int8_t CurrentSampleB() const noexcept { return fifo_b_.current_sample; }
    std::uint16_t SoundCntL() const noexcept { return soundcnt_l_; }
    std::uint16_t SoundCntH() const noexcept { return soundcnt_h_; }
    std::uint16_t SoundCntX() const noexcept { return soundcnt_x_; }
    std::uint16_t SoundBias() const noexcept { return soundbias_; }

private:
    struct SoundFifo {
        std::array<std::int8_t, 32> buffer{};
        std::size_t read_index{0};
        std::size_t write_index{0};
        std::size_t count{0};
        std::int8_t current_sample{0};

        void Reset() noexcept {
            read_index = 0;
            write_index = 0;
            count = 0;
            current_sample = 0;
        }

        void Push(std::int8_t byte) noexcept {
            if (count < buffer.size()) {
                buffer[write_index] = byte;
                write_index = (write_index + 1) % buffer.size();
                ++count;
            }
        }

        void Pop() noexcept {
            if (count > 0) {
                current_sample = buffer[read_index];
                read_index = (read_index + 1) % buffer.size();
                --count;
            }
        }
    };

    void Sample() noexcept;

    AudioSink* sink_{nullptr};

    std::uint16_t soundcnt_l_{0};
    std::uint16_t soundcnt_h_{0};
    std::uint16_t soundcnt_x_{0};
    std::uint16_t soundbias_{0x0200};

    SoundFifo fifo_a_{};
    SoundFifo fifo_b_{};

    std::uint64_t last_cycle_{0};
    std::uint64_t sample_accumulator_{0};

    std::vector<std::int16_t> sample_buffer_{};
};

} // namespace ngba
