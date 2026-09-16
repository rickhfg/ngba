#include "ngba/runtime.hpp"
#include "ngba/file_io.hpp"

#include <cstdio>
#include <type_traits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ngba {
namespace {

std::uint64_t Hash(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto value : bytes) { hash ^= value; hash *= 1099511628211ull; }
    return hash;
}

class Archive {
public:
    Archive(std::vector<std::uint8_t>& bytes, bool reading) : bytes_(bytes), reading_(reading) {}

    template <typename Value>
    void Field(Value& value) {
        static_assert(std::is_integral<Value>::value && std::is_unsigned<Value>::value, "unsigned state fields only");
        std::uint64_t decoded = 0;
        for (unsigned index = 0; index < sizeof(Value); ++index) {
            if (reading_) {
                if (position_ == bytes_.size()) throw std::runtime_error("truncated savestate");
                decoded |= static_cast<std::uint64_t>(bytes_[position_++]) << (index * 8);
            } else {
                bytes_.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (index * 8)));
            }
        }
        if (reading_) value = static_cast<Value>(decoded);
    }

    void Field(bool& value) {
        std::uint8_t encoded = value ? 1 : 0;
        Field(encoded);
        if (encoded > 1) throw std::runtime_error("invalid savestate boolean");
        value = encoded != 0;
    }

    void Field(std::vector<std::uint8_t>& bytes) { for (auto& value : bytes) Field(value); }

    template <typename... Values>
    void Fields(Values&... values) { int result[] = {0, (Field(values), 0)...}; (void)result; }

    void Finish() const {
        if (reading_ && position_ != bytes_.size()) throw std::runtime_error("unexpected savestate data");
    }

private:
    std::vector<std::uint8_t>& bytes_;
    bool reading_;
    std::size_t position_{};
};

void WriteFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::string temporary = path + ".tmp";
    WriteBinaryFile(temporary, bytes);
