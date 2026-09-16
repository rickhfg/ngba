#include "ngba/apu.hpp"

#include <algorithm>

namespace ngba {

void Apu::Channel1::Reset() noexcept {
    enabled = false;
    length_enabled = false;
    length = 0;
    duty = 0;
    duty_step = 0;
    initial_volume = 0;
    current_volume = 0;
    envelope_direction = 0;
    envelope_period = 0;
    envelope_timer = 0;
    frequency = 0;
    frequency_timer = 0;
    sweep_period = 0;
    sweep_timer = 0;
    sweep_direction = 0;
    sweep_shift = 0;
    shadow_frequency = 0;
    sweep_enabled = false;
}

void Apu::Channel1::Trigger() noexcept {
    enabled = true;
    current_volume = initial_volume;
    envelope_timer = envelope_period;
    frequency_timer = (2048 - frequency) * 16;
    duty_step = 0;
    if (length == 0) length = 64;

    shadow_frequency = frequency;
    sweep_timer = sweep_period == 0 ? 8 : sweep_period;
    sweep_enabled = (sweep_period > 0) || (sweep_shift > 0);
    if (sweep_shift > 0) {
        const int delta = shadow_frequency >> sweep_shift;
        const int next_f = shadow_frequency + (sweep_direction ? -delta : delta);
        if (next_f > 2047) enabled = false;
    }
}

void Apu::Channel1::StepFrequency(int cycles) noexcept {
    if (!enabled) return;
    frequency_timer -= cycles;
    while (frequency_timer <= 0) {
        frequency_timer += (2048 - frequency) * 16;
        duty_step = (duty_step + 1) & 7;
    }
}

void Apu::Channel1::StepLength() noexcept {
    if (length_enabled && length > 0) {
        if (--length == 0) enabled = false;
    }
}

void Apu::Channel1::StepEnvelope() noexcept {
    if (!enabled || envelope_period == 0) return;
    if (--envelope_timer <= 0) {
        envelope_timer = envelope_period;
        if (envelope_direction && current_volume < 15) ++current_volume;
        else if (!envelope_direction && current_volume > 0) --current_volume;
    }
}

void Apu::Channel1::StepSweep() noexcept {
    if (!sweep_enabled || !enabled || sweep_period == 0) return;
    if (--sweep_timer <= 0) {
        sweep_timer = sweep_period == 0 ? 8 : sweep_period;
        if (sweep_shift > 0) {
            const int delta = shadow_frequency >> sweep_shift;
            const int next_f = shadow_frequency + (sweep_direction ? -delta : delta);
            if (next_f <= 2047 && next_f >= 0) {
                shadow_frequency = next_f;
                frequency = next_f;
                frequency_timer = (2048 - frequency) * 16;
                const int delta2 = shadow_frequency >> sweep_shift;
                const int next2 = shadow_frequency + (sweep_direction ? -delta2 : delta2);
                if (next2 > 2047) enabled = false;
            } else {
                enabled = false;
            }
        }
    }
}

std::int32_t Apu::Channel1::Sample() const noexcept {
    if (!enabled || current_volume == 0) return 0;
    static const std::uint8_t kDutyTable[4][8] = {
        { 0, 0, 0, 0, 0, 0, 0, 1 }, // 12.5%
        { 1, 0, 0, 0, 0, 0, 0, 1 }, // 25%
        { 1, 0, 0, 0, 0, 1, 1, 1 }, // 50%
        { 0, 1, 1, 1, 1, 1, 1, 0 }  // 75%
    };
    return kDutyTable[duty][duty_step] ? current_volume : 0;
}

void Apu::Channel2::Reset() noexcept {
    enabled = false;
    length_enabled = false;
    length = 0;
    duty = 0;
    duty_step = 0;
    initial_volume = 0;
    current_volume = 0;
    envelope_direction = 0;
    envelope_period = 0;
    envelope_timer = 0;
    frequency = 0;
    frequency_timer = 0;
}

void Apu::Channel2::Trigger() noexcept {
    enabled = true;
    current_volume = initial_volume;
    envelope_timer = envelope_period;
    frequency_timer = (2048 - frequency) * 16;
    duty_step = 0;
    if (length == 0) length = 64;
}

void Apu::Channel2::StepFrequency(int cycles) noexcept {
    if (!enabled) return;
    frequency_timer -= cycles;
    while (frequency_timer <= 0) {
        frequency_timer += (2048 - frequency) * 16;
        duty_step = (duty_step + 1) & 7;
    }
}

void Apu::Channel2::StepLength() noexcept {
    if (length_enabled && length > 0) {
        if (--length == 0) enabled = false;
    }
}

void Apu::Channel2::StepEnvelope() noexcept {
    if (!enabled || envelope_period == 0) return;
    if (--envelope_timer <= 0) {
        envelope_timer = envelope_period;
        if (envelope_direction && current_volume < 15) ++current_volume;
        else if (!envelope_direction && current_volume > 0) --current_volume;
    }
}

std::int32_t Apu::Channel2::Sample() const noexcept {
    if (!enabled || current_volume == 0) return 0;
    static const std::uint8_t kDutyTable[4][8] = {
        { 0, 0, 0, 0, 0, 0, 0, 1 },
        { 1, 0, 0, 0, 0, 0, 0, 1 },
        { 1, 0, 0, 0, 0, 1, 1, 1 },
        { 0, 1, 1, 1, 1, 1, 1, 0 }
    };
    return kDutyTable[duty][duty_step] ? current_volume : 0;
}

void Apu::Channel3::Reset() noexcept {
    enabled = false;
    length_enabled = false;
    length = 0;
    frequency = 0;
    frequency_timer = 0;
    sample_index = 0;
    volume_code = 0;
    force_75 = false;
    wave_ram.fill(0);
}

void Apu::Channel3::Trigger() noexcept {
    enabled = true;
    frequency_timer = (2048 - frequency) * 8;
    sample_index = 0;
    if (length == 0) length = 256;
}

void Apu::Channel3::StepFrequency(int cycles) noexcept {
    if (!enabled) return;
    frequency_timer -= cycles;
    while (frequency_timer <= 0) {
        frequency_timer += (2048 - frequency) * 8;
        sample_index = (sample_index + 1) & 31;
    }
}

void Apu::Channel3::StepLength() noexcept {
    if (length_enabled && length > 0) {
        if (--length == 0) enabled = false;
    }
}

std::int32_t Apu::Channel3::Sample() const noexcept {
    if (!enabled || volume_code == 0) return 0;
    const std::uint8_t byte = wave_ram[sample_index >> 1];
    std::uint32_t val = (sample_index & 1) ? (byte & 0x0Fu) : ((byte >> 4) & 0x0Fu);
    switch (volume_code) {
    case 1: break;            // 100%
    case 2: val >>= 1; break; // 50%
    case 3: val >>= 2; break; // 25%
    default: val = 0; break;
    }
    if (force_75) val = (val * 3) / 4;
    return static_cast<std::int32_t>(val);
}

void Apu::Channel4::Reset() noexcept {
    enabled = false;
    length_enabled = false;
    length = 0;
    initial_volume = 0;
    current_volume = 0;
    envelope_direction = 0;
    envelope_period = 0;
    envelope_timer = 0;
    ratio = 0;
    counter_width_7 = false;
    shift_clock = 0;
    frequency_timer = 0;
    lfsr = 0x7FFF;
}

int Apu::Channel4::Period() const noexcept {
    const int base = (ratio == 0) ? 32 : (ratio * 64);
    return (base << shift_clock);
}

void Apu::Channel4::Trigger() noexcept {
    enabled = true;
    current_volume = initial_volume;
    envelope_timer = envelope_period;
    frequency_timer = Period();
    lfsr = 0x7FFF;
    if (length == 0) length = 64;
}

void Apu::Channel4::StepFrequency(int cycles) noexcept {
    if (!enabled) return;
    frequency_timer -= cycles;
    while (frequency_timer <= 0) {
        frequency_timer += Period();
        const std::uint16_t new_bit = (lfsr & 1u) ^ ((lfsr >> 1) & 1u);
        lfsr = (lfsr >> 1) | (new_bit << 14);
        if (counter_width_7) {
            lfsr = (lfsr & ~0x40u) | (new_bit << 6);
        }
    }
}

void Apu::Channel4::StepLength() noexcept {
    if (length_enabled && length > 0) {
        if (--length == 0) enabled = false;
    }
}

void Apu::Channel4::StepEnvelope() noexcept {
    if (!enabled || envelope_period == 0) return;
    if (--envelope_timer <= 0) {
        envelope_timer = envelope_period;
        if (envelope_direction && current_volume < 15) ++current_volume;
        else if (!envelope_direction && current_volume > 0) --current_volume;
    }
}

std::int32_t Apu::Channel4::Sample() const noexcept {
    if (!enabled || current_volume == 0) return 0;
    return (~lfsr & 1u) ? current_volume : 0;
}

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
    ch1_.Reset();
    ch2_.Reset();
    ch3_.Reset();
    ch4_.Reset();
    last_cycle_ = 0;
    sample_accumulator_ = 0;
    frame_seq_accumulator_ = 0;
    frame_seq_step_ = 0;
    lp_l_ = 0.0;
    lp_r_ = 0.0;
    hp_l_prev_ = 0.0;
    hp_r_prev_ = 0.0;
    lp_l_prev_in_ = 0.0;
    lp_r_prev_in_ = 0.0;
    sample_buffer_.clear();
}

