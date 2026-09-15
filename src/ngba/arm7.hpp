#pragma once

#include "ngba/bus.hpp"
#include "ngba/native.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ngba {

class CpuError : public std::runtime_error {
public:
    explicit CpuError(const std::string& message) : std::runtime_error(message) {}
};

struct CpuState {
    std::uint32_t r[16]{};
    std::uint32_t cpsr{};
};

enum class BiosMode : std::uint8_t { Hle, External, Hybrid };
BiosMode ParseBiosMode(const std::string& name);
const char* BiosModeName(BiosMode mode) noexcept;

struct TraceRecord {
    std::uint32_t pc{};
    std::uint32_t instruction{};
    bool thumb{};
    bool executed{true};
};

struct StepResult {
    TraceRecord trace{};
    Cycle cycles{};
    bool retired{};
};

enum class RunStopReason : std::uint8_t {
    CycleBudget,
    FrameReady,
    CpuStopped,
};

struct RunResult {
    Cycle elapsed_cycles{};
    std::uint64_t retired_instructions{};
    Cycle frames_published{};
    Cycle overshoot{};
    RunStopReason reason{RunStopReason::CycleBudget};
};

struct SoftwareInterruptRecord {
    Cycle cycle{};
    std::uint32_t pc{};
    std::uint32_t lr{};
    std::uint32_t stack_top{};
    std::uint8_t number{};
    bool thumb{};
    bool bios{};
    std::uint32_t r0{};
    std::uint32_t r1{};
    std::uint32_t r2{};
    std::uint32_t sp{};
    std::uint32_t sp_user_system{};
    std::uint32_t sp_irq{};
    std::uint32_t sp_svc{};
    std::uint32_t cpsr{};
};

class Arm7Tdmi {
public:
    static constexpr std::uint32_t kCpsrN = 1u << 31;
    static constexpr std::uint32_t kCpsrZ = 1u << 30;
    static constexpr std::uint32_t kCpsrC = 1u << 29;
    static constexpr std::uint32_t kCpsrV = 1u << 28;
    static constexpr std::uint32_t kCpsrI = 1u << 7;
    static constexpr std::uint32_t kCpsrF = 1u << 6;
    static constexpr std::uint32_t kCpsrT = 1u << 5;

    static constexpr std::uint32_t kModeUser = 0x10u;
    static constexpr std::uint32_t kModeFiq = 0x11u;
    static constexpr std::uint32_t kModeIrq = 0x12u;
    static constexpr std::uint32_t kModeSupervisor = 0x13u;
    static constexpr std::uint32_t kModeAbort = 0x17u;
    static constexpr std::uint32_t kModeUndefined = 0x1Bu;
    static constexpr std::uint32_t kModeSystem = 0x1Fu;

    explicit Arm7Tdmi(MemoryBus& bus);

    void Reset(std::uint32_t pc);
    /// Reset into the ARM7TDMI supervisor entry point at BIOS address zero.
    /// This is an opt-in diagnostic path; the normal cartridge path remains
    /// Reset(rom.ResetVectorAddress()).
    void ResetBios();
    TraceRecord Step();

    const CpuState& State() const noexcept;
    CpuState& State() noexcept;
    std::uint64_t Steps() const noexcept;
    Cycle CpuExecutionCycles() const noexcept;
    Cycle HaltCycles() const noexcept;
    std::uint64_t HaltEntries() const noexcept;
    std::uint64_t HaltWakeups() const noexcept;
    std::uint64_t BiosSoftwareInterrupts() const noexcept;
    void SetBiosMode(BiosMode mode);
    BiosMode GetBiosMode() const noexcept;
    const std::vector<SoftwareInterruptRecord>&
        SoftwareInterruptTrace() const noexcept;
    bool IsThumb() const noexcept;
    bool IsHalted() const noexcept;

