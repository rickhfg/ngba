#pragma once

#include "ngba/rom.hpp"
#include "ngba/rtc.hpp"
#include "ngba/eeprom.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ngba {

using Cycle = std::uint64_t;

enum class PowerRequest : std::uint8_t {
    None,
    Halt,
    Stop,
};

struct IwramWriteRecord {
    Cycle cycle{};
    std::uint32_t address{};
    std::uint8_t value{};
    std::uint32_t pc{};
};

struct MemoryWriteRecord {
    Cycle cycle{};
    std::uint32_t address{};
    std::uint8_t value{};
    std::uint32_t pc{};
};

/// First-pass GBA address bus.
///
/// Devices are represented as byte arrays initially. Their behavioural
/// registers will move behind dedicated devices as the CPU reaches them; the
/// flat mapping here gives the interpreter a deterministic memory surface from
/// the first instruction.
class MemoryBus {
public:
    static constexpr Cycle kClockFrequency = 16777216u;
    static constexpr Cycle kCyclesPerScanline = 1232u;
    static constexpr std::uint16_t kScanlinesPerFrame = 228u;
    static constexpr std::uint16_t kVisibleScanlines = 160u;
    static constexpr std::uint32_t kHblankStartCycle = 960u;

    explicit MemoryBus(const RomImage& rom,
                       const std::string& bios_path = std::string());

    std::uint8_t Read8(std::uint32_t address) const;
    std::uint16_t Read16(std::uint32_t address) const;
    std::uint32_t Read32(std::uint32_t address) const;

    void Write8(std::uint32_t address, std::uint8_t value);
    void Write16(std::uint32_t address, std::uint16_t value);
    void Write32(std::uint32_t address, std::uint32_t value);

    /// CPU-owned accesses used by the timing-aware interpreter. Ordinary
    /// Read/Write calls remain side-effect free with respect to CPU timing so
    /// that devices, renderers and tests can inspect memory directly.
    void ResetCpuTiming() noexcept;
    void BeginCpuInstruction() noexcept;
    void BeginCpuInstruction(std::uint32_t address) noexcept;
    std::uint16_t CpuFetch16(std::uint32_t address);
    std::uint32_t CpuFetch32(std::uint32_t address);
    std::uint8_t CpuRead8(std::uint32_t address);
    std::uint16_t CpuRead16(std::uint32_t address);
    std::uint32_t CpuRead32(std::uint32_t address);
    void CpuWrite8(std::uint32_t address, std::uint8_t value);
    void CpuWrite16(std::uint32_t address, std::uint16_t value);
    void CpuWrite32(std::uint32_t address, std::uint32_t value);
    Cycle EndCpuInstruction() noexcept;

    /// Request a CPU power-state transition, as a HALTCNT write would.
    void RequestPower(PowerRequest request) noexcept;
    PowerRequest ConsumePowerRequest() noexcept;

    /// Set the currently pressed GBA buttons using the KEYINPUT bit layout.
    /// A one bit means that the corresponding button is physically pressed;
    /// KEYINPUT exposes the inverse, active-low representation to the guest.
    void SetKeys(std::uint16_t pressed);
    std::uint16_t PressedKeys() const noexcept;

    /// Implements the RAM portion of BIOS RegisterRamReset.
    void ResetRam(std::uint8_t flags);

    /// Clears the BIOS-owned IWRAM work area used by SoftReset.
    void ResetBiosWorkArea();

    /// Clears bounded guest-write diagnostics collected since the last reset.
    void ClearDiagnostics() noexcept;
    Cycle IwramWriteCount() const noexcept;
    const std::vector<IwramWriteRecord>& IwramWriteRecords() const noexcept;
    void WatchIwramWrites(std::uint32_t address, std::uint32_t size) noexcept;
    const std::vector<IwramWriteRecord>&
        WatchedIwramWriteRecords() const noexcept;
    void WatchMemoryWrites(std::uint32_t address, std::uint32_t size) noexcept;
    const std::vector<MemoryWriteRecord>&
        WatchedMemoryWriteRecords() const noexcept;