std::uint16_t Apu::SoundCntX() const noexcept {
    std::uint16_t status = soundcnt_x_ & 0x0080u;
    if (ch1_.enabled) status |= 0x0001u;
    if (ch2_.enabled) status |= 0x0002u;
    if (ch3_.enabled) status |= 0x0004u;
    if (ch4_.enabled) status |= 0x0008u;
    return status;
}

std::uint8_t Apu::Read8(std::uint32_t offset) const noexcept {
    switch (offset) {
    case 0x060u: return static_cast<std::uint8_t>(ch1_.sweep_shift | (ch1_.sweep_direction << 3) | (ch1_.sweep_period << 4));
    case 0x062u: return static_cast<std::uint8_t>(ch1_.duty << 6);
    case 0x063u: return static_cast<std::uint8_t>(ch1_.envelope_period | (ch1_.envelope_direction << 3) | (ch1_.initial_volume << 4));
    case 0x064u: return 0;
    case 0x065u: return static_cast<std::uint8_t>(ch1_.length_enabled ? 0x40 : 0);
    case 0x068u: return static_cast<std::uint8_t>(ch2_.duty << 6);
    case 0x069u: return static_cast<std::uint8_t>(ch2_.envelope_period | (ch2_.envelope_direction << 3) | (ch2_.initial_volume << 4));
    case 0x06Cu: return 0;
    case 0x06Du: return static_cast<std::uint8_t>(ch2_.length_enabled ? 0x40 : 0);
    case 0x070u: return static_cast<std::uint8_t>(ch3_.enabled ? 0x80 : 0);
    case 0x072u: return 0;
    case 0x073u: return static_cast<std::uint8_t>((ch3_.volume_code << 5) | (ch3_.force_75 ? 0x80 : 0));
    case 0x074u: return 0;
    case 0x075u: return static_cast<std::uint8_t>(ch3_.length_enabled ? 0x40 : 0);
    case 0x078u: return 0;
    case 0x079u: return static_cast<std::uint8_t>(ch4_.envelope_period | (ch4_.envelope_direction << 3) | (ch4_.initial_volume << 4));
    case 0x07Cu: return static_cast<std::uint8_t>(ch4_.ratio | (ch4_.counter_width_7 ? 0x08 : 0) | (ch4_.shift_clock << 4));
    case 0x07Du: return static_cast<std::uint8_t>(ch4_.length_enabled ? 0x40 : 0);
    case 0x080u: return static_cast<std::uint8_t>(soundcnt_l_);
    case 0x081u: return static_cast<std::uint8_t>(soundcnt_l_ >> 8);
    case 0x082u: return static_cast<std::uint8_t>(soundcnt_h_);
    case 0x083u: return static_cast<std::uint8_t>(soundcnt_h_ >> 8);
    case 0x084u: return static_cast<std::uint8_t>(SoundCntX());
    case 0x085u: return 0;
    case 0x088u: return static_cast<std::uint8_t>(soundbias_);
    case 0x089u: return static_cast<std::uint8_t>(soundbias_ >> 8);
    default:
        if (offset >= 0x090u && offset <= 0x09Fu) {
            return ch3_.wave_ram[offset - 0x090u];
        }
        return 0;
    }
}

