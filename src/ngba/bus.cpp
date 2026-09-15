#include "ngba/bus.hpp"
#include "ngba/file_io.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace ngba {
namespace {

constexpr std::uint32_t kEwramBase = 0x02000000u;
constexpr std::uint32_t kIwramBase = 0x03000000u;
constexpr std::uint32_t kIoBase = 0x04000000u;
constexpr std::uint32_t kPaletteBase = 0x05000000u;
constexpr std::uint32_t kVramBase = 0x06000000u;
constexpr std::uint32_t kOamBase = 0x07000000u;
constexpr std::uint32_t kRomBase = 0x08000000u;
constexpr std::uint32_t kSramBase = 0x0E000000u;
constexpr std::size_t kDma0Offset = 0x0B0;
constexpr std::size_t kDmaStride = 0x0C;
constexpr std::size_t kTimer0Offset = 0x100;
constexpr std::size_t kTimerStride = 0x04;
constexpr Cycle kNoEvent = std::numeric_limits<Cycle>::max();
// The first DMA model is still access-atomic. These are explicit provisional
// bus costs, not a claim that the final wait-state model is complete.
constexpr Cycle kDmaHalfwordCycles = 2u;
constexpr Cycle kDmaWordCycles = 4u;
constexpr std::size_t kIwramDiagnosticLimit = 2048u;

template <typename T>
bool InRange(std::uint32_t address, std::uint32_t base, std::uint32_t size,
             T& offset) {
    if (address < base || address - base >= size) {
        return false;
    }
    offset = static_cast<T>(address - base);
    return true;
}

std::uint16_t LoadLe16(std::uint8_t lo, std::uint8_t hi) {
    return static_cast<std::uint16_t>(lo) |
           (static_cast<std::uint16_t>(hi) << 8);
}

unsigned WaitstateNonSequential(std::uint16_t waitcnt,
                                unsigned select) {
    static const unsigned kWaits[4] = {4u, 3u, 2u, 8u};
    const unsigned shift = select == 0u ? 2u : select == 1u ? 5u : 8u;
    return kWaits[(waitcnt >> shift) & 0x03u];
}

unsigned SramWaitstate(std::uint16_t waitcnt) {
    static const unsigned kWaits[4] = {4u, 3u, 2u, 8u};
    return kWaits[waitcnt & 0x03u];
}

unsigned WaitstateSequential(std::uint16_t waitcnt, unsigned select) {
    if (select == 0u) return (waitcnt & (1u << 4)) != 0 ? 1u : 2u;
    if (select == 1u) return (waitcnt & (1u << 7)) != 0 ? 1u : 4u;
    return (waitcnt & (1u << 10)) != 0 ? 1u : 8u;
}

} // namespace

MemoryBus::MemoryBus(const RomImage& rom, const std::string& bios_path)
    : rom_(rom),
      bios_(0x4000, 0),
      ewram_(256 * 1024, 0),
      iwram_(32 * 1024, 0),
      io_(1024, 0),
      palette_(1024, 0),
      vram_(96 * 1024, 0),
      oam_(1024, 0),
      sram_(64 * 1024, 0xFF) {
    if (rom_.Size() == 0) {
        throw std::invalid_argument("cannot create a bus for an empty ROM");
    }

    const auto has_signature = [&](const std::string& signature) {
        return std::search(rom_.Bytes().begin(), rom_.Bytes().end(),
                           signature.begin(), signature.end()) != rom_.Bytes().end();
    };
    const bool flash_1m = has_signature("FLASH1M_V");
    flash_ = flash_1m || has_signature("FLASH512_V") || has_signature("FLASH_V");
    if (flash_1m) sram_.resize(128 * 1024, 0xFF);

    iwram_write_records_.reserve(kIwramDiagnosticLimit);
    watched_iwram_write_records_.reserve(kIwramDiagnosticLimit);
    watched_memory_write_records_.reserve(kIwramDiagnosticLimit);

    if (!bios_path.empty()) {
        auto image = ReadBinaryFile(bios_path, 0x4000u);
        if (image.size() != bios_.size()) {
            throw std::invalid_argument("BIOS image must be exactly 16384 bytes: " +
                                        bios_path);
        }
        bios_.swap(image);
        using_external_bios_ = true;
    } else {
        // The fallback only needs the real GBA IRQ ABI: the vector branches
        // to the dispatcher, which calls the ARM handler pointer at
        // 0x03007FFC and returns through SUBS PC,LR,#4.
        auto WriteBios32 = [this](std::size_t offset, std::uint32_t value) {
            bios_[offset] = static_cast<std::uint8_t>(value);
            bios_[offset + 1] = static_cast<std::uint8_t>(value >> 8);
            bios_[offset + 2] = static_cast<std::uint8_t>(value >> 16);
            bios_[offset + 3] = static_cast<std::uint8_t>(value >> 24);
        };
        WriteBios32(0x0018u, 0xEA000042u); // B 0x00000128
        WriteBios32(0x0128u, 0xE92D500Fu); // STMFD SP!,{r0-r3,r12,lr}
        WriteBios32(0x012Cu, 0xE3A00404u); // MOV r0,#0x04000000
        WriteBios32(0x0130u, 0xE28FE000u); // ADD lr,pc,#0
        WriteBios32(0x0134u, 0xE510F004u); // LDR pc,[r0,#-4]
        WriteBios32(0x0138u, 0xE8BD500Fu); // LDMFD SP!,{r0-r3,r12,lr}
        WriteBios32(0x013Cu, 0xE25EF004u); // SUBS pc,lr,#4
    }

    // KEYINPUT is active-low: at power-on all buttons are released.
    UpdateKeyInput();
    UpdateWaitcnt();
    UpdateInterruptFlags();
    next_ppu_event_cycle_ = kHblankStartCycle;
    RecalculateNextEvent();
}

std::uint8_t MemoryBus::Read8(std::uint32_t address) const {
    return ReadMapped8(address);
}

std::uint16_t MemoryBus::Read16(std::uint32_t address) const {
    const unsigned region = address >> 24;
    if (region >= 0x08 && region <= 0x0D && address < kSramBase) {
        const std::size_t rom_offset = static_cast<std::size_t>(address - kRomBase) % rom_.Size();
        if (rom_offset + 1 < rom_.Size()) {
            std::uint16_t val;
            std::memcpy(&val, rom_.Bytes().data() + rom_offset, 2);
            return val;
        }
    } else if (region == 0x03 && address >= kIwramBase && address < kIoBase) {
        const std::size_t offset = static_cast<std::size_t>(address - kIwramBase) % iwram_.size();
        if (offset + 1 < iwram_.size()) {
            std::uint16_t val;
            std::memcpy(&val, iwram_.data() + offset, 2);
            return val;
        }
    } else if (region == 0x02) {
        const std::size_t offset = address - kEwramBase;
        if (offset + 1 < ewram_.size()) {
            std::uint16_t val;
            std::memcpy(&val, ewram_.data() + offset, 2);
            return val;
        }
    } else if (region == 0x06) {
        const std::size_t offset = address - kVramBase;
        if (offset + 1 < vram_.size()) {
            std::uint16_t val;
            std::memcpy(&val, vram_.data() + offset, 2);
            return val;
        }
    }
    return LoadLe16(Read8(address), Read8(address + 1));
}

