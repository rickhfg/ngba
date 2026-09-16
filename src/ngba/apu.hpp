#pragma once

#include "ngba/audio.hpp"

#include <algorithm>
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
    void SetLastCycle(std::uint64_t cycle) noexcept { last_cycle_ = cycle; }

    void RestoreFromIo(const std::array<std::uint8_t, 1024>& io, bool sound_dma_active) noexcept;

    // DirectSound FIFO inspection (for testing)
    std::size_t FifoCountA() const noexcept { return fifo_a_.count; }
    std::size_t FifoCountB() const noexcept { return fifo_b_.count; }
    std::int8_t CurrentSampleA() const noexcept { return fifo_a_.current_sample; }
    std::int8_t CurrentSampleB() const noexcept { return fifo_b_.current_sample; }
    std::uint16_t SoundCntL() const noexcept { return soundcnt_l_; }
    std::uint16_t SoundCntH() const noexcept { return soundcnt_h_; }
    std::uint16_t SoundCntX() const noexcept;
    std::uint16_t SoundBias() const noexcept { return soundbias_; }

    // PSG channel status inspection (for testing)
    bool Channel1Active() const noexcept { return ch1_.enabled; }
    bool Channel2Active() const noexcept { return ch2_.enabled; }
    bool Channel3Active() const noexcept { return ch3_.enabled; }
    bool Channel4Active() const noexcept { return ch4_.enabled; }

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

    struct Channel1 {
        bool enabled{false};
        bool length_enabled{false};
        int length{0};
        int duty{0};
        int duty_step{0};
        int initial_volume{0};
        int current_volume{0};
        int envelope_direction{0};
        int envelope_period{0};
        int envelope_timer{0};
        int frequency{0};
        int frequency_timer{0};
        int sweep_period{0};
        int sweep_timer{0};
        int sweep_direction{0};
        int sweep_shift{0};
        int shadow_frequency{0};
        bool sweep_enabled{false};

        void Reset() noexcept;
        void Trigger() noexcept;
        void StepFrequency(int cycles) noexcept;
        void StepLength() noexcept;
        void StepEnvelope() noexcept;
        void StepSweep() noexcept;
        std::int32_t Sample() const noexcept;
    };

    struct Channel2 {
        bool enabled{false};
        bool length_enabled{false};
        int length{0};
        int duty{0};
        int duty_step{0};
        int initial_volume{0};
        int current_volume{0};
        int envelope_direction{0};
        int envelope_period{0};
        int envelope_timer{0};
        int frequency{0};
        int frequency_timer{0};

        void Reset() noexcept;
        void Trigger() noexcept;
        void StepFrequency(int cycles) noexcept;
        void StepLength() noexcept;
        void StepEnvelope() noexcept;
        std::int32_t Sample() const noexcept;
    };

    struct Channel3 {
        bool enabled{false};
        bool length_enabled{false};
        int length{0};
        int frequency{0};
        int frequency_timer{0};
        int sample_index{0};
        int volume_code{0};
        bool force_75{false};
        std::array<std::uint8_t, 16> wave_ram{};

        void Reset() noexcept;
        void Trigger() noexcept;
        void StepFrequency(int cycles) noexcept;
        void StepLength() noexcept;
        std::int32_t Sample() const noexcept;
    };

    struct Channel4 {
        bool enabled{false};
        bool length_enabled{false};
        int length{0};
        int initial_volume{0};
        int current_volume{0};
        int envelope_direction{0};
        int envelope_period{0};
        int envelope_timer{0};
        int ratio{0};
        bool counter_width_7{false};
        int shift_clock{0};
        int frequency_timer{0};
        std::uint16_t lfsr{0x7FFF};

        void Reset() noexcept;
        int Period() const noexcept;
        void Trigger() noexcept;
        void StepFrequency(int cycles) noexcept;
        void StepLength() noexcept;
        void StepEnvelope() noexcept;
        std::int32_t Sample() const noexcept;
    };

    void StepFrameSequencer() noexcept;
    void Sample() noexcept;

    AudioSink* sink_{nullptr};

    std::uint16_t soundcnt_l_{0};
    std::uint16_t soundcnt_h_{0};
    std::uint16_t soundcnt_x_{0};
    std::uint16_t soundbias_{0x0200};

    SoundFifo fifo_a_{};
    SoundFifo fifo_b_{};

    Channel1 ch1_{};
    Channel2 ch2_{};
    Channel3 ch3_{};
    Channel4 ch4_{};

    std::uint64_t last_cycle_{0};
    std::uint64_t sample_accumulator_{0};
    std::uint64_t frame_seq_accumulator_{0};
    int frame_seq_step_{0};

    std::vector<std::int16_t> sample_buffer_{};

    double lp_l_{0.0};
    double lp_r_{0.0};
    double hp_l_prev_{0.0};
    double hp_r_prev_{0.0};
    double lp_l_prev_in_{0.0};
    double lp_r_prev_in_{0.0};
};

} // namespace ngba