    /// Advance the master clock by a relative number of cycles.
    void Tick(std::uint32_t cycles);
    /// Advance the master clock to an absolute timestamp. CPU instructions
    /// are not executed here; PPU/event state is processed in timestamp order.
    void AdvanceTo(Cycle target_cycle);
    Cycle NextEventCycle() const noexcept;
    Cycle Cycles() const noexcept;
    Cycle Frames() const noexcept;
    Cycle DmaTransfers() const noexcept;
    Cycle DmaCycles() const noexcept;
    bool DmaActive() const noexcept;
    bool DisplayWasEnabled() const noexcept;
    Cycle DisplayControlWrites() const noexcept;
    std::uint32_t LastDisplayControlWritePc() const noexcept;
    std::uint16_t LastDisplayControlWriteValue() const noexcept;
    std::uint16_t VCount() const noexcept;
    bool InterruptRequested() const noexcept;
    bool InterruptPending() const noexcept;
    std::uint32_t IrqHandlerAddress() const noexcept;
    bool UsingExternalBios() const noexcept;

    const std::vector<std::uint8_t>& Vram() const noexcept { return vram_; }
    const std::vector<std::uint8_t>& Palette() const noexcept { return palette_; }
    const std::vector<std::uint8_t>& Oam() const noexcept { return oam_; }
    const std::vector<std::uint8_t>& Io() const noexcept { return io_; }

    bool SaveMemoryDirty() const noexcept {
        return eeprom_.Enabled() ? eeprom_.Dirty() : sram_dirty_;
    }
    void ClearSaveMemoryDirty() noexcept {
        if (eeprom_.Enabled()) eeprom_.ClearDirty();
        sram_dirty_ = false;
    }
    const std::vector<std::uint8_t>& SaveMemory() const noexcept {
        return eeprom_.Enabled() ? eeprom_.Data() : sram_;
    }
    void LoadSaveMemory(const std::vector<std::uint8_t>& data);

    const Rtc& GetRtc() const noexcept { return rtc_; }
    Rtc& GetRtc() noexcept { return rtc_; }

    const Eeprom& GetEeprom() const noexcept { return eeprom_; }
    Eeprom& GetEeprom() noexcept { return eeprom_; }
    bool IsEepromAddress(std::uint32_t address) const noexcept;

private:
    friend class StateCodec;
    friend class Runtime;
    void WriteSave(std::uint32_t offset, std::uint8_t value);
    std::uint8_t ReadMapped8(std::uint32_t address) const;
    void WriteMapped8(std::uint32_t address, std::uint8_t value);
    void MaybeRunDma(std::size_t io_offset);
    void StartDma(unsigned channel);
    void TriggerDma(unsigned timing);
    void ProcessDmaEvent();
    void WriteTimerData(unsigned timer_index, std::size_t register_offset,
                        std::uint8_t value);
    void UpdateTimerControl(unsigned timer_index);
    void ScheduleTimerOverflow(unsigned timer_index);
    void SynchronizeTimersTo(Cycle target_cycle);
    Cycle NextTimerEventCycle() const noexcept;
    void ProcessTimerEvents();
    void ProcessTimerOverflow(unsigned timer_index);
    void IncrementCascadedTimer(unsigned timer_index);
    unsigned CpuTimingRegion(std::uint32_t address) const noexcept;
    Cycle CpuAccessCycles(std::uint32_t address, unsigned width,
                          bool sequential) const noexcept;
    Cycle CpuAccessCycles(std::uint32_t address, unsigned width,
                          bool sequential, unsigned region) const noexcept;
    void RecalculateNextEvent() noexcept;
    void UpdateWaitcnt() noexcept;
    void UpdateInterruptFlags() noexcept;
    bool PrefetchEnabled() const noexcept;
    void ClearPrefetch() noexcept;
    void StartPrefetchStream(std::uint32_t address, unsigned width,
                             unsigned region) noexcept;
    bool ConsumePrefetch(std::uint32_t address, unsigned width,
                         unsigned region) noexcept;
    void FillPrefetch(Cycle available_cycles) noexcept;
    void RecordCpuAccess(std::uint32_t address, unsigned width, bool write,
                         bool instruction_fetch) noexcept;
    Cycle NextPpuEventCycle() const noexcept;
    Cycle NextDmaEventCycle() const noexcept;
    void UpdateDisplayStatus();
    void RequestInterrupt(std::uint16_t mask);
    void UpdateKeyInput();
    void UpdateKeypadInterrupt();
    void RecordIwramWrite(std::uint32_t address, std::uint8_t value);
    void RecordWatchedMemoryWrite(std::uint32_t address,
                                  std::uint8_t value);