std::uint32_t MemoryBus::Read32(std::uint32_t address) const {
    const unsigned region = address >> 24;
    if (region >= 0x08 && region <= 0x0D && address < kSramBase) {
        const std::size_t rom_offset = static_cast<std::size_t>(address - kRomBase) % rom_.Size();
        if (rom_offset + 3 < rom_.Size()) {
            std::uint32_t val;
            std::memcpy(&val, rom_.Bytes().data() + rom_offset, 4);
            return val;
        }
    } else if (region == 0x03 && address >= kIwramBase && address < kIoBase) {
        const std::size_t offset = static_cast<std::size_t>(address - kIwramBase) % iwram_.size();
        if (offset + 3 < iwram_.size()) {
            std::uint32_t val;
            std::memcpy(&val, iwram_.data() + offset, 4);
            return val;
        }
    } else if (region == 0x02) {
        const std::size_t offset = address - kEwramBase;
        if (offset + 3 < ewram_.size()) {
            std::uint32_t val;
            std::memcpy(&val, ewram_.data() + offset, 4);
            return val;
        }
    } else if (region == 0x06) {
        const std::size_t offset = address - kVramBase;
        if (offset + 3 < vram_.size()) {
            std::uint32_t val;
            std::memcpy(&val, vram_.data() + offset, 4);
            return val;
        }
    }
    return static_cast<std::uint32_t>(Read8(address)) |
           (static_cast<std::uint32_t>(Read8(address + 1)) << 8) |
           (static_cast<std::uint32_t>(Read8(address + 2)) << 16) |
           (static_cast<std::uint32_t>(Read8(address + 3)) << 24);
}

void MemoryBus::Write8(std::uint32_t address, std::uint8_t value) {
    WriteMapped8(address, value);
}

void MemoryBus::Write16(std::uint32_t address, std::uint16_t value) {
    const unsigned region = address >> 24;
    if (region == 0x03 && address >= kIwramBase && address < kIoBase) {
        const std::size_t offset = static_cast<std::size_t>(address - kIwramBase) % iwram_.size();
        if (offset + 1 < iwram_.size()) {
            std::memcpy(iwram_.data() + offset, &value, 2);
            return;
        }
    } else if (region == 0x02) {
        const std::size_t offset = address - kEwramBase;
        if (offset + 1 < ewram_.size()) {
            std::memcpy(ewram_.data() + offset, &value, 2);
            return;
        }
    } else if (region == 0x06) {
        const std::size_t offset = address - kVramBase;
        if (offset + 1 < vram_.size()) {
            std::memcpy(vram_.data() + offset, &value, 2);
            return;
        }
    }
    Write8(address, static_cast<std::uint8_t>(value));
    Write8(address + 1, static_cast<std::uint8_t>(value >> 8));
}

void MemoryBus::Write32(std::uint32_t address, std::uint32_t value) {
    const unsigned region = address >> 24;
    if (region == 0x03 && address >= kIwramBase && address < kIoBase) {
        const std::size_t offset = static_cast<std::size_t>(address - kIwramBase) % iwram_.size();
        if (offset + 3 < iwram_.size()) {
            std::memcpy(iwram_.data() + offset, &value, 4);
            return;
        }
    } else if (region == 0x02) {
        const std::size_t offset = address - kEwramBase;
        if (offset + 3 < ewram_.size()) {
            std::memcpy(ewram_.data() + offset, &value, 4);
            return;
        }
    } else if (region == 0x06) {
        const std::size_t offset = address - kVramBase;
        if (offset + 3 < vram_.size()) {
            std::memcpy(vram_.data() + offset, &value, 4);
            return;
        }
    }
    Write8(address, static_cast<std::uint8_t>(value));
    Write8(address + 1, static_cast<std::uint8_t>(value >> 8));
    Write8(address + 2, static_cast<std::uint8_t>(value >> 16));
    Write8(address + 3, static_cast<std::uint8_t>(value >> 24));
}

void MemoryBus::ResetCpuTiming() noexcept {
    cpu_instruction_active_ = false;
    cpu_last_access_valid_ = false;
    cpu_last_access_address_ = 0;
    cpu_last_access_width_ = 0;
    cpu_last_access_region_ = 0;
    cpu_data_reads_ = 0;
    cpu_access_penalty_ = 0;
    cpu_instruction_address_ = 0;
    ClearPrefetch();
}

void MemoryBus::BeginCpuInstruction() noexcept {
    BeginCpuInstruction(0);
}

void MemoryBus::BeginCpuInstruction(std::uint32_t address) noexcept {
    cpu_instruction_active_ = true;
    cpu_instruction_address_ = address;
    cpu_data_reads_ = 0;
    cpu_access_penalty_ = 0;
}

std::uint16_t MemoryBus::CpuFetch16(std::uint32_t address) {
    RecordCpuAccess(address, 2u, false, true);
    return Read16(address);
}

std::uint32_t MemoryBus::CpuFetch32(std::uint32_t address) {
    RecordCpuAccess(address, 4u, false, true);
    return Read32(address);
}

std::uint8_t MemoryBus::CpuRead8(std::uint32_t address) {
    RecordCpuAccess(address, 1u, false, false);
    return Read8(address);
}

std::uint16_t MemoryBus::CpuRead16(std::uint32_t address) {
    RecordCpuAccess(address, 2u, false, false);
    return Read16(address);
}

std::uint32_t MemoryBus::CpuRead32(std::uint32_t address) {
    RecordCpuAccess(address, 4u, false, false);
    return Read32(address);
}

void MemoryBus::CpuWrite8(std::uint32_t address, std::uint8_t value) {
    RecordCpuAccess(address, 1u, true, false);
    Write8(address, value);
}

void MemoryBus::CpuWrite16(std::uint32_t address, std::uint16_t value) {
    RecordCpuAccess(address, 2u, true, false);
    Write16(address, value);
}

void MemoryBus::CpuWrite32(std::uint32_t address, std::uint32_t value) {
    RecordCpuAccess(address, 4u, true, false);
    Write32(address, value);
}

Cycle MemoryBus::EndCpuInstruction() noexcept {
    const Cycle penalty = cpu_access_penalty_;
    cpu_instruction_active_ = false;
    cpu_access_penalty_ = 0;
    return penalty;
}

void MemoryBus::RequestPower(PowerRequest request) noexcept {
    power_request_ = request;
}