std::uint16_t Apu::Read16(std::uint32_t offset) const noexcept {
    switch (offset) {
    case 0x080u: return soundcnt_l_;
    case 0x082u: return soundcnt_h_;
    case 0x084u: return SoundCntX();
    case 0x088u: return soundbias_;
    default:
        return static_cast<std::uint16_t>(Read8(offset)) |
               (static_cast<std::uint16_t>(Read8(offset + 1)) << 8);
    }
}

void Apu::Write8(std::uint32_t offset, std::uint8_t value) noexcept {
    switch (offset) {
    case 0x060u:
        ch1_.sweep_shift = value & 0x07u;
        ch1_.sweep_direction = (value >> 3) & 1u;
        ch1_.sweep_period = (value >> 4) & 0x07u;
        break;
    case 0x062u:
        ch1_.length = 64 - (value & 0x3Fu);
        ch1_.duty = (value >> 6) & 0x03u;
        break;
    case 0x063u:
        ch1_.envelope_period = value & 0x07u;
        ch1_.envelope_direction = (value >> 3) & 1u;
        ch1_.initial_volume = (value >> 4) & 0x0Fu;
        break;
    case 0x064u:
        ch1_.frequency = (ch1_.frequency & 0x0700u) | value;
        break;
    case 0x065u:
        ch1_.frequency = (ch1_.frequency & 0x00FFu) | ((static_cast<std::uint16_t>(value) & 0x07u) << 8);
        ch1_.length_enabled = (value & 0x40u) != 0;
        if ((value & 0x80u) != 0) ch1_.Trigger();
        break;
    case 0x068u:
        ch2_.length = 64 - (value & 0x3Fu);
        ch2_.duty = (value >> 6) & 0x03u;
        break;
    case 0x069u:
        ch2_.envelope_period = value & 0x07u;
        ch2_.envelope_direction = (value >> 3) & 1u;
        ch2_.initial_volume = (value >> 4) & 0x0Fu;
        break;
    case 0x06Cu:
        ch2_.frequency = (ch2_.frequency & 0x0700u) | value;
        break;
    case 0x06Du:
        ch2_.frequency = (ch2_.frequency & 0x00FFu) | ((static_cast<std::uint16_t>(value) & 0x07u) << 8);
        ch2_.length_enabled = (value & 0x40u) != 0;
        if ((value & 0x80u) != 0) ch2_.Trigger();
        break;
    case 0x070u:
        ch3_.enabled = (value & 0x80u) != 0;
        break;
    case 0x072u:
        ch3_.length = 256 - value;
        break;
    case 0x073u:
        ch3_.volume_code = (value >> 5) & 3u;
        ch3_.force_75 = (value & 0x80u) != 0;
        break;
    case 0x074u:
        ch3_.frequency = (ch3_.frequency & 0x0700u) | value;
        break;
    case 0x075u:
        ch3_.frequency = (ch3_.frequency & 0x00FFu) | ((static_cast<std::uint16_t>(value) & 0x07u) << 8);
        ch3_.length_enabled = (value & 0x40u) != 0;
        if ((value & 0x80u) != 0) ch3_.Trigger();
        break;
    case 0x078u:
        ch4_.length = 64 - (value & 0x3Fu);
        break;
    case 0x079u:
        ch4_.envelope_period = value & 0x07u;
        ch4_.envelope_direction = (value >> 3) & 1u;
        ch4_.initial_volume = (value >> 4) & 0x0Fu;
        break;
    case 0x07Cu:
        ch4_.ratio = value & 0x07u;
        ch4_.counter_width_7 = (value & 0x08u) != 0;
        ch4_.shift_clock = (value >> 4) & 0x0Fu;
        break;
    case 0x07Du:
        ch4_.length_enabled = (value & 0x40u) != 0;
        if ((value & 0x80u) != 0) ch4_.Trigger();
        break;

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
        soundcnt_h_ = static_cast<std::uint16_t>((soundcnt_h_ & 0x00FFu) | (static_cast<std::uint16_t>(value & ~0x88u) << 8));
        if ((value & 0x08u) != 0) fifo_a_.Reset();
        if ((value & 0x80u) != 0) fifo_b_.Reset();
        break;
    }
    case 0x084u: {
        const bool enabled = (value & 0x80u) != 0;
        if (!enabled) {
            soundcnt_l_ = 0;
            soundcnt_h_ = 0;
            soundcnt_x_ = 0;
            fifo_a_.Reset();
            fifo_b_.Reset();
            ch1_.Reset();
            ch2_.Reset();
            ch3_.Reset();
            ch4_.Reset();
            sample_accumulator_ = 0;
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
        if (offset >= 0x090u && offset <= 0x09Fu) {
            ch3_.wave_ram[offset - 0x090u] = value;
        }
        break;
    }
}

void Apu::Write16(std::uint32_t offset, std::uint16_t value) noexcept {
    Write8(offset, static_cast<std::uint8_t>(value));
    Write8(offset + 1, static_cast<std::uint8_t>(value >> 8));
}

void Apu::Write32(std::uint32_t offset, std::uint32_t value) noexcept {
    Write16(offset, static_cast<std::uint16_t>(value));
    Write16(offset + 2, static_cast<std::uint16_t>(value >> 16));
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

void Apu::StepFrameSequencer() noexcept {
    if (frame_seq_step_ == 0 || frame_seq_step_ == 2 || frame_seq_step_ == 4 || frame_seq_step_ == 6) {
        ch1_.StepLength();
        ch2_.StepLength();
        ch3_.StepLength();
        ch4_.StepLength();
    }
    if (frame_seq_step_ == 2 || frame_seq_step_ == 6) {
        ch1_.StepSweep();
    }
    if (frame_seq_step_ == 7) {
        ch1_.StepEnvelope();
        ch2_.StepEnvelope();
        ch4_.StepEnvelope();
    }
    frame_seq_step_ = (frame_seq_step_ + 1) & 7;
}

void Apu::AdvanceTo(std::uint64_t cycles) noexcept {
    if (cycles <= last_cycle_) return;

    while (cycles > last_cycle_) {
        const std::uint64_t needed_sample = kCyclesPerSecond - sample_accumulator_;
        const std::uint64_t cycles_to_sample = (needed_sample + kSampleRate - 1) / kSampleRate;
        const std::uint64_t cycles_to_seq = 32768 - frame_seq_accumulator_;
        const std::uint64_t available = cycles - last_cycle_;
        const std::uint64_t step_cycles = std::min(available, std::min(cycles_to_sample, cycles_to_seq));

        ch1_.StepFrequency(static_cast<int>(step_cycles));
        ch2_.StepFrequency(static_cast<int>(step_cycles));
        ch3_.StepFrequency(static_cast<int>(step_cycles));
        ch4_.StepFrequency(static_cast<int>(step_cycles));
        last_cycle_ += step_cycles;

        frame_seq_accumulator_ += step_cycles;
        if (frame_seq_accumulator_ >= 32768) {
            frame_seq_accumulator_ -= 32768;
            StepFrameSequencer();
        }

        sample_accumulator_ += step_cycles * static_cast<std::uint64_t>(kSampleRate);
        if (sample_accumulator_ >= kCyclesPerSecond) {
            sample_accumulator_ -= kCyclesPerSecond;
            Sample();
        }
    }
}

void Apu::Sample() noexcept {
    if ((soundcnt_x_ & 0x0080u) == 0) {
        sample_buffer_.push_back(0);
        sample_buffer_.push_back(0);
        return;
    }

    // DirectSound A & B 10-bit DAC scaling (4 = 100%, 2 = 50%)
    const std::int32_t a_scale = (soundcnt_h_ & 0x0004u) != 0 ? 4 : 2;
    const std::int32_t b_scale = (soundcnt_h_ & 0x0008u) != 0 ? 4 : 2;

    const std::int32_t sample_a = static_cast<std::int32_t>(fifo_a_.current_sample) * a_scale;
    const std::int32_t sample_b = static_cast<std::int32_t>(fifo_b_.current_sample) * b_scale;

    const bool a_right = (soundcnt_h_ & 0x0100u) != 0;
    const bool a_left  = (soundcnt_h_ & 0x0200u) != 0;
    const bool b_right = (soundcnt_h_ & 0x1000u) != 0;
    const bool b_left  = (soundcnt_h_ & 0x2000u) != 0;

    // PSG Channels (Sound 1-4)
    const int s1 = ch1_.Sample();
    const int s2 = ch2_.Sample();
    const int s3 = ch3_.Sample();
    const int s4 = ch4_.Sample();

    int psg_left = 0;
    int psg_right = 0;
    if (soundcnt_l_ & 0x1000u) psg_left += s1;
    if (soundcnt_l_ & 0x2000u) psg_left += s2;
    if (soundcnt_l_ & 0x4000u) psg_left += s3;

    if (soundcnt_l_ & 0x0100u) psg_right += s1;
    if (soundcnt_l_ & 0x0200u) psg_right += s2;
    if (soundcnt_l_ & 0x0400u) psg_right += s3;

    psg_left <<= 3;
    psg_right <<= 3;

    if (soundcnt_l_ & 0x8000u) psg_left += (s4 << 3);
    if (soundcnt_l_ & 0x0800u) psg_right += (s4 << 3);

    const int vol_r = soundcnt_l_ & 0x07u;
    const int vol_l = (soundcnt_l_ >> 4) & 0x07u;

    psg_left = psg_left * (1 + vol_l);
    psg_right = psg_right * (1 + vol_r);

    const unsigned psg_ratio = soundcnt_h_ & 0x03u;
    const int psg_shift = 4 - std::min(psg_ratio, 2u);
    psg_left >>= psg_shift;
    psg_right >>= psg_shift;

    std::int32_t left = psg_left;
    if (a_left) left += sample_a;
    if (b_left) left += sample_b;

    std::int32_t right = psg_right;
    if (a_right) right += sample_a;
    if (b_right) right += sample_b;

    // Apply SOUNDBIAS and clamp to 10-bit DAC range [0, 1023]
    const std::int32_t bias = static_cast<std::int32_t>(soundbias_ & 0x03FFu);
    const std::int32_t dac_l = std::max<std::int32_t>(0, std::min<std::int32_t>(1023, left + bias));
    const std::int32_t dac_r = std::max<std::int32_t>(0, std::min<std::int32_t>(1023, right + bias));

    // Re-center around 0 and scale to 16-bit PCM output range (48 provides 25% headroom, matching mGBA)
    const std::int32_t pcm_l = (dac_l - bias) * 48;
    const std::int32_t pcm_r = (dac_r - bias) * 48;

    sample_buffer_.push_back(static_cast<std::int16_t>(std::max(-32768, std::min(32767, pcm_l))));
    sample_buffer_.push_back(static_cast<std::int16_t>(std::max(-32768, std::min(32767, pcm_r))));
}

void Apu::FlushFrame() noexcept {
    if (sink_ && !sample_buffer_.empty()) {
        sink_->SubmitSamples(sample_buffer_.data(), sample_buffer_.size() / 2);
    }
    sample_buffer_.clear();
}

void Apu::RestoreFromIo(const std::array<std::uint8_t, 1024>& io, bool sound_dma_active) noexcept {
    soundcnt_l_ = static_cast<std::uint16_t>(io[0x080]) | (static_cast<std::uint16_t>(io[0x081]) << 8);
    soundcnt_h_ = static_cast<std::uint16_t>(io[0x082]) | (static_cast<std::uint16_t>(io[0x083]) << 8);
    soundcnt_x_ = static_cast<std::uint16_t>(io[0x084]) | (static_cast<std::uint16_t>(io[0x085]) << 8);
    soundbias_  = static_cast<std::uint16_t>(io[0x088]) | (static_cast<std::uint16_t>(io[0x089]) << 8);

    if (soundbias_ == 0) soundbias_ = 0x0200;

    // Backward compatibility: make sure older savestates pre-apu dont load muted
    if ((soundcnt_x_ & 0x80u) == 0 && sound_dma_active) {
        soundcnt_x_ = 0x80;
        soundcnt_h_ = 0x3302;
        soundcnt_l_ = 0xFF77;
        soundbias_  = 0x0200;
    }
    if ((soundcnt_l_ & 0xFF00u) == 0 && sound_dma_active) {
        soundcnt_l_ |= 0xFF00u;
    }

    // Restore Channel 1
    ch1_.sweep_shift = io[0x060] & 0x07u;
    ch1_.sweep_direction = (io[0x060] >> 3) & 1u;
    ch1_.sweep_period = (io[0x060] >> 4) & 0x07u;
    ch1_.length = 64 - (io[0x062] & 0x3Fu);
    ch1_.duty = (io[0x062] >> 6) & 0x03u;
    ch1_.envelope_period = io[0x063] & 0x07u;
    ch1_.envelope_direction = (io[0x063] >> 3) & 1u;
    ch1_.initial_volume = (io[0x063] >> 4) & 0x0Fu;
    ch1_.frequency = (static_cast<std::uint16_t>(io[0x064])) | ((static_cast<std::uint16_t>(io[0x065] & 7u)) << 8);
    ch1_.length_enabled = (io[0x065] & 0x40u) != 0;

    // Restore Channel 2
    ch2_.length = 64 - (io[0x068] & 0x3Fu);
    ch2_.duty = (io[0x068] >> 6) & 0x03u;
    ch2_.envelope_period = io[0x069] & 0x07u;
    ch2_.envelope_direction = (io[0x069] >> 3) & 1u;
    ch2_.initial_volume = (io[0x069] >> 4) & 0x0Fu;
    ch2_.frequency = (static_cast<std::uint16_t>(io[0x06C])) | ((static_cast<std::uint16_t>(io[0x06D] & 7u)) << 8);
    ch2_.length_enabled = (io[0x06D] & 0x40u) != 0;

    // Restore Channel 3
    ch3_.enabled = (io[0x070] & 0x80u) != 0;
    ch3_.length = 256 - io[0x072];
    ch3_.volume_code = (io[0x073] >> 5) & 3u;
    ch3_.force_75 = (io[0x073] & 0x80u) != 0;
    ch3_.frequency = (static_cast<std::uint16_t>(io[0x074])) | ((static_cast<std::uint16_t>(io[0x075] & 7u)) << 8);
    ch3_.length_enabled = (io[0x075] & 0x40u) != 0;
    for (std::size_t i = 0; i < 16; ++i) {
        ch3_.wave_ram[i] = io[0x090 + i];
    }

    // Restore Channel 4
    ch4_.length = 64 - (io[0x078] & 0x3Fu);
    ch4_.envelope_period = io[0x079] & 0x07u;
    ch4_.envelope_direction = (io[0x079] >> 3) & 1u;
    ch4_.initial_volume = (io[0x079] >> 4) & 0x0Fu;
    ch4_.ratio = io[0x07C] & 0x07u;
    ch4_.counter_width_7 = (io[0x07C] & 0x08u) != 0;
    ch4_.shift_clock = (io[0x07C] >> 4) & 0x0Fu;
    ch4_.length_enabled = (io[0x07D] & 0x40u) != 0;
}

} // namespace ngba