#ifdef _WIN32
    if (!MoveFileExW(Utf8ToWide(temporary).c_str(), Utf8ToWide(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("cannot replace file: " + path);
    }
#else
    if (std::rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error("cannot replace file: " + path);
#endif
}

std::string GetBatterySavePath(const std::string& rom_path) {
    const auto dot = rom_path.find_last_of('.');
    if (dot != std::string::npos) {
        return rom_path.substr(0, dot) + ".sav";
    }
    return rom_path + ".sav";
}

}

class StateCodec {
public:
    static void Cpu(Archive& archive, Arm7Tdmi& cpu) {
        for (auto& value : cpu.state_.r) archive.Field(value);
        archive.Field(cpu.state_.cpsr);
        for (auto* bank : {&cpu.fiq_bank_, &cpu.irq_bank_, &cpu.supervisor_bank_, &cpu.abort_bank_, &cpu.undefined_bank_}) {
            for (auto& value : bank->r8) archive.Field(value);
            archive.Fields(bank->r13, bank->r14, bank->spsr);
        }
        for (auto& value : cpu.user_r8_12_) archive.Field(value);
        archive.Fields(cpu.user_sp_, cpu.user_lr_, cpu.steps_, cpu.cpu_execution_cycles_, cpu.halt_cycles_,
                       cpu.halt_entries_, cpu.halt_wakeups_, cpu.bios_swi_entries_,
                       cpu.halted_, cpu.stopped_);
        auto mode = static_cast<std::uint8_t>(cpu.GetBiosMode());
        archive.Field(mode);
        cpu.SetBiosMode(static_cast<BiosMode>(mode));
    }

    static void Bus(Archive& archive, MemoryBus& bus, std::uint32_t version) {
        archive.Fields(bus.ewram_, bus.iwram_, bus.io_, bus.palette_, bus.vram_, bus.oam_, bus.sram_,
                       bus.flash_id_, bus.flash_sequence_, bus.flash_bank_, bus.flash_command_,
                       bus.cycles_, bus.frames_, bus.dma_transfers_, bus.dma_cycles_, bus.scanline_cycles_,
                       bus.vcount_, bus.display_was_enabled_, bus.pressed_keys_);
        std::uint8_t power = static_cast<std::uint8_t>(bus.power_request_);
        archive.Field(power);
        if (power > 2) throw std::runtime_error("invalid power state");
        bus.power_request_ = static_cast<PowerRequest>(power);
        for (auto& dma : bus.dma_) {
            archive.Fields(dma.armed, dma.active, dma.initial_source, dma.initial_destination, dma.initial_count,
                           dma.source, dma.destination, dma.remaining, dma.control, dma.next_cycle, dma.word,
                           dma.source_control, dma.destination_control);
        }
        for (auto& timer : bus.timers_) {
            archive.Fields(timer.enabled, timer.irq_enabled, timer.cascade, timer.reload, timer.counter,
                           timer.prescaler, timer.cycle_remainder, timer.last_cycle, timer.next_overflow);
        }
        archive.Fields(bus.cpu_instruction_active_, bus.cpu_last_access_valid_, bus.cpu_last_access_address_,
                       bus.cpu_last_access_width_, bus.cpu_last_access_region_, bus.cpu_data_reads_,
                       bus.cpu_access_penalty_, bus.cpu_instruction_address_, bus.prefetch_valid_,
                       bus.prefetch_buffer_address_, bus.prefetch_next_address_, bus.prefetch_count_, bus.prefetch_region_);
        if (version >= 3) {
            std::uint8_t rtc_status = bus.rtc_.Status();
            std::uint64_t rtc_offset = static_cast<std::uint64_t>(bus.rtc_.TimeOffset());
            archive.Fields(rtc_status, rtc_offset);
            bus.rtc_.SetStatus(rtc_status);
            bus.rtc_.SetTimeOffset(static_cast<std::int64_t>(rtc_offset));
        }
    }

    static std::uint64_t BiosHash(const MemoryBus& bus) { return Hash(bus.bios_); }

    static void Validate(const MemoryBus& bus, const Arm7Tdmi& cpu) {
        if (bus.vcount_ >= 228 || bus.scanline_cycles_ >= 1232 || bus.prefetch_count_ > 8 ||
            bus.flash_sequence_ > 2 || static_cast<std::size_t>(bus.flash_bank_) * 65536 >= bus.sram_.size() ||
            bus.cpu_instruction_active_) throw std::runtime_error("invalid savestate hardware state");
        for (const auto& timer : bus.timers_) {
            if (timer.prescaler != 1 && timer.prescaler != 64 && timer.prescaler != 256 && timer.prescaler != 1024) {
                throw std::runtime_error("invalid savestate timer");
            }
            if (timer.cycle_remainder >= timer.prescaler || timer.last_cycle > bus.cycles_ ||
                (timer.enabled && !timer.cascade && timer.next_overflow <= bus.cycles_)) {
                throw std::runtime_error("invalid savestate timer schedule");
            }
        }
        for (const auto& dma : bus.dma_) {
            if (dma.source_control > 3 || dma.destination_control > 3 ||
                (dma.active && (dma.remaining == 0 || dma.remaining > 65536 || dma.next_cycle <= bus.cycles_))) {
                throw std::runtime_error("invalid savestate DMA schedule");
            }
        }
        const unsigned mode = cpu.state_.cpsr & 31u;
        if (mode != 0x10 && mode != 0x11 && mode != 0x12 && mode != 0x13 && mode != 0x17 && mode != 0x1B && mode != 0x1F) {
            throw std::runtime_error("invalid savestate CPU mode");
        }
    }
};

Runtime::Runtime(const std::string& rom_path, const std::string& bios_path, bool native)
    : rom_path_(rom_path),
      save_path_(GetBatterySavePath(rom_path)),
      rom_(RomImage::Load(rom_path)),
      bus_(new MemoryBus(rom_, bios_path)),
      cpu_(new Arm7Tdmi(*bus_)),
      native_(native) {
    cpu_->Reset(rom_.ResetVectorAddress());
    cpu_->EnableNative(native);
    try {
        const auto save_data = ReadBinaryFile(save_path_, 128u * 1024u);
        bus_->LoadSaveMemory(save_data);
    } catch (...) {
        // Battery save does not exist or cannot be opened; start fresh
    }
}

Runtime::~Runtime() {
    try {
        FlushBatterySave();
    } catch (...) {
    }
}

void Runtime::FlushBatterySave() {
    if (!bus_ || !bus_->SaveMemoryDirty()) return;
    WriteFile(save_path_, bus_->SaveMemory());
    bus_->ClearSaveMemoryDirty();
}

MemoryBus& Runtime::Bus() noexcept { return *bus_; }
Arm7Tdmi& Runtime::Cpu() noexcept { return *cpu_; }
void Runtime::SetPaused(bool paused) noexcept { paused_ = paused; }
bool Runtime::Paused() const noexcept { return paused_; }
void Runtime::SetAudioSink(AudioSink* sink) noexcept {
    audio_sink_ = sink;
    if (bus_) {
        bus_->GetApu().SetAudioSink(sink);
    }
}
RunResult Runtime::RunForCycles(Cycle cycles) { return paused_ ? RunResult{} : cpu_->RunForCycles(cycles); }
void Runtime::StepFrame() { cpu_->RunUntilFrameReady(bus_->Frames() + 1); }
void Runtime::Render(Framebuffer& framebuffer) const { PpuRenderer(*bus_).Render(framebuffer); }

void Runtime::SaveState(const std::string& path) const {
    const_cast<MemoryBus*>(bus_.get())->SynchronizeTimersTo(bus_->Cycles());
    const_cast<MemoryBus*>(bus_.get())->UpdateDisplayStatus();
    std::vector<std::uint8_t> payload;
    Archive archive(payload, false);
    std::uint32_t version = 3;
    StateCodec::Bus(archive, *bus_, version);
    StateCodec::Cpu(archive, *cpu_);
    std::vector<std::uint8_t> bytes;
    Archive header(bytes, false);
    std::uint64_t magic = 0x3154534142474Eull;
    std::uint64_t rom_hash = Hash(rom_.Bytes());
    std::uint64_t bios_hash = StateCodec::BiosHash(*bus_);
    std::uint64_t checksum = Hash(payload);
    header.Fields(magic, version, rom_hash, bios_hash, checksum);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    WriteFile(path, bytes);
}

void Runtime::LoadState(const std::string& path) {
    auto bytes = ReadBinaryFile(path, 2u * 1024u * 1024u);
    if (bytes.size() < 36) throw std::runtime_error("invalid savestate size");
    Archive header(bytes, true);
    std::uint64_t magic = 0, rom_hash = 0, bios_hash = 0, checksum = 0;
    std::uint32_t version = 0;
    header.Fields(magic, version, rom_hash, bios_hash, checksum);
    if (magic != 0x3154534142474Eull || (version != 2 && version != 3) || rom_hash != Hash(rom_.Bytes()) ||
        bios_hash != StateCodec::BiosHash(*bus_)) throw std::runtime_error("savestate version, ROM or BIOS mismatch");
    std::vector<std::uint8_t> payload(bytes.begin() + 36, bytes.end());
    if (Hash(payload) != checksum) throw std::runtime_error("savestate checksum mismatch");
    std::unique_ptr<MemoryBus> next_bus(new MemoryBus(*bus_));
    std::unique_ptr<Arm7Tdmi> next_cpu(new Arm7Tdmi(*next_bus));
    Archive archive(payload, true);
    StateCodec::Bus(archive, *next_bus, version);
    StateCodec::Cpu(archive, *next_cpu);
    archive.Finish();
    StateCodec::Validate(*next_bus, *next_cpu);
    next_bus->UpdateWaitcnt();
    next_bus->UpdateInterruptFlags();
    next_bus->RecalculateNextEvent();
    next_cpu->EnableNative(native_);
    cpu_.swap(next_cpu);
    bus_.swap(next_bus);
    bus_->sram_dirty_ = true;
    bus_->GetApu().SetAudioSink(audio_sink_);
}

}