PowerRequest MemoryBus::ConsumePowerRequest() noexcept {
    const PowerRequest request = power_request_;
    power_request_ = PowerRequest::None;
    return request;
}

void MemoryBus::SetKeys(std::uint16_t pressed) {
    pressed_keys_ = static_cast<std::uint16_t>(pressed & 0x03FFu);
    UpdateKeyInput();
    UpdateKeypadInterrupt();
}

std::uint16_t MemoryBus::PressedKeys() const noexcept {
    return pressed_keys_;
}

void MemoryBus::ResetRam(std::uint8_t flags) {
    if ((flags & 0x01u) != 0) std::fill(ewram_.begin(), ewram_.end(), 0);
    if ((flags & 0x02u) != 0) {
        // 0x03007E00..0x03007FFF belongs to the BIOS and is preserved by
        // RegisterRamReset. It contains the IRQ bookkeeping and SoftReset
        // selector used by the cartridge runtime.
        std::fill(iwram_.begin(), iwram_.begin() + 0x7E00u, 0);
    }
    if ((flags & 0x04u) != 0) std::fill(palette_.begin(), palette_.end(), 0);
    if ((flags & 0x08u) != 0) std::fill(vram_.begin(), vram_.end(), 0);
    if ((flags & 0x10u) != 0) std::fill(oam_.begin(), oam_.end(), 0);
    if ((flags & 0xE0u) != 0) {
        std::fill(io_.begin(), io_.end(), 0);
        power_request_ = PowerRequest::None;
        dma_ = std::array<DmaTransfer, 4>{};
        timers_ = std::array<Timer, 4>{};
        io_[0x00u] = 0x80u; // DISPCNT forced-blank after a RAM reset.
        io_[0x132u] = 0;
        io_[0x133u] = 0;
        UpdateKeyInput();
        UpdateWaitcnt();
        UpdateInterruptFlags();
        RecalculateNextEvent();
    }

    // RegisterRamReset always leaves the display in forced blank, even when
    // none of the optional RAM/register groups were requested. This is a
    // BIOS side effect, not a reason to clear the rest of the I/O block.
    io_[0x00u] = 0x80u;
    io_[0x01u] = 0;
}

void MemoryBus::ResetBiosWorkArea() {
    std::fill(iwram_.begin() + 0x7E00u, iwram_.end(), 0);
}

void MemoryBus::ClearDiagnostics() noexcept {
    iwram_write_count_ = 0;
    iwram_write_records_.clear();
    watched_iwram_write_records_.clear();
    watched_memory_write_records_.clear();
}

Cycle MemoryBus::IwramWriteCount() const noexcept {
    return iwram_write_count_;
}

const std::vector<IwramWriteRecord>& MemoryBus::IwramWriteRecords() const noexcept {
    return iwram_write_records_;
}

void MemoryBus::WatchIwramWrites(std::uint32_t address,
                                 std::uint32_t size) noexcept {
    iwram_watch_begin_ = address;
    iwram_watch_end_ = size > 0xFFFFFFFFu - address
        ? 0xFFFFFFFFu
        : address + size;
    watched_iwram_write_records_.clear();
}

const std::vector<IwramWriteRecord>&
MemoryBus::WatchedIwramWriteRecords() const noexcept {
    return watched_iwram_write_records_;
}

void MemoryBus::WatchMemoryWrites(std::uint32_t address,
                                  std::uint32_t size) noexcept {
    memory_watch_begin_ = address;
    memory_watch_end_ = size > 0xFFFFFFFFu - address
        ? 0xFFFFFFFFu
        : address + size;
    watched_memory_write_records_.clear();
}

const std::vector<MemoryWriteRecord>&
MemoryBus::WatchedMemoryWriteRecords() const noexcept {
    return watched_memory_write_records_;
}

void MemoryBus::Tick(std::uint32_t cycles) {
    AdvanceTo(cycles_ + static_cast<Cycle>(cycles));
}

Cycle MemoryBus::NextPpuEventCycle() const noexcept {
    const std::uint32_t until_event = scanline_cycles_ < kHblankStartCycle
        ? kHblankStartCycle - scanline_cycles_
        : kCyclesPerScanline - scanline_cycles_;
    return cycles_ + static_cast<Cycle>(until_event);
}

Cycle MemoryBus::NextDmaEventCycle() const noexcept {
    Cycle next = kNoEvent;
    for (const DmaTransfer& transfer : dma_) {
        if (transfer.active && transfer.next_cycle < next) {
            next = transfer.next_cycle;
        }
    }
    return next;
}

Cycle MemoryBus::NextTimerEventCycle() const noexcept {
    Cycle next = kNoEvent;
    for (const Timer& timer : timers_) {
        if (timer.enabled && !timer.cascade && timer.next_overflow < next) {
            next = timer.next_overflow;
        }
    }
    return next;
}

Cycle MemoryBus::NextEventCycle() const noexcept {
    return next_event_cycle_;
}

void MemoryBus::RecalculateNextEvent() noexcept {
    next_event_cycle_ = std::min(NextPpuEventCycle(),
                                 std::min(NextDmaEventCycle(), NextTimerEventCycle()));
}

void MemoryBus::AdvanceTo(Cycle target_cycle) {
    if (target_cycle < cycles_) {
        throw std::invalid_argument("cannot move the GBA clock backwards");
    }

    if (target_cycle < next_event_cycle_) {
        const Cycle delta = target_cycle - cycles_;
        scanline_cycles_ += static_cast<std::uint32_t>(delta);
        cycles_ = target_cycle;
        return;
    }

    while (true) {
        const Cycle ppu_event = NextPpuEventCycle();
        const Cycle dma_event = NextDmaEventCycle();
        const Cycle timer_event = NextTimerEventCycle();
        const Cycle event_cycle = std::min(
            ppu_event, std::min(dma_event, timer_event));
        if (event_cycle == kNoEvent || event_cycle > target_cycle) {
            if (cycles_ < target_cycle) {
                const Cycle delta = target_cycle - cycles_;
                scanline_cycles_ += static_cast<std::uint32_t>(delta);
                cycles_ = target_cycle;
            }
            break;
        }

        if (cycles_ < event_cycle) {
            const Cycle delta = event_cycle - cycles_;
            scanline_cycles_ += static_cast<std::uint32_t>(delta);
            cycles_ = event_cycle;
        }

        if (ppu_event == cycles_) {
            if (scanline_cycles_ == kHblankStartCycle) {
                const std::uint16_t dispstat = LoadLe16(io_[0x004], io_[0x005]);
                if ((dispstat & 0x0010u) != 0) RequestInterrupt(0x0002u);
                if (vcount_ < kVisibleScanlines) {
                    TriggerDma(2u);
                }
            }

            if (scanline_cycles_ == kCyclesPerScanline) {
                scanline_cycles_ = 0;
                vcount_ = static_cast<std::uint16_t>(
                    (vcount_ + 1u) % kScanlinesPerFrame);
                io_[0x006] = static_cast<std::uint8_t>(vcount_);
                io_[0x007] = static_cast<std::uint8_t>(vcount_ >> 8);

                const std::uint16_t dispstat = LoadLe16(io_[0x004], io_[0x005]);
                if (vcount_ == kVisibleScanlines) {
                    ++frames_;
                    if ((dispstat & 0x0008u) != 0) RequestInterrupt(0x0001u);
                    TriggerDma(1u);
                }
                if (vcount_ == static_cast<std::uint16_t>(dispstat >> 8) &&
                    (dispstat & 0x0020u) != 0) {
                    RequestInterrupt(0x0004u);
                }
            }
            UpdateDisplayStatus();
        }

        if (timer_event == cycles_) {
            SynchronizeTimersTo(cycles_);
            ProcessTimerEvents();
        }
        if (dma_event == cycles_) ProcessDmaEvent();
    }

    RecalculateNextEvent();
}