    struct DmaTransfer {
        bool armed{};
        bool active{};
        std::uint32_t initial_source{};
        std::uint32_t initial_destination{};
        std::uint32_t initial_count{};
        std::uint32_t source{};
        std::uint32_t destination{};
        std::uint32_t remaining{};
        std::uint16_t control{};
        Cycle next_cycle{};
        bool word{};
        unsigned source_control{};
        unsigned destination_control{};
    };

    struct Timer {
        bool enabled{};
        bool irq_enabled{};
        bool cascade{};
        std::uint16_t reload{};
        std::uint16_t counter{};
        std::uint32_t prescaler{1};
        std::uint32_t cycle_remainder{};
        Cycle last_cycle{};
        Cycle next_overflow{0};
    };

    const RomImage& rom_;
    std::vector<std::uint8_t> bios_;
    std::vector<std::uint8_t> ewram_;
    std::vector<std::uint8_t> iwram_;
    std::vector<std::uint8_t> io_;
    std::vector<std::uint8_t> palette_;
    std::vector<std::uint8_t> vram_;
    std::vector<std::uint8_t> oam_;
    std::vector<std::uint8_t> sram_;
    bool flash_{};
    bool flash_id_{};
    std::uint8_t flash_sequence_{};
    std::uint8_t flash_bank_{};
    std::uint8_t flash_command_{};
    Cycle cycles_{};
    Cycle frames_{};
    Cycle dma_transfers_{};
    Cycle dma_cycles_{};
    std::uint32_t scanline_cycles_{};
    std::uint16_t vcount_{};
    bool display_was_enabled_{};
    bool using_external_bios_{};
    std::uint16_t pressed_keys_{};
    PowerRequest power_request_{PowerRequest::None};
    std::array<DmaTransfer, 4> dma_{};
    std::array<Timer, 4> timers_{};
    bool cpu_instruction_active_{};
    bool cpu_last_access_valid_{};
    std::uint32_t cpu_last_access_address_{};
    std::uint8_t cpu_last_access_width_{};
    unsigned cpu_last_access_region_{};
    unsigned cpu_data_reads_{};
    Cycle cpu_access_penalty_{};
    std::uint32_t cpu_instruction_address_{};
    Cycle display_control_writes_{};
    std::uint32_t last_display_control_write_pc_{};
    std::uint16_t last_display_control_write_value_{};
    Cycle iwram_write_count_{};
    std::vector<IwramWriteRecord> iwram_write_records_;
    std::uint32_t iwram_watch_begin_{};
    std::uint32_t iwram_watch_end_{};
    std::vector<IwramWriteRecord> watched_iwram_write_records_;
    std::uint32_t memory_watch_begin_{};
    std::uint32_t memory_watch_end_{};
    std::vector<MemoryWriteRecord> watched_memory_write_records_;
    bool prefetch_valid_{};
    std::uint32_t prefetch_buffer_address_{};
    std::uint32_t prefetch_next_address_{};
    unsigned prefetch_count_{};
    unsigned prefetch_region_{};
    Cycle next_ppu_event_cycle_{kHblankStartCycle};
    Cycle next_event_cycle_{kHblankStartCycle};
    std::uint16_t waitcnt_{};
    bool prefetch_enabled_{};
    unsigned sram_wait_{4};
    unsigned ws_seq_[3]{2, 4, 8};
    unsigned ws_non_seq_[3]{4, 4, 4};
    bool interrupt_pending_{};
    bool interrupt_requested_{};
    Rtc rtc_{};
    bool sram_dirty_{false};
    Eeprom eeprom_{};
};

} // namespace ngba
