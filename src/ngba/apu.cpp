#include "ngba/apu.hpp"

#include <algorithm>

namespace ngba {

Apu::Apu() noexcept {
    sample_buffer_.reserve(2048);
    Reset();
}

void Apu::Reset() noexcept {
    soundcnt_l_ = 0;
    soundcnt_h_ = 0;
    soundcnt_x_ = 0;
    soundbias_ = 0x0200;
    fifo_a_.Reset();
    fifo_b_.Reset();
    last_cycle_ = 0;
    sample_accumulator_ = 0;
    sample_buffer_.clear();
}

std::uint8_t Apu::Read8(std::uint32_t offset) const noexcept {
    switch (offset) {
    case 0x080u: return static_cast<std::uint8_t>(soundcnt_l_);
    case 0x081u: return static_cast<std::uint8_t>(soundcnt_l_ >> 8);
    case 0x082u: return static_cast<std::uint8_t>(soundcnt_h_);
    case 0x083u: return static_cast<std::uint8_t>(soundcnt_h_ >> 8);
    case 0x084u: return static_cast<std::uint8_t>(soundcnt_x_);
    case 0x085u: return static_cast<std::uint8_t>(soundcnt_x_ >> 8);
    case 0x088u: return static_cast<std::uint8_t>(soundbias_);
    case 0x089u: return static_cast<std::uint8_t>(soundbias_ >> 8);
    default:
        return 0;
    }
}

std::uint16_t Apu::Read16(std::uint32_t offset) const noexcept {
    switch (offset) {
    case 0x080u: return soundcnt_l_;
    case 0x082u: return soundcnt_h_;
    case 0x084u: return soundcnt_x_;
    case 0x088u: return soundbias_;
    default:
        return 0;
    }
}

void Apu::Write8(std::uint32_t offset, std::uint8_t value) noexcept {
    switch (offset) {
    case 0x080u:
        soundcnt_l_ = static_cast<std::uint16_t>((soundcnt_l_ & 0xFF00u) | value);
        break;
    case 0x081u:
        soundcnt_l_ = static_cast<std::uint16_t>((soundcnt_l_ & 0x00FFu) | (static_cast<std::uint16_t>(value) << 8));
        break;
    case 0x082u:
        soundcnt_h_ = static_cast<std::uint16_t>((soundcnt_h_ & 0xFF00u) | (value & 0x0Fu));
        break;
    case 0x083u: {
        soundcnt_h_ = static_cast<std::uint16_t>((soundcnt_h_ & 0x00FFu) | (static_cast<std::uint16_t>(value) << 8));
        if ((value & 0x08u) != 0) fifo_a_.Reset(); // bit 11 (bit 3 of upper byte)
        if ((value & 0x80u) != 0) fifo_b_.Reset(); // bit 15 (bit 7 of upper byte)
        break;
    }
    case 0x084u: {
        const bool enabled = (value & 0x80u) != 0;
        if (!enabled) {
            Reset();
        } else {
            soundcnt_x_ |= 0x0080u;
        }
        break;
    }
    case 0x088u:
        soundbias_ = static_cast<std::uint16_t>((soundbias_ & 0xFF00u) | value);
        break;
    case 0x089u:
        soundbias_ = static_cast<std::uint16_t>((soundbias_ & 0x00FFu) | (static_cast<std::uint16_t>(value) << 8));
        break;

    // FIFO A byte writes
    case 0x0A0u:
    case 0x0A1u:
    case 0x0A2u:
    case 0x0A3u:
        fifo_a_.Push(static_cast<std::int8_t>(value));
        break;

    // FIFO B byte writes
    case 0x0A4u:
    case 0x0A5u:
    case 0x0A6u:
    case 0x0A7u:
        fifo_b_.Push(static_cast<std::int8_t>(value));
        break;

    default:
        break;
    }
}

void Apu::Write16(std::uint32_t offset, std::uint16_t value) noexcept {
    switch (offset) {
    case 0x080u:
        soundcnt_l_ = value;
        break;
    case 0x082u: {
        soundcnt_h_ = value;
        if ((value & 0x0800u) != 0) fifo_a_.Reset();
        if ((value & 0x8000u) != 0) fifo_b_.Reset();
        break;
    }
    case 0x084u: {
        const bool enabled = (value & 0x0080u) != 0;
        if (!enabled) {
            Reset();
        } else {
            soundcnt_x_ |= 0x0080u;
        }
        break;
    }
    case 0x088u:
        soundbias_ = value;
        break;
    case 0x0A0u:
    case 0x0A2u:
        fifo_a_.Push(static_cast<std::int8_t>(value & 0xFFu));
        fifo_a_.Push(static_cast<std::int8_t>((value >> 8) & 0xFFu));
        break;
    case 0x0A4u:
    case 0x0A6u:
        fifo_b_.Push(static_cast<std::int8_t>(value & 0xFFu));
        fifo_b_.Push(static_cast<std::int8_t>((value >> 8) & 0xFFu));
        break;
    default:
        break;
    }
}

void Apu::Write32(std::uint32_t offset, std::uint32_t value) noexcept {
    switch (offset) {
    case 0x080u:
        Write16(0x080u, static_cast<std::uint16_t>(value));
        Write16(0x082u, static_cast<std::uint16_t>(value >> 16));
        break;
    case 0x084u:
        Write16(0x084u, static_cast<std::uint16_t>(value));
        break;
    case 0x088u:
        Write16(0x088u, static_cast<std::uint16_t>(value));
        break;
    case 0x0A0u:
        fifo_a_.Push(static_cast<std::int8_t>(value & 0xFFu));
        fifo_a_.Push(static_cast<std::int8_t>((value >> 8) & 0xFFu));
        fifo_a_.Push(static_cast<std::int8_t>((value >> 16) & 0xFFu));
        fifo_a_.Push(static_cast<std::int8_t>((value >> 24) & 0xFFu));
        break;
    case 0x0A4u:
        fifo_b_.Push(static_cast<std::int8_t>(value & 0xFFu));
        fifo_b_.Push(static_cast<std::int8_t>((value >> 8) & 0xFFu));
        fifo_b_.Push(static_cast<std::int8_t>((value >> 16) & 0xFFu));
        fifo_b_.Push(static_cast<std::int8_t>((value >> 24) & 0xFFu));
        break;
    default:
        break;
    }
}

unsigned Apu::StepTimer(unsigned timer_index) noexcept {
    if (timer_index > 1) return 0;

    unsigned dma_mask = 0;
    const unsigned timer_a = (soundcnt_h_ >> 10) & 1u;
    const unsigned timer_b = (soundcnt_h_ >> 14) & 1u;

    if (timer_index == timer_a) {
        fifo_a_.Pop();
        if (fifo_a_.count <= 16) {
            dma_mask |= 1u; // DMA channel 1
        }
    }

    if (timer_index == timer_b) {
        fifo_b_.Pop();
        if (fifo_b_.count <= 16) {
            dma_mask |= 2u; // DMA channel 2
        }
    }

    return dma_mask;
}

void Apu::AdvanceTo(std::uint64_t cycles) noexcept {
    if (cycles <= last_cycle_) return;

    const std::uint64_t elapsed = cycles - last_cycle_;
    last_cycle_ = cycles;

    sample_accumulator_ += elapsed * static_cast<std::uint64_t>(kSampleRate);
    while (sample_accumulator_ >= kCyclesPerSecond) {
        sample_accumulator_ -= kCyclesPerSecond;
        Sample();
    }
}

void Apu::Sample() noexcept {
    if ((soundcnt_x_ & 0x0080u) == 0) {
        sample_buffer_.push_back(0);
        sample_buffer_.push_back(0);
        return;
    }

    std::int32_t sample_a = fifo_a_.current_sample;
    if ((soundcnt_h_ & 0x0004u) == 0) sample_a >>= 1; // 50% volume

    std::int32_t sample_b = fifo_b_.current_sample;
    if ((soundcnt_h_ & 0x0008u) == 0) sample_b >>= 1; // 50% volume

    const bool a_right = (soundcnt_h_ & 0x0100u) != 0;
    const bool a_left  = (soundcnt_h_ & 0x0200u) != 0;
    const bool b_right = (soundcnt_h_ & 0x1000u) != 0;
    const bool b_left  = (soundcnt_h_ & 0x2000u) != 0;

    std::int32_t left = 0;
    if (a_left) left += sample_a;
    if (b_left) left += sample_b;

    std::int32_t right = 0;
    if (a_right) right += sample_a;
    if (b_right) right += sample_b;

    const std::int32_t left_pcm = std::max(-32768, std::min(32767, left * 128));
    const std::int32_t right_pcm = std::max(-32768, std::min(32767, right * 128));

    sample_buffer_.push_back(static_cast<std::int16_t>(left_pcm));
    sample_buffer_.push_back(static_cast<std::int16_t>(right_pcm));
}

void Apu::FlushFrame() noexcept {
    if (sink_ && !sample_buffer_.empty()) {
        sink_->SubmitSamples(sample_buffer_.data(), sample_buffer_.size() / 2);
    }
    sample_buffer_.clear();
}

} // namespace ngba