Cycle MemoryBus::Cycles() const noexcept {
    return cycles_;
}

Cycle MemoryBus::Frames() const noexcept {
    return frames_;
}

Cycle MemoryBus::DmaTransfers() const noexcept {
    return dma_transfers_;
}

Cycle MemoryBus::DmaCycles() const noexcept {
    return dma_cycles_;
}

bool MemoryBus::DmaActive() const noexcept {
    return NextDmaEventCycle() != kNoEvent;
}

bool MemoryBus::DisplayWasEnabled() const noexcept {
    return display_was_enabled_;
}

Cycle MemoryBus::DisplayControlWrites() const noexcept {
    return display_control_writes_;
}

std::uint32_t MemoryBus::LastDisplayControlWritePc() const noexcept {
    return last_display_control_write_pc_;
}

std::uint16_t MemoryBus::LastDisplayControlWriteValue() const noexcept {
    return last_display_control_write_value_;
}

std::uint16_t MemoryBus::VCount() const noexcept {
    return vcount_;
}

void MemoryBus::UpdateWaitcnt() noexcept {
    waitcnt_ = LoadLe16(io_[0x204u], io_[0x205u]);
    prefetch_enabled_ = (waitcnt_ & 0x4000u) != 0;
    sram_wait_ = SramWaitstate(waitcnt_);
    ws_seq_[0] = WaitstateSequential(waitcnt_, 0u);
    ws_seq_[1] = WaitstateSequential(waitcnt_, 1u);
    ws_seq_[2] = WaitstateSequential(waitcnt_, 2u);
    ws_non_seq_[0] = WaitstateNonSequential(waitcnt_, 0u);
    ws_non_seq_[1] = WaitstateNonSequential(waitcnt_, 1u);
    ws_non_seq_[2] = WaitstateNonSequential(waitcnt_, 2u);
}

void MemoryBus::UpdateInterruptFlags() noexcept {
    const std::uint16_t master = LoadLe16(io_[0x208u], io_[0x209u]);
    const std::uint16_t enable = LoadLe16(io_[0x200u], io_[0x201u]);
    const std::uint16_t flags = LoadLe16(io_[0x202u], io_[0x203u]);
    interrupt_requested_ = (enable & flags & 0x3FFFu) != 0;
    interrupt_pending_ = (master & 0x0001u) != 0 && interrupt_requested_;
}

bool MemoryBus::InterruptPending() const noexcept {
    return interrupt_pending_;
}

bool MemoryBus::InterruptRequested() const noexcept {
    return interrupt_requested_;
}

std::uint32_t MemoryBus::IrqHandlerAddress() const noexcept {
    return static_cast<std::uint32_t>(iwram_[0x7FFCu]) |
           (static_cast<std::uint32_t>(iwram_[0x7FFDu]) << 8) |
           (static_cast<std::uint32_t>(iwram_[0x7FFEu]) << 16) |
           (static_cast<std::uint32_t>(iwram_[0x7FFFu]) << 24);
}

bool MemoryBus::UsingExternalBios() const noexcept {
    return using_external_bios_;
}

static constexpr std::uint8_t kTimingRegionTable[16] = {
    0, 0, 2, 3, 4, 5, 6, 7, 8, 8, 10, 10, 12, 12, 14, 14
};

unsigned MemoryBus::CpuTimingRegion(std::uint32_t address) const noexcept {
    const unsigned top = address >> 24;
    return top < 16 ? kTimingRegionTable[top] : 15u;
}

Cycle MemoryBus::CpuAccessCycles(std::uint32_t address,
                                unsigned width,
                                bool sequential) const noexcept {
    return CpuAccessCycles(address, width, sequential, CpuTimingRegion(address));
}

Cycle MemoryBus::CpuAccessCycles(std::uint32_t address,
                                unsigned width,
                                bool sequential,
                                unsigned region) const noexcept {
    (void)address;
    if (region >= 8u && region <= 12u) {
        const unsigned select = region == 8u ? 0u : region == 10u ? 1u : 2u;
        const Cycle first = sequential ? ws_seq_[select] : ws_non_seq_[select];
        if (width <= 2u) return first;
        return first + ws_seq_[select];
    }
    if (region == 2u) {
        return width <= 2u ? 3u : 6u;
    }
    if (region == 14u) {
        return width * sram_wait_;
    }
    return 1u;
}

bool MemoryBus::PrefetchEnabled() const noexcept {
    return prefetch_enabled_;
}

void MemoryBus::ClearPrefetch() noexcept {
    prefetch_valid_ = false;
    prefetch_buffer_address_ = 0;
    prefetch_next_address_ = 0;
    prefetch_count_ = 0;
    prefetch_region_ = 0;
}

void MemoryBus::StartPrefetchStream(std::uint32_t address,
                                    unsigned width,
                                    unsigned region) noexcept {
    if (!prefetch_enabled_ ||
        !(region == 8u || region == 10u || region == 12u)) {
        ClearPrefetch();
        return;
    }
    prefetch_valid_ = true;
    prefetch_buffer_address_ = address + width;
    prefetch_next_address_ = address + width;
    prefetch_count_ = 0;
    prefetch_region_ = region;
}

bool MemoryBus::ConsumePrefetch(std::uint32_t address,
                                unsigned width,
                                unsigned region) noexcept {
    if (!prefetch_enabled_ || !prefetch_valid_ ||
        prefetch_region_ != region || address != prefetch_buffer_address_) {
        return false;
    }

    const unsigned units = (width + 1u) / 2u;
    if (prefetch_count_ < units) return false;
    prefetch_buffer_address_ += units * 2u;
    prefetch_count_ -= units;
    return true;
}