    /// Run against an absolute master-clock deadline using the provisional
    /// instruction-atomic timing model. Halt advances directly to the next event
    /// or the deadline; it does not retire synthetic instructions.
    RunResult RunForCycles(Cycle cycles);
    /// Run until the PPU has published the requested absolute frame number.
    RunResult RunUntilFrameReady(Cycle target_frame);
    /// Execute one instruction, or advance through Halt until the next
    /// event. The returned cycle count is master-clock time consumed by this
    /// step; halted steps report retired=false.
    StepResult StepTimed();
    void EnableNative(bool enabled);
    std::uint64_t TranslatedInstructions() const noexcept;
    std::size_t NativeBlocks() const noexcept;

private:
    friend class StateCodec;
    struct Bank {
        std::uint32_t r8[5]{};
        std::uint32_t r13{};
        std::uint32_t r14{};
        std::uint32_t spsr{};
    };

    void ExecuteArm(std::uint32_t instruction, std::uint32_t pc);
    void ExecuteThumb(std::uint16_t instruction, std::uint32_t pc);
    TraceRecord StepHalted(Cycle deadline);
    TraceRecord StepDma(Cycle deadline);
    StepResult StepInternal(Cycle deadline);
    void StepFast();
    void CompleteInstruction(Cycle cycles);
    void ApplyPowerRequest();
    void EnterSoftwareInterrupt(unsigned number, std::uint32_t pc,
                                bool thumb);
    void RecordSoftwareInterrupt(unsigned number, std::uint32_t pc,
                                 bool thumb, bool bios);
    void EnterIrq();
    void RestoreSavedStatus();
    bool ShouldExecuteBiosSwi(unsigned number) const noexcept;
    std::uint32_t StackPointerForMode(std::uint32_t mode) const noexcept;

    bool ConditionPassed(unsigned condition) const noexcept;
    std::uint32_t ReadArmRegister(unsigned index, std::uint32_t pc) const;
    std::uint32_t ReadThumbRegister(unsigned index, std::uint32_t pc) const;
    void WriteRegister(unsigned index, std::uint32_t value,
                       bool thumb_pc = false);

    std::uint32_t DecodeArmOperand2(std::uint32_t instruction,
                                    std::uint32_t pc, bool& carry_out) const;
    std::uint32_t Shift(std::uint32_t value, unsigned type, unsigned amount,
                        bool& carry_out, bool amount_is_register) const;

    void SetCpsr(std::uint32_t value);
    void SaveBank(std::uint32_t mode);
    void LoadBank(std::uint32_t mode);
    Bank* BankFor(std::uint32_t mode);
    const Bank* BankFor(std::uint32_t mode) const;

    bool Flag(std::uint32_t flag) const noexcept;
    void SetFlag(std::uint32_t flag, bool value) noexcept;
    void SetNz(std::uint32_t value) noexcept;
    std::uint32_t AddWithCarry(std::uint32_t left, std::uint32_t right,
                               bool carry, bool set_flags);
    std::uint32_t SubWithBorrow(std::uint32_t left, std::uint32_t right,
                                bool borrow, bool set_flags);

    void Unsupported(std::uint32_t pc, std::uint32_t instruction,
                     bool thumb) const;
    void HandleSoftwareInterrupt(unsigned number, std::uint32_t pc,
                                 bool thumb);
    Cycle InstructionCycles(std::uint32_t instruction,
                            std::uint32_t pc,
                            bool thumb,
                            std::uint32_t before_cpsr,
                            std::uint32_t arm_rs_val = 0) const noexcept;

    MemoryBus& bus_;
    CpuState state_{};
    Bank fiq_bank_{};
    Bank irq_bank_{};
    Bank supervisor_bank_{};
    Bank abort_bank_{};
    Bank undefined_bank_{};
    std::uint32_t user_r8_12_[5]{};
    std::uint32_t user_sp_{};
    std::uint32_t user_lr_{};
    std::uint64_t steps_{};
    Cycle cpu_execution_cycles_{};
    Cycle halt_cycles_{};
    std::uint64_t halt_entries_{};
    std::uint64_t halt_wakeups_{};
    std::uint64_t bios_swi_entries_{};
    std::vector<SoftwareInterruptRecord> software_interrupt_trace_;
    BiosMode bios_mode_{BiosMode::Hle};
    bool halted_{};
    bool stopped_{};
    std::shared_ptr<NativeBackend> native_backend_;
    std::uint64_t translated_instructions_{};
};

} // namespace ngba