void MemoryBus::FillPrefetch(Cycle available_cycles) noexcept {
    if (!prefetch_enabled_ || !prefetch_valid_ || prefetch_count_ >= 8u) {
        return;
    }

    const unsigned select = prefetch_region_ == 10u ? 1u : prefetch_region_ == 12u ? 2u : 0u;
    const unsigned sequential_cycles = ws_seq_[select];
    if (sequential_cycles == 0u) return;

    const unsigned max_loads = 8u - prefetch_count_;
    const unsigned loads = std::min(static_cast<unsigned>(available_cycles / sequential_cycles), max_loads);
    if (loads != 0) {
        if (prefetch_count_ == 0u) {
            prefetch_buffer_address_ = prefetch_next_address_;
        }
        prefetch_count_ += loads;
        prefetch_next_address_ += loads * 2u;
    }
}

void MemoryBus::RecordCpuAccess(std::uint32_t address,
                                unsigned width,
                                bool write,
                                bool instruction_fetch) noexcept {
    if (!cpu_instruction_active_) return;

    const unsigned region = CpuTimingRegion(address);
    const bool sequential = cpu_last_access_valid_ &&
        cpu_last_access_region_ == region &&
        cpu_last_access_address_ + cpu_last_access_width_ == address;
    Cycle actual = CpuAccessCycles(address, width, sequential, region);

    const bool gamepak = region == 8u || region == 10u || region == 12u;
    if (gamepak) {
        if (instruction_fetch && !write && PrefetchEnabled()) {
            if (ConsumePrefetch(address, width, region)) {
                // A prefetched halfword costs only the base bus cycle. The
                // access is still counted once per 16-bit unit.
                actual = (width + 1u) / 2u;
            } else {
                ClearPrefetch();
                StartPrefetchStream(address, width, region);
            }
        } else {
            // A data transfer or a disabled prefetch path moves the cartridge
            // bus cursor away from the instruction stream.
            ClearPrefetch();
        }
    } else if (!instruction_fetch && !write) {
        // Non-cartridge accesses leave time for the Game Pak prefetch unit.
        // This first model uses the access duration as its available budget.
        FillPrefetch(actual);
    } else if (!instruction_fetch && write) {
        FillPrefetch(actual);
    }

    // InstructionCycles already models the ordinary one-cycle fetch and the
    // baseline data phases (first read=2, later reads=1, writes=1). Only add
    // wait-state excess here; this keeps the old ARM/Thumb internal model and
    // makes the ROM/EWRAM timing contribution observable independently.
    Cycle nominal = 1u;
    if (!instruction_fetch) {
        if (!write) nominal = cpu_data_reads_++ == 0u ? 2u : 1u;
    }
    if (actual > nominal) cpu_access_penalty_ += actual - nominal;

    cpu_last_access_valid_ = true;
    cpu_last_access_address_ = address;
    cpu_last_access_width_ = static_cast<std::uint8_t>(width);
    cpu_last_access_region_ = region;
}

std::uint8_t MemoryBus::ReadMapped8(std::uint32_t address) const {
    switch (address >> 24) {
    case 0x00:
        if (address < bios_.size()) return bios_[address];
        return 0;
    case 0x02: {
        const std::size_t offset = address - kEwramBase;
        if (offset < ewram_.size()) return ewram_[offset];
        return 0;
    }
    case 0x03:
        if (address >= kIwramBase && address < kIoBase) {
            return iwram_[static_cast<std::size_t>(address - kIwramBase) % iwram_.size()];
        }
        return 0;
    case 0x04: {
        const std::size_t offset = address - kIoBase;
        if (offset < io_.size()) {
            if (offset >= 0x100u && offset < 0x110u) {
                const_cast<MemoryBus*>(this)->SynchronizeTimersTo(cycles_);
            } else if (offset == 0x004u || offset == 0x005u) {
                const_cast<MemoryBus*>(this)->UpdateDisplayStatus();
            }
            return io_[offset];
        }
        return 0;
    }
    case 0x05: {
        const std::size_t offset = address - kPaletteBase;
        if (offset < palette_.size()) return palette_[offset];
        return 0;
    }
    case 0x06: {
        const std::size_t offset = address - kVramBase;
        if (offset < vram_.size()) return vram_[offset];
        return 0;
    }
    case 0x07: {
        const std::size_t offset = address - kOamBase;
        if (offset < oam_.size()) return oam_[offset];
        return 0;
    }
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D: {
        if (address >= kRomBase && address < kSramBase) {
            const std::size_t rom_offset =
                static_cast<std::size_t>(address - kRomBase) % rom_.Size();
            return rom_.Bytes()[rom_offset];
        }
        return 0;
    }
    case 0x0E: {
        if (address >= kSramBase && address < 0x10000000u) {
            const std::size_t offset = address & 0xFFFFu;
            if (flash_ && flash_id_ && offset < 2u) {
                return offset == 0 ? 0xC2u : (sram_.size() > 65536u ? 0x09u : 0x1Cu);
            }
            return sram_[offset + static_cast<std::size_t>(flash_bank_) * 65536u];
        }
        return 0;
    }
    default:
        return 0;
    }
}

void MemoryBus::WriteMapped8(std::uint32_t address, std::uint8_t value) {
    RecordWatchedMemoryWrite(address, value);

    switch (address >> 24) {
    case 0x02: {
        const std::size_t offset = address - kEwramBase;
        if (offset < ewram_.size()) {
            ewram_[offset] = value;
        }
        return;
    }
    case 0x03: {
        if (address >= kIwramBase && address < kIoBase) {
            const std::size_t offset = static_cast<std::size_t>(address - kIwramBase) % iwram_.size();
            iwram_[offset] = value;
            RecordIwramWrite(kIwramBase + static_cast<std::uint32_t>(offset), value);
        }
        return;
    }
    case 0x04: {
        const std::size_t offset = address - kIoBase;
        if (offset < io_.size()) {
            if (offset == 0x006u || offset == 0x007u) return;
            if (offset == 0x004u) {
                io_[offset] = static_cast<std::uint8_t>((io_[offset] & 0x07u) | (value & 0xF8u));
                return;
            }
            if (offset == 0x204u) {
                io_[offset] = value;
                ClearPrefetch();
                UpdateWaitcnt();
                return;
            }
            if (offset == 0x205u) {
                io_[offset] = static_cast<std::uint8_t>(value & 0x7Fu);
                ClearPrefetch();
                UpdateWaitcnt();
                return;
            }
            if (offset == 0x200u || offset == 0x201u) {
                io_[offset] = value;
                UpdateInterruptFlags();
                return;
            }
            if (offset == 0x202u || offset == 0x203u) {
                io_[offset] = static_cast<std::uint8_t>(io_[offset] & ~value);
                UpdateInterruptFlags();
                return;
            }
            if (offset == 0x130u || offset == 0x131u) return;
            if (offset == 0x132u || offset == 0x133u) {
                io_[offset] = offset == 0x133u ? static_cast<std::uint8_t>(value & 0xC0u) : value;
                UpdateKeypadInterrupt();
                return;
            }
            if (offset == 0x208u) {
                io_[offset] = static_cast<std::uint8_t>(value & 0x01u);
                UpdateInterruptFlags();
                return;
            }
            if (offset == 0x209u) {
                io_[offset] = 0;
                UpdateInterruptFlags();
                return;
            }
            if (offset == 0x301u) {
                RequestPower((value & 0x80u) != 0 ? PowerRequest::Stop : PowerRequest::Halt);
                return;
            }
            if (offset >= kTimer0Offset && offset < kTimer0Offset + 4u * kTimerStride) {
                const unsigned timer_index = static_cast<unsigned>((offset - kTimer0Offset) / kTimerStride);
                const std::size_t register_offset = (offset - kTimer0Offset) % kTimerStride;
                if (register_offset < 2u) {
                    WriteTimerData(timer_index, register_offset, value);
                } else {
                    io_[offset] = value;
                    UpdateTimerControl(timer_index);
                }
                return;
            }
            io_[offset] = value;
            if (offset < 2u) {
                ++display_control_writes_;
                last_display_control_write_pc_ = cpu_instruction_active_ ? cpu_instruction_address_ : 0;
                last_display_control_write_value_ = LoadLe16(io_[0x000u], io_[0x001u]);
            }
            const std::uint16_t dispcnt = LoadLe16(io_[0x000u], io_[0x001u]);
            if (offset < 2u && (dispcnt & 0x0080u) == 0) {
                display_was_enabled_ = true;
            }
            MaybeRunDma(offset);
        }
        return;
    }
    case 0x05: {
        const std::size_t offset = address - kPaletteBase;
        if (offset < palette_.size()) palette_[offset] = value;
        return;
    }
    case 0x06: {
        const std::size_t offset = address - kVramBase;
        if (offset < vram_.size()) vram_[offset] = value;
        return;
    }
    case 0x07: {
        const std::size_t offset = address - kOamBase;
        if (offset < oam_.size()) oam_[offset] = value;
        return;
    }
    case 0x0E: {
        if (address >= kSramBase && address < 0x10000000u) {
            WriteSave(address & 0xFFFFu, value);
        }
        return;
    }
    default:
        return;
    }
}

void MemoryBus::WriteSave(std::uint32_t offset, std::uint8_t value) {
    const std::size_t target = offset + static_cast<std::size_t>(flash_bank_) * 65536u;
    if (!flash_) {
        sram_[offset] = value;
        return;
    }
    if (flash_command_ == 0xA0u) {
        sram_[target] &= value;
        flash_command_ = 0;
        return;
    }
    if (flash_command_ == 0xB0u) {
        if (offset == 0 && sram_.size() > 65536u) flash_bank_ = value & 1u;
        flash_command_ = 0;
        return;
    }
    if (value == 0xF0u) {
        flash_id_ = false;
        flash_sequence_ = 0;
        flash_command_ = 0;
        return;
    }
    if (flash_sequence_ == 0) {
        flash_sequence_ = offset == 0x5555u && value == 0xAAu ? 1 : 0;
        return;
    }
    if (flash_sequence_ == 1) {
        flash_sequence_ = offset == 0x2AAAu && value == 0x55u ? 2 : 0;
        return;
    }
    flash_sequence_ = 0;
    if (flash_command_ == 0x80u) {
        if (value == 0x30u) {
            const std::size_t begin = target & ~std::size_t(0xFFFu);
            std::fill(sram_.begin() + begin, sram_.begin() + begin + 4096u, 0xFF);
        } else if (offset == 0x5555u && value == 0x10u) {
            std::fill(sram_.begin(), sram_.end(), 0xFF);
        }
        flash_command_ = 0;
        return;
    }
    if (offset != 0x5555u) return;
    if (value == 0x90u) flash_id_ = true;
    else if (value == 0xA0u || value == 0xB0u || value == 0x80u) {
        flash_command_ = value;
    }
}

void MemoryBus::WriteTimerData(unsigned timer_index,
                               std::size_t register_offset,
                               std::uint8_t value) {
    if (timer_index >= timers_.size() || register_offset >= 2u) return;

    // A register write occurs at the current bus timestamp. Bring an active
    // timer up to that timestamp before changing its reload value.
    SynchronizeTimersTo(cycles_);

    const std::size_t base = kTimer0Offset + timer_index * kTimerStride;
    io_[base + register_offset] = value;
    Timer& timer = timers_[timer_index];
    timer.reload = LoadLe16(io_[base], io_[base + 1u]);
    if (!timer.enabled) {
        timer.counter = timer.reload;
        timer.last_cycle = cycles_;
        timer.cycle_remainder = 0;
    }
}

void MemoryBus::UpdateTimerControl(unsigned timer_index) {
    if (timer_index >= timers_.size()) return;

    Timer& timer = timers_[timer_index];
    const bool was_enabled = timer.enabled;
    if (was_enabled) SynchronizeTimersTo(cycles_);

    const std::size_t base = kTimer0Offset + timer_index * kTimerStride;
    const std::uint16_t control = LoadLe16(io_[base + 2u], io_[base + 3u]);
    const unsigned prescaler_select = control & 0x0003u;
    const std::uint32_t prescaler = prescaler_select == 0u
        ? 1u
        : prescaler_select == 1u
            ? 64u
            : prescaler_select == 2u ? 256u : 1024u;
    const bool new_cascade = (control & 0x0004u) != 0;
    const bool new_irq_enabled = (control & 0x0040u) != 0;
    const bool new_enabled = (control & 0x0080u) != 0;

    timer.prescaler = prescaler;
    timer.cascade = new_cascade;
    timer.irq_enabled = new_irq_enabled;
    timer.enabled = new_enabled;
    timer.last_cycle = cycles_;
    timer.cycle_remainder = 0;

    if (!new_enabled) {
        timer.next_overflow = kNoEvent;
        RecalculateNextEvent();
        return;
    }

    // Enabling a timer loads its current counter from the reload register.
    // Changing control on an already-running timer preserves the counter but
    // restarts its fractional prescaler at this register-write timestamp.
    if (!was_enabled) timer.counter = timer.reload;
    ScheduleTimerOverflow(timer_index);
}

void MemoryBus::ScheduleTimerOverflow(unsigned timer_index) {
    if (timer_index >= timers_.size()) return;
    Timer& timer = timers_[timer_index];
    if (!timer.enabled || timer.cascade) {
        timer.next_overflow = kNoEvent;
        RecalculateNextEvent();
        return;
    }

    const Cycle ticks = static_cast<Cycle>(0x10000u - timer.counter);
    timer.next_overflow = cycles_ + ticks * timer.prescaler;
    RecalculateNextEvent();
}

void MemoryBus::SynchronizeTimersTo(Cycle target_cycle) {
    for (unsigned timer_index = 0; timer_index < timers_.size();
         ++timer_index) {
        Timer& timer = timers_[timer_index];
        if (!timer.enabled || timer.cascade || target_cycle <= timer.last_cycle) {
            continue;
        }

        const Cycle elapsed = target_cycle - timer.last_cycle;
        const Cycle total = elapsed + timer.cycle_remainder;
        const Cycle ticks = total / timer.prescaler;
        timer.cycle_remainder = static_cast<std::uint32_t>(
            total % timer.prescaler);
        if (ticks != 0) {
            const std::uint32_t updated =
                static_cast<std::uint32_t>(timer.counter) +
                static_cast<std::uint32_t>(ticks & 0xFFFFu);
            timer.counter = static_cast<std::uint16_t>(updated);

            const std::size_t base =
                kTimer0Offset + timer_index * kTimerStride;
            io_[base] = static_cast<std::uint8_t>(timer.counter);
            io_[base + 1u] = static_cast<std::uint8_t>(timer.counter >> 8);
        }
        timer.last_cycle = target_cycle;
    }
}

void MemoryBus::ProcessTimerEvents() {
    // A timer overflow can cascade through the following timers. The loop
    // also handles independent timers whose scheduled edge shares a cycle.
    bool processed = false;
    do {
        processed = false;
        for (unsigned timer_index = 0; timer_index < timers_.size();
             ++timer_index) {
            const Timer& timer = timers_[timer_index];
            if (timer.enabled && !timer.cascade &&
                timer.next_overflow == cycles_) {
                ProcessTimerOverflow(timer_index);
                processed = true;
            }
        }
    } while (processed);
}

void MemoryBus::ProcessTimerOverflow(unsigned timer_index) {
    if (timer_index >= timers_.size()) return;
    Timer& timer = timers_[timer_index];
    if (!timer.enabled || timer.cascade || timer.next_overflow != cycles_) {
        return;
    }

    timer.counter = timer.reload;
    timer.last_cycle = cycles_;
    timer.cycle_remainder = 0;
    const std::size_t base = kTimer0Offset + timer_index * kTimerStride;
    io_[base] = static_cast<std::uint8_t>(timer.counter);
    io_[base + 1u] = static_cast<std::uint8_t>(timer.counter >> 8);
    if (timer.irq_enabled) {
        RequestInterrupt(static_cast<std::uint16_t>(1u << (3u + timer_index)));
    }
    ScheduleTimerOverflow(timer_index);
    IncrementCascadedTimer(timer_index + 1u);
}

void MemoryBus::IncrementCascadedTimer(unsigned timer_index) {
    if (timer_index >= timers_.size()) return;
    Timer& timer = timers_[timer_index];
    if (!timer.enabled || !timer.cascade) return;

    timer.last_cycle = cycles_;
    timer.cycle_remainder = 0;
    if (timer.counter != 0xFFFFu) {
        ++timer.counter;
        const std::size_t base = kTimer0Offset + timer_index * kTimerStride;
        io_[base] = static_cast<std::uint8_t>(timer.counter);
        io_[base + 1u] = static_cast<std::uint8_t>(timer.counter >> 8);
        return;
    }

    timer.counter = timer.reload;
    const std::size_t base = kTimer0Offset + timer_index * kTimerStride;
    io_[base] = static_cast<std::uint8_t>(timer.counter);
    io_[base + 1u] = static_cast<std::uint8_t>(timer.counter >> 8);
    if (timer.irq_enabled) {
        RequestInterrupt(static_cast<std::uint16_t>(1u << (3u + timer_index)));
    }
    IncrementCascadedTimer(timer_index + 1u);
}

void MemoryBus::UpdateDisplayStatus() {
    // DISPSTAT bits 0-2 are status flags; the upper byte is the programmable
    // VCOUNT compare value. The interrupt request bits are left untouched.
    std::uint16_t dispstat = LoadLe16(io_[0x004], io_[0x005]);
    dispstat = static_cast<std::uint16_t>(dispstat & ~0x0007u);
    if (vcount_ >= kVisibleScanlines && vcount_ < 227u) {
        dispstat = static_cast<std::uint16_t>(dispstat | 0x0001u);
    }
    if (scanline_cycles_ >= kHblankStartCycle) {
        dispstat = static_cast<std::uint16_t>(dispstat | 0x0002u);
    }
    if (vcount_ == static_cast<std::uint16_t>(dispstat >> 8)) {
        dispstat = static_cast<std::uint16_t>(dispstat | 0x0004u);
    }
    io_[0x004] = static_cast<std::uint8_t>(dispstat);
    io_[0x005] = static_cast<std::uint8_t>(dispstat >> 8);
}

void MemoryBus::RequestInterrupt(std::uint16_t mask) {
    const std::uint16_t flags = static_cast<std::uint16_t>(
        LoadLe16(io_[0x202], io_[0x203]) | mask);
    io_[0x202] = static_cast<std::uint8_t>(flags);
    io_[0x203] = static_cast<std::uint8_t>(flags >> 8);
    UpdateInterruptFlags();
}

void MemoryBus::UpdateKeyInput() {
    const std::uint16_t released = static_cast<std::uint16_t>(
        (~pressed_keys_) & 0x03FFu);
    io_[0x130u] = static_cast<std::uint8_t>(released);
    io_[0x131u] = static_cast<std::uint8_t>(released >> 8);
}

void MemoryBus::UpdateKeypadInterrupt() {
    const std::uint16_t keycnt = LoadLe16(io_[0x132u], io_[0x133u]);
    if ((keycnt & 0x8000u) == 0) return;

    const std::uint16_t selected = static_cast<std::uint16_t>(
        keycnt & 0x03FFu);
    if (selected == 0) return;

    const bool all = (keycnt & 0x4000u) != 0;
    const std::uint16_t pressed = static_cast<std::uint16_t>(
        pressed_keys_ & selected);
    const bool condition = all ? pressed == selected : pressed != 0;
    if (condition) RequestInterrupt(0x1000u);
}

void MemoryBus::RecordIwramWrite(std::uint32_t address,
                                  std::uint8_t value) {
    if (!cpu_instruction_active_) return;

    ++iwram_write_count_;
    const IwramWriteRecord record{
        cycles_, address, value, cpu_instruction_address_};
    if (iwram_write_records_.size() < kIwramDiagnosticLimit) {
        iwram_write_records_.push_back(record);
    }
    if (address >= iwram_watch_begin_ && address < iwram_watch_end_ &&
        watched_iwram_write_records_.size() < kIwramDiagnosticLimit) {
        watched_iwram_write_records_.push_back(record);
    }
}

void MemoryBus::RecordWatchedMemoryWrite(std::uint32_t address,
                                         std::uint8_t value) {
    if (!cpu_instruction_active_ ||
        address < memory_watch_begin_ || address >= memory_watch_end_ ||
        watched_memory_write_records_.size() >= kIwramDiagnosticLimit) {
        return;
    }

    watched_memory_write_records_.push_back(MemoryWriteRecord{
        cycles_, address, value, cpu_instruction_address_});
}

void MemoryBus::MaybeRunDma(std::size_t io_offset) {
    if (io_offset < kDma0Offset) return;
    const std::size_t relative = io_offset - kDma0Offset;
    if (relative >= 4u * kDmaStride) return;

    const unsigned channel = static_cast<unsigned>(relative / kDmaStride);
    const std::size_t channel_offset = kDma0Offset + channel * kDmaStride;
    // DMA control is the final halfword in the channel register block. A
    // byte write to its high byte is the point at which a word/halfword write
    // has supplied the complete enable bit and timing mode.
    if (io_offset == channel_offset + 0x0Bu) {
        StartDma(channel);
    }
}

void MemoryBus::StartDma(unsigned channel) {
    if (channel >= 4) return;

    const std::size_t base = kDma0Offset + channel * kDmaStride;
    const std::uint16_t control = LoadLe16(io_[base + 0x0Au], io_[base + 0x0Bu]);
    if ((control & 0x8000u) == 0) {
        dma_[channel] = DmaTransfer{};
        return;
    }

    std::uint32_t source = static_cast<std::uint32_t>(io_[base]) |
                           (static_cast<std::uint32_t>(io_[base + 1]) << 8) |
                           (static_cast<std::uint32_t>(io_[base + 2]) << 16) |
                           (static_cast<std::uint32_t>(io_[base + 3]) << 24);
    std::uint32_t destination = static_cast<std::uint32_t>(io_[base + 4]) |
                               (static_cast<std::uint32_t>(io_[base + 5]) << 8) |
                               (static_cast<std::uint32_t>(io_[base + 6]) << 16) |
                               (static_cast<std::uint32_t>(io_[base + 7]) << 24);
    std::uint32_t count = static_cast<std::uint32_t>(
        LoadLe16(io_[base + 0x08u], io_[base + 0x09u]));
    if (count == 0) count = channel == 3 ? 0x10000u : 0x4000u;

    const bool word = (control & 0x0400u) != 0;
    const unsigned destination_control = (control >> 5) & 0x03u;
    const unsigned source_control = (control >> 7) & 0x03u;
    const unsigned timing = (control >> 12) & 0x03u;

    DmaTransfer& transfer = dma_[channel];
    transfer = DmaTransfer{};
    // Immediate starts on the next bus slot; HBlank/VBlank stay armed until
    // the corresponding PPU edge. FIFO timing remains intentionally absent.
    transfer.armed = timing <= 2u;
    transfer.active = timing == 0u;
    transfer.initial_source = source;
    transfer.initial_destination = destination;
    transfer.initial_count = count;
    transfer.source = source;
    transfer.destination = destination;
    transfer.remaining = count;
    transfer.control = control;
    transfer.next_cycle = transfer.active ? cycles_ + 1u : 0;
    transfer.word = word;
    transfer.source_control = source_control;
    transfer.destination_control = destination_control;
    RecalculateNextEvent();
}

void MemoryBus::TriggerDma(unsigned timing) {
    if (timing == 0u || timing > 2u) return;

    for (DmaTransfer& transfer : dma_) {
        if (!transfer.armed || transfer.active ||
            ((transfer.control >> 12) & 0x03u) != timing) {
            continue;
        }

        transfer.remaining = transfer.initial_count;
        // Source continues across repeated triggers. Destination mode 3 is
        // the documented reload mode and returns to the original address.
        if (transfer.destination_control == 3u) {
            transfer.destination = transfer.initial_destination;
        }
        transfer.active = true;
        transfer.next_cycle = cycles_ + 1u;
    }
    RecalculateNextEvent();
}

void MemoryBus::ProcessDmaEvent() {
    unsigned channel = 4;
    Cycle selected_cycle = kNoEvent;
    for (unsigned i = 0; i < dma_.size(); ++i) {
        if (!dma_[i].active) continue;
        if (dma_[i].next_cycle < selected_cycle ||
            (dma_[i].next_cycle == selected_cycle && i < channel)) {
            selected_cycle = dma_[i].next_cycle;
            channel = i;
        }
    }
    if (channel >= dma_.size() || selected_cycle != cycles_) return;

    DmaTransfer& transfer = dma_[channel];
    const std::uint32_t unit = transfer.word ? 4u : 2u;
    if (transfer.word) {
        const std::uint32_t value = Read32(transfer.source);
        Write32(transfer.destination, value);
    } else {
        const std::uint16_t value = Read16(transfer.source);
        Write16(transfer.destination, value);
    }

    auto StepAddress = [unit](std::uint32_t& address, unsigned mode) {
        if (mode == 0) address += unit;
        else if (mode == 1) address -= unit;
    };

    StepAddress(transfer.source, transfer.source_control);
    StepAddress(transfer.destination, transfer.destination_control == 3u ? 0u : transfer.destination_control);
    --transfer.remaining;
    dma_cycles_ += transfer.word ? kDmaWordCycles : kDmaHalfwordCycles;

    const std::size_t base = kDma0Offset + channel * kDmaStride;
    for (unsigned i = 0; i < 4; ++i) {
        io_[base + i] = static_cast<std::uint8_t>(transfer.source >> (i * 8));
        io_[base + 4 + i] = static_cast<std::uint8_t>(
            transfer.destination >> (i * 8));
    }

    if (transfer.remaining == 0) {
        ++dma_transfers_;
        const unsigned timing = (transfer.control >> 12) & 0x03u;
        const bool repeating = (transfer.control & 0x0200u) != 0;
        // Immediate DMA has no future trigger in this model. HBlank/VBlank
        // DMA remains armed only when the repeat bit requests it.
        if (timing == 0u || !repeating) {
            io_[base + 0x0Bu] = static_cast<std::uint8_t>(
                io_[base + 0x0Bu] & 0x7Fu);
            transfer.armed = false;
        }
        transfer.active = false;
        transfer.next_cycle = 0;
    } else {
        transfer.next_cycle = cycles_ +
            (transfer.word ? kDmaWordCycles : kDmaHalfwordCycles);
    }

    // A lower-priority channel that was already due cannot perform another
    // unit at the same timestamp. It waits for the next available bus slot.
    for (unsigned i = 0; i < dma_.size(); ++i) {
        if (i == channel || !dma_[i].active ||
            dma_[i].next_cycle > cycles_) {
            continue;
        }
        dma_[i].next_cycle = cycles_ +
            (dma_[i].word ? kDmaWordCycles : kDmaHalfwordCycles);
    }
    RecalculateNextEvent();
}

} // namespace ngba
