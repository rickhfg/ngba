#include "ngba/arm7.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ngba {
namespace {

constexpr std::uint32_t kModeMask = 0x1Fu;
constexpr std::uint32_t kSwiVectorAddress = 0x00000008u;
constexpr std::uint32_t kIrqVectorAddress = 0x00000018u;
constexpr std::size_t kSoftwareInterruptDiagnosticLimit = 512u;

std::uint32_t RotateRight(std::uint32_t value, unsigned amount) {
    amount &= 31u;
    if (amount == 0) {
        return value;
    }
    return (value >> amount) | (value << (32u - amount));
}

std::int32_t SignExtend(std::uint32_t value, unsigned bits) {
    const std::uint32_t sign = 1u << (bits - 1u);
    if ((value & sign) != 0) {
        value |= ~((1u << bits) - 1u);
    }
    return static_cast<std::int32_t>(value);
}

bool ConditionPassedFor(std::uint32_t cpsr, unsigned condition) {
    const bool n = (cpsr & Arm7Tdmi::kCpsrN) != 0;
    const bool z = (cpsr & Arm7Tdmi::kCpsrZ) != 0;
    const bool c = (cpsr & Arm7Tdmi::kCpsrC) != 0;
    const bool v = (cpsr & Arm7Tdmi::kCpsrV) != 0;
    switch (condition & 0xFu) {
    case 0x0: return z;                 // EQ
    case 0x1: return !z;                // NE
    case 0x2: return c;                 // CS/HS
    case 0x3: return !c;                // CC/LO
    case 0x4: return n;                 // MI
    case 0x5: return !n;                // PL
    case 0x6: return v;                 // VS
    case 0x7: return !v;                // VC
    case 0x8: return c && !z;            // HI
    case 0x9: return !c || z;            // LS
    case 0xA: return n == v;             // GE
    case 0xB: return n != v;             // LT
    case 0xC: return !z && (n == v);     // GT
    case 0xD: return z || (n != v);      // LE
    case 0xE: return true;               // AL
    default:  return false;              // NV
    }
}

unsigned CountBits(std::uint32_t value) {
    unsigned count = 0;
    while (value != 0) {
        value &= value - 1u;
        ++count;
    }
    return count;
}

Cycle MultiplyCycles(std::uint32_t value, bool accumulate) {
    const bool byte = (value & 0xFFFFFF00u) == 0 ||
                      (value & 0xFFFFFF00u) == 0xFFFFFF00u;
    const bool halfword = (value & 0xFFFF0000u) == 0 ||
                          (value & 0xFFFF0000u) == 0xFFFF0000u;
    const bool word = (value & 0xFF000000u) == 0 ||
                      (value & 0xFF000000u) == 0xFF000000u;
    const Cycle extra = byte ? 1u : (halfword ? 2u : (word ? 3u : 4u));
    return 1u + extra + (accumulate ? 1u : 0u);
}

} // namespace

Arm7Tdmi::Arm7Tdmi(MemoryBus& bus) : bus_(bus) {
    bios_mode_ = bus.UsingExternalBios() ? BiosMode::External : BiosMode::Hle;
    software_interrupt_trace_.reserve(kSoftwareInterruptDiagnosticLimit);
    Reset(0);
}

void Arm7Tdmi::Reset(std::uint32_t pc) {
    state_ = CpuState{};
    fiq_bank_ = Bank{};
    irq_bank_ = Bank{};
    supervisor_bank_ = Bank{};
    abort_bank_ = Bank{};
    undefined_bank_ = Bank{};
    user_r8_12_[0] = user_r8_12_[1] = user_r8_12_[2] = 0;
    user_r8_12_[3] = user_r8_12_[4] = 0;
    user_sp_ = 0;
    user_lr_ = 0;
    steps_ = 0;
    cpu_execution_cycles_ = 0;
    halt_cycles_ = 0;
    halt_entries_ = 0;
    halt_wakeups_ = 0;
    bios_swi_entries_ = 0;
    translated_instructions_ = 0;
    software_interrupt_trace_.clear();
    halted_ = false;
    stopped_ = false;
    bus_.ResetCpuTiming();
    bus_.ClearDiagnostics();

    // The BIOS normally enters a cartridge in ARM/system mode after setting
    // up its own stacks. NGBA starts at the cartridge entry, so seed the
    // conventional banks needed by native BIOS exceptions as well.
    user_sp_ = 0x03007F00u;
    irq_bank_.r13 = 0x03007FA0u;
    supervisor_bank_.r13 = 0x03007FE0u;
    state_.cpsr = kModeSystem;
    state_.r[13] = user_sp_;
    state_.r[15] = pc;
}

void Arm7Tdmi::ResetBios() {
    Reset(0);

    // On reset the ARM7TDMI starts in ARM/SVC with both exception masks set.
    // Keep the conventional banks seeded by Reset() so a BIOS dump that
    // samples them before installing its own stacks remains deterministic;
    // the BIOS is still responsible for establishing its real post-boot
    // values.
    SetCpsr(kCpsrF | kCpsrI | kModeSupervisor);
    state_.r[15] = 0;
}

TraceRecord Arm7Tdmi::Step() {
    return StepInternal(std::numeric_limits<Cycle>::max()).trace;
}

StepResult Arm7Tdmi::StepTimed() {
    return StepInternal(std::numeric_limits<Cycle>::max());
}

TraceRecord Arm7Tdmi::StepDma(Cycle deadline) {
    const std::uint32_t pc = state_.r[15];
    const bool thumb = IsThumb();
    Cycle target = bus_.NextEventCycle();
    if (deadline != std::numeric_limits<Cycle>::max() &&
        target > deadline) {
        target = deadline;
    }
    if (target >= bus_.Cycles()) bus_.AdvanceTo(target);

    // A request may have become eligible while DMA owned the bus. It is
    // considered at this instruction boundary, never in the middle of a
    // transfer.
    if (!bus_.DmaActive() && (state_.cpsr & kCpsrI) == 0 &&
        bus_.InterruptPending()) {
        EnterIrq();
    }
    return TraceRecord{pc, 0, thumb, false};
}

void Arm7Tdmi::StepFast() {
    const std::uint32_t pc = state_.r[15];
    const bool thumb = IsThumb();
    const std::uint32_t before_cpsr = state_.cpsr;
    bus_.BeginCpuInstruction(pc);
    if (thumb) {
        const std::uint16_t instruction = bus_.CpuFetch16(pc);
        state_.r[15] += 2;
        if (native_backend_ && native_backend_->Execute(state_, instruction, true)) {
            ++translated_instructions_;
        } else {
            ExecuteThumb(instruction, pc);
        }
        ++steps_;
        const Cycle access_penalty = bus_.EndCpuInstruction();
        CompleteInstruction(InstructionCycles(instruction, pc, true, before_cpsr, 0) +
                            access_penalty);
        return;
    }

    const std::uint32_t instruction = bus_.CpuFetch32(pc);
    state_.r[15] += 4;
    const unsigned arm_rs = (instruction >> 8) & 0xFu;
    const std::uint32_t arm_rs_val = arm_rs == 15u ? pc + 8u : state_.r[arm_rs];
    if (native_backend_ && native_backend_->Execute(state_, instruction, false)) {
        ++translated_instructions_;
    } else {
        ExecuteArm(instruction, pc);
    }
    ++steps_;
    const Cycle access_penalty = bus_.EndCpuInstruction();
    CompleteInstruction(InstructionCycles(instruction, pc, false, before_cpsr, arm_rs_val) +
                        access_penalty);
}

StepResult Arm7Tdmi::StepInternal(Cycle deadline) {
    const Cycle start_cycle = bus_.Cycles();
    if (stopped_) {
        throw CpuError("CPU is in unimplemented GBA Stop state");
    }
    if (halted_) {
        const TraceRecord trace = StepHalted(deadline);
        return StepResult{trace, bus_.Cycles() - start_cycle, false};
    }

    if (bus_.DmaActive()) {
        const TraceRecord trace = StepDma(deadline);
        return StepResult{trace, bus_.Cycles() - start_cycle, false};
    }

    const std::uint32_t pc = state_.r[15];
    const bool thumb = IsThumb();
    const std::uint32_t instruction = thumb ? bus_.Read16(pc) : bus_.Read32(pc);
    StepFast();
    return StepResult{TraceRecord{pc, instruction, thumb, true},
                      bus_.Cycles() - start_cycle, true};
}

void Arm7Tdmi::EnableNative(bool enabled) {
    if (enabled && !NativeBackend::Available()) throw CpuError("native backend requires x86 Windows");
    if (enabled && !native_backend_) native_backend_ = std::make_shared<NativeBackend>();
    if (!enabled) native_backend_.reset();
}

std::uint64_t Arm7Tdmi::TranslatedInstructions() const noexcept { return translated_instructions_; }
std::size_t Arm7Tdmi::NativeBlocks() const noexcept { return native_backend_ ? native_backend_->CachedBlocks() : 0; }

const CpuState& Arm7Tdmi::State() const noexcept {
    return state_;
}

CpuState& Arm7Tdmi::State() noexcept {
    return state_;
}

std::uint64_t Arm7Tdmi::Steps() const noexcept {
    return steps_;
}

std::uint64_t Arm7Tdmi::HaltEntries() const noexcept {
    return halt_entries_;
}

std::uint64_t Arm7Tdmi::HaltWakeups() const noexcept {
    return halt_wakeups_;
}

std::uint64_t Arm7Tdmi::BiosSoftwareInterrupts() const noexcept {
    return bios_swi_entries_;
}

BiosMode ParseBiosMode(const std::string& name) {
    if (name == "hle") return BiosMode::Hle;
    if (name == "external") return BiosMode::External;
    if (name == "hybrid") return BiosMode::Hybrid;
    throw CpuError("BIOS mode must be external, hle, or hybrid");
}

const char* BiosModeName(BiosMode mode) noexcept {
    switch (mode) {
    case BiosMode::Hle: return "hle";
    case BiosMode::External: return "external";
    case BiosMode::Hybrid: return "hybrid";
    }
    return "invalid";
}

void Arm7Tdmi::SetBiosMode(BiosMode mode) {
    if (mode != BiosMode::Hle && mode != BiosMode::External && mode != BiosMode::Hybrid) {
        throw CpuError("invalid BIOS mode");
    }
    if (mode != BiosMode::Hle && !bus_.UsingExternalBios()) {
        throw CpuError("external and hybrid BIOS modes require a BIOS image");
    }
    bios_mode_ = mode;
}

BiosMode Arm7Tdmi::GetBiosMode() const noexcept {
    return bios_mode_;
}

const std::vector<SoftwareInterruptRecord>&
Arm7Tdmi::SoftwareInterruptTrace() const noexcept {
    return software_interrupt_trace_;
}

bool Arm7Tdmi::IsThumb() const noexcept {
    return (state_.cpsr & kCpsrT) != 0;
}

bool Arm7Tdmi::IsHalted() const noexcept {
    return halted_;
}

Cycle Arm7Tdmi::CpuExecutionCycles() const noexcept {
    return cpu_execution_cycles_;
}

Cycle Arm7Tdmi::HaltCycles() const noexcept {
    return halt_cycles_;
}

RunResult Arm7Tdmi::RunForCycles(Cycle cycles) {
    const Cycle start_cycle = bus_.Cycles();
    const std::uint64_t start_steps = steps_;
    const Cycle start_frames = bus_.Frames();
    const Cycle deadline = start_cycle + cycles;
    if (deadline < start_cycle) {
        throw std::invalid_argument("cycle budget overflows the master clock");
    }

    while (bus_.Cycles() < deadline) {
        if (stopped_) break;
        StepInternal(deadline);
    }

    RunResult result;
    result.elapsed_cycles = bus_.Cycles() - start_cycle;
    result.retired_instructions = steps_ - start_steps;
    result.frames_published = bus_.Frames() - start_frames;
    result.overshoot = bus_.Cycles() > deadline
        ? bus_.Cycles() - deadline
        : 0;
    result.reason = stopped_ && bus_.Cycles() < deadline
        ? RunStopReason::CpuStopped
        : RunStopReason::CycleBudget;
    return result;
}

RunResult Arm7Tdmi::RunUntilFrameReady(Cycle target_frame) {
    const Cycle start_cycle = bus_.Cycles();
    const std::uint64_t start_steps = steps_;
    const Cycle start_frames = bus_.Frames();

    while (bus_.Frames() < target_frame) {
        if (stopped_) break;
        if (halted_) {
            StepHalted(std::numeric_limits<Cycle>::max());
            continue;
        }
        if (bus_.DmaActive()) {
            StepDma(std::numeric_limits<Cycle>::max());
            continue;
        }
        StepFast();
    }

    RunResult result;
    result.elapsed_cycles = bus_.Cycles() - start_cycle;
    result.retired_instructions = steps_ - start_steps;
    result.frames_published = bus_.Frames() - start_frames;
    result.reason = bus_.Frames() >= target_frame
        ? RunStopReason::FrameReady
        : RunStopReason::CpuStopped;
    return result;
}

TraceRecord Arm7Tdmi::StepHalted(Cycle deadline) {
    const std::uint32_t pc = state_.r[15];
    const bool thumb = IsThumb();
    const Cycle start_cycle = bus_.Cycles();

    // A halted CPU retires no instruction, but the hardware clock and its
    // event sources continue to advance. Jump directly to the next event (or
    // a caller-provided deadline) instead of manufacturing idle instructions.
    if (!bus_.InterruptRequested()) {
        Cycle target = bus_.NextEventCycle();
        if (deadline != std::numeric_limits<Cycle>::max() && target > deadline) {
            target = deadline;
        }
        if (target >= bus_.Cycles()) bus_.AdvanceTo(target);
    }
    if (bus_.InterruptRequested()) {
        halted_ = false;
        ++halt_wakeups_;
    }
    if (!halted_ && !bus_.DmaActive() && (state_.cpsr & kCpsrI) == 0 &&
        bus_.InterruptPending()) {
        EnterIrq();
    }
    halt_cycles_ += bus_.Cycles() - start_cycle;
    return TraceRecord{pc, 0, thumb, false};
}

void Arm7Tdmi::CompleteInstruction(Cycle cycles) {
    const Cycle start_cycle = bus_.Cycles();
    bus_.AdvanceTo(bus_.Cycles() + cycles);
    cpu_execution_cycles_ += bus_.Cycles() - start_cycle;
    ApplyPowerRequest();

    if (halted_ && bus_.InterruptRequested()) {
        halted_ = false;
        ++halt_wakeups_;
    }
    if (!halted_ && !stopped_ && !bus_.DmaActive() &&
        (state_.cpsr & kCpsrI) == 0 &&
        bus_.InterruptPending()) {
        EnterIrq();
    }
}

void Arm7Tdmi::ApplyPowerRequest() {
    switch (bus_.ConsumePowerRequest()) {
    case PowerRequest::Halt:
        halted_ = true;
        ++halt_entries_;
        return;
    case PowerRequest::Stop:
        stopped_ = true;
        return;
    case PowerRequest::None:
        return;
    }
}

void Arm7Tdmi::EnterSoftwareInterrupt(unsigned number, std::uint32_t pc,
                                       bool thumb) {
    RecordSoftwareInterrupt(number, pc, thumb, true);
    const std::uint32_t saved_cpsr = state_.cpsr;
    supervisor_bank_.spsr = saved_cpsr;
    const std::uint32_t return_pc = pc + (thumb ? 2u : 4u);
    const std::uint32_t svc_cpsr =
        (saved_cpsr & ~(kCpsrT | kModeMask)) |
        kCpsrI | kModeSupervisor;
    SetCpsr(svc_cpsr);
    state_.r[14] = return_pc;
    state_.r[15] = kSwiVectorAddress;
    ++bios_swi_entries_;
    (void)number;
}

void Arm7Tdmi::RecordSoftwareInterrupt(unsigned number, std::uint32_t pc,
                                       bool thumb, bool bios) {
    if (software_interrupt_trace_.size() >=
        kSoftwareInterruptDiagnosticLimit) {
        return;
    }

    software_interrupt_trace_.push_back(SoftwareInterruptRecord{
        bus_.Cycles(),
        pc,
        state_.r[14],
        bus_.Read32(state_.r[13]),
        static_cast<std::uint8_t>(number & 0xFFu),
        thumb,
        bios,
        state_.r[0],
        state_.r[1],
        state_.r[2],
        state_.r[13],
        StackPointerForMode(kModeSystem),
        StackPointerForMode(kModeIrq),
        StackPointerForMode(kModeSupervisor),
        state_.cpsr});
}

bool Arm7Tdmi::ShouldExecuteBiosSwi(
    unsigned number) const noexcept {
    if (bios_mode_ == BiosMode::External) return true;
    if (bios_mode_ == BiosMode::Hle) return false;
    const unsigned id = number & 0xFFu;
    return id == 0x02u || id == 0x04u || id == 0x05u;
}

std::uint32_t Arm7Tdmi::StackPointerForMode(std::uint32_t mode) const noexcept {
    const std::uint32_t current = state_.cpsr & kModeMask;
    const std::uint32_t requested = mode & kModeMask;
    if ((requested == kModeUser || requested == kModeSystem) &&
        (current == kModeUser || current == kModeSystem)) {
        return state_.r[13];
    }
    if (requested == kModeUser || requested == kModeSystem) return user_sp_;
    if (requested == kModeIrq) {
        return current == kModeIrq ? state_.r[13] : irq_bank_.r13;
    }
    if (requested == kModeSupervisor) {
        return current == kModeSupervisor ? state_.r[13]
                                          : supervisor_bank_.r13;
    }
    if (const Bank* bank = BankFor(requested)) return bank->r13;
    return 0;
}

void Arm7Tdmi::EnterIrq() {
    const std::uint32_t saved_cpsr = state_.cpsr;
    const std::uint32_t return_pc = state_.r[15];
    irq_bank_.spsr = saved_cpsr;
    const std::uint32_t irq_cpsr =
        (saved_cpsr & ~(kCpsrT | kCpsrI | kModeMask)) |
        kCpsrI | kModeIrq;
    SetCpsr(irq_cpsr);
    state_.r[14] = return_pc + 4u;
    // Hardware vectors through BIOS 0x18.  The BIOS dispatcher then loads
    // the user ARM handler from 0x03007FFC and finishes with
    // SUBS PC,LR,#4, which restores SPSR_irq.  Jumping directly to the game
    // pointer would make valid GBA handlers ending in BX LR return in IRQ
    // mode with the wrong T bit.
    state_.r[15] = kIrqVectorAddress;
}

void Arm7Tdmi::RestoreSavedStatus() {
    const Bank* bank = BankFor(state_.cpsr & kModeMask);
    if (bank == nullptr) {
        return;
    }
    SetCpsr(bank->spsr);
}

bool Arm7Tdmi::Flag(std::uint32_t flag) const noexcept {
    return (state_.cpsr & flag) != 0;
}

void Arm7Tdmi::SetFlag(std::uint32_t flag, bool value) noexcept {
    if (value) {
        state_.cpsr |= flag;
    } else {
        state_.cpsr &= ~flag;
    }
}

void Arm7Tdmi::SetNz(std::uint32_t value) noexcept {
    SetFlag(kCpsrN, (value & 0x80000000u) != 0);
    SetFlag(kCpsrZ, value == 0);
}

std::uint32_t Arm7Tdmi::AddWithCarry(std::uint32_t left, std::uint32_t right,
                                     bool carry, bool set_flags) {
    const std::uint64_t wide = static_cast<std::uint64_t>(left) + right +
                               (carry ? 1u : 0u);
    const std::uint32_t result = static_cast<std::uint32_t>(wide);
    if (set_flags) {
        SetNz(result);
        SetFlag(kCpsrC, (wide >> 32) != 0);
        SetFlag(kCpsrV, ((~(left ^ right) & (left ^ result)) & 0x80000000u) != 0);
    }
    return result;
}

std::uint32_t Arm7Tdmi::SubWithBorrow(std::uint32_t left, std::uint32_t right,
                                      bool borrow, bool set_flags) {
    const std::uint64_t subtrahend = static_cast<std::uint64_t>(right) +
                                     (borrow ? 1u : 0u);
    const std::uint32_t result = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(left) - subtrahend);
    if (set_flags) {
        SetNz(result);
        SetFlag(kCpsrC, static_cast<std::uint64_t>(left) >= subtrahend);
        SetFlag(kCpsrV, (((left ^ right) & (left ^ result)) & 0x80000000u) != 0);
    }
    return result;
}

bool Arm7Tdmi::ConditionPassed(unsigned condition) const noexcept {
    const bool n = Flag(kCpsrN);
    const bool z = Flag(kCpsrZ);
    const bool c = Flag(kCpsrC);
    const bool v = Flag(kCpsrV);
    switch (condition & 0xFu) {
    case 0x0: return z;                 // EQ
    case 0x1: return !z;                // NE
    case 0x2: return c;                 // CS/HS
    case 0x3: return !c;                // CC/LO
    case 0x4: return n;                 // MI
    case 0x5: return !n;                // PL
    case 0x6: return v;                 // VS
    case 0x7: return !v;                // VC
    case 0x8: return c && !z;            // HI
    case 0x9: return !c || z;            // LS
    case 0xA: return n == v;             // GE
    case 0xB: return n != v;             // LT
    case 0xC: return !z && (n == v);     // GT
    case 0xD: return z || (n != v);      // LE
    case 0xE: return true;               // AL
    default:  return false;              // NV
    }
}

Cycle Arm7Tdmi::InstructionCycles(std::uint32_t instruction,
                                   std::uint32_t pc,
                                   bool thumb,
                                   std::uint32_t before_cpsr,
                                   std::uint32_t arm_rs_val) const noexcept {
    (void)pc;
    // This is the first CPU-cost model, deliberately kept at instruction
    // granularity. It models pipeline refill and the number of data accesses;
    // the bus adds the first WAITCNT/S-N wait-state penalties separately.
    // The bus also has a first prefetch-buffer model; VRAM/OAM contention and
    // exact pipeline overlap remain future refinements.
    if (!thumb) {
        if (!ConditionPassedFor(before_cpsr, instruction >> 28)) return 1u;

        if ((instruction & 0x0FFFFFF0u) == 0x012FFF10u) {
            return 3u; // BX: branch plus pipeline refill.
        }
        if ((instruction & 0x0E000000u) == 0x0A000000u) {
            return 3u; // B/BL.
        }
        if ((instruction & 0x0F000000u) == 0x0F000000u) {
            return 3u; // SWI exception entry.
        }
        if ((instruction & 0x0FB00FF0u) == 0x01000090u) {
            return 4u; // SWP: read, write and instruction overhead.
        }
        if ((instruction & 0x0FC000F0u) == 0x00000090u) {
            return MultiplyCycles(arm_rs_val, (instruction & (1u << 21)) != 0);
        }
        if ((instruction & 0x0E000090u) == 0x00000090u) {
            return (instruction & (1u << 20)) != 0 ? 3u : 2u;
        }
        if ((instruction & 0x0C000000u) == 0x04000000u) {
            return (instruction & (1u << 20)) != 0 ? 3u : 2u;
        }
        if ((instruction & 0x0E000000u) == 0x08000000u) {
            unsigned count = CountBits(instruction & 0xFFFFu);
            if (count == 0) count = 16u;
            Cycle cycles = count +
                ((instruction & (1u << 20)) != 0 ? 2u : 1u);
            if ((instruction & (1u << 20)) != 0 &&
                (instruction & (1u << 15)) != 0) {
                ++cycles; // loading PC also refills the pipeline.
            }
            return cycles;
        }
        if ((instruction & 0x0C000000u) == 0x00000000u) {
            const unsigned opcode = (instruction >> 21) & 0xFu;
            const unsigned rd = (instruction >> 12) & 0xFu;
            if (rd == 15u && opcode != 8u && opcode != 9u &&
                opcode != 10u && opcode != 11u) {
                return 3u;
            }
        }
        return 1u;
    }

    const std::uint16_t thumb_instruction =
        static_cast<std::uint16_t>(instruction);
    if ((thumb_instruction & 0xF000u) == 0xD000u) {
        const unsigned condition = (thumb_instruction >> 8) & 0xFu;
        if (condition == 0xFu) return 3u; // SWI.
        return ConditionPassedFor(before_cpsr, condition) ? 3u : 1u;
    }
    if ((thumb_instruction & 0xF800u) == 0xE000u) {
        return 3u; // Unconditional branch.
    }
    if ((thumb_instruction & 0xF800u) == 0xF000u) {
        return 1u; // First half of BL prepares the high offset.
    }
    if ((thumb_instruction & 0xF800u) == 0xF800u) {
        return 3u; // Second half of BL branches and refills.
    }
    if ((thumb_instruction & 0xFC00u) == 0x4400u) {
        const unsigned op = (thumb_instruction >> 8) & 3u;
        const unsigned rd = (thumb_instruction & 7u) |
                            (((thumb_instruction >> 7) & 1u) << 3);
        return op == 3u || (rd == 15u && (op == 0u || op == 2u))
            ? 3u
            : 1u;
    }
    if ((thumb_instruction & 0xFC00u) == 0x4000u) {
        const unsigned op = (thumb_instruction >> 6) & 0xFu;
        if (op == 0xDu) {
            const unsigned rs = (thumb_instruction >> 3) & 7u;
            return MultiplyCycles(state_.r[rs], false);
        }
        return 1u;
    }
    if ((thumb_instruction & 0xF800u) == 0x4800u) {
        return 3u; // PC-relative literal load.
    }
    if ((thumb_instruction & 0xF000u) == 0x5000u) {
        const unsigned op = (thumb_instruction >> 9) & 7u;
        return op >= 3u ? 3u : 2u;
    }
    if ((thumb_instruction & 0xE000u) == 0x6000u) {
        const unsigned op = (thumb_instruction >> 11) & 3u;
        return (op & 1u) != 0 ? 3u : 2u;
    }
    if ((thumb_instruction & 0xF000u) == 0x8000u) {
        return (thumb_instruction & (1u << 11)) != 0 ? 3u : 2u;
    }
    if ((thumb_instruction & 0xF000u) == 0x9000u) {
        return (thumb_instruction & (1u << 11)) != 0 ? 3u : 2u;
    }
    if ((thumb_instruction & 0xFE00u) == 0xB400u ||
        (thumb_instruction & 0xFE00u) == 0xBC00u) {
        const bool pop = (thumb_instruction & 0x0800u) != 0;
        const bool extra = (thumb_instruction & 0x0100u) != 0;
        unsigned count = CountBits(thumb_instruction & 0x00FFu);
        if (extra) ++count;
        return pop && extra ? count + 2u : count + 1u;
    }
    if ((thumb_instruction & 0xFF00u) == 0xB000u) {
        return 1u;
    }
    if ((thumb_instruction & 0xF000u) == 0xC000u) {
        unsigned count = CountBits(thumb_instruction & 0x00FFu);
        if (count == 0) count = 8u;
        return (thumb_instruction & (1u << 11)) != 0
            ? count + 2u
            : count + 1u;
    }
    return 1u;
}

std::uint32_t Arm7Tdmi::ReadArmRegister(unsigned index, std::uint32_t pc) const {
    return index == 15 ? pc + 8 : state_.r[index & 15u];
}

std::uint32_t Arm7Tdmi::ReadThumbRegister(unsigned index, std::uint32_t pc) const {
    return index == 15 ? ((pc + 4) & ~3u) : state_.r[index & 15u];
}

void Arm7Tdmi::WriteRegister(unsigned index, std::uint32_t value, bool thumb_pc) {
    if ((index & 15u) != 15u) {
        state_.r[index & 15u] = value;
        return;
    }
    // Exception returns restore CPSR after writing PC. The caller supplies
    // the restored T bit so an interrupted Thumb halfword is not rounded
    // down as if the return were still in ARM state.
    state_.r[15] = thumb_pc ? (value & ~1u) : (value & ~3u);
}

std::uint32_t Arm7Tdmi::Shift(std::uint32_t value, unsigned type,
                              unsigned amount, bool& carry_out,
                              bool amount_is_register) const {
    const bool old_carry = Flag(kCpsrC);
    if (amount_is_register) {
        amount &= 0xFFu;
        if (amount == 0) {
            carry_out = old_carry;
            return value;
        }
    }

    switch (type & 3u) {
    case 0: // LSL
        if (amount == 0) {
            carry_out = old_carry;
        } else if (amount < 32) {
            carry_out = ((value >> (32 - amount)) & 1u) != 0;
        } else if (amount == 32) {
            carry_out = (value & 1u) != 0;
            return 0;
        } else {
            carry_out = false;
            return 0;
        }
        return amount == 0 ? value : value << amount;
    case 1: // LSR
        if (amount == 0 && !amount_is_register) {
            amount = 32;
        }
        if (amount < 32) {
            carry_out = ((value >> (amount - 1)) & 1u) != 0;
            return value >> amount;
        }
        carry_out = amount == 32 ? ((value >> 31) & 1u) != 0 : false;
        return 0;
    case 2: // ASR
        if (amount == 0 && !amount_is_register) {
            amount = 32;
        }
        if (amount < 32) {
            carry_out = ((value >> (amount - 1)) & 1u) != 0;
            return static_cast<std::uint32_t>(
                static_cast<std::int32_t>(value) >> amount);
        }
        carry_out = (value & 0x80000000u) != 0;
        return carry_out ? 0xFFFFFFFFu : 0;
    default: // ROR / RRX
        if (amount == 0 && !amount_is_register) {
            carry_out = (value & 1u) != 0;
            return (old_carry ? 0x80000000u : 0u) | (value >> 1);
        }
        amount &= 31u;
        if (amount == 0) {
            carry_out = ((value >> 31) & 1u) != 0;
            return value;
        }
        carry_out = ((value >> (amount - 1)) & 1u) != 0;
        return RotateRight(value, amount);
    }
}

std::uint32_t Arm7Tdmi::DecodeArmOperand2(std::uint32_t instruction,
                                          std::uint32_t pc,
                                          bool& carry_out) const {
    if ((instruction & (1u << 25)) != 0) {
        const unsigned rotate = ((instruction >> 8) & 0xFu) * 2u;
        const std::uint32_t immediate = instruction & 0xFFu;
        const std::uint32_t value = RotateRight(immediate, rotate);
        carry_out = rotate == 0 ? Flag(kCpsrC) : ((value >> 31) & 1u) != 0;
        return value;
    }

    const unsigned rm = instruction & 0xFu;
    const std::uint32_t value = ReadArmRegister(rm, pc);
    const unsigned type = (instruction >> 5) & 3u;
    if ((instruction & (1u << 4)) != 0) {
        const unsigned rs = (instruction >> 8) & 0xFu;
        return Shift(value, type, ReadArmRegister(rs, pc) & 0xFFu,
                     carry_out, true);
    }
    return Shift(value, type, (instruction >> 7) & 0x1Fu, carry_out, false);
}

Arm7Tdmi::Bank* Arm7Tdmi::BankFor(std::uint32_t mode) {
    switch (mode & kModeMask) {
    case kModeFiq: return &fiq_bank_;
    case kModeIrq: return &irq_bank_;
    case kModeSupervisor: return &supervisor_bank_;
    case kModeAbort: return &abort_bank_;
    case kModeUndefined: return &undefined_bank_;
    default: return nullptr;
    }
}

const Arm7Tdmi::Bank* Arm7Tdmi::BankFor(std::uint32_t mode) const {
    return const_cast<Arm7Tdmi*>(this)->BankFor(mode);
}

void Arm7Tdmi::SaveBank(std::uint32_t mode) {
    if ((mode & kModeMask) == kModeFiq) {
        for (unsigned i = 0; i < 5; ++i) {
            fiq_bank_.r8[i] = state_.r[8 + i];
        }
    }

    if ((mode & kModeMask) == kModeUser ||
        (mode & kModeMask) == kModeSystem) {
        user_sp_ = state_.r[13];
        user_lr_ = state_.r[14];
    } else if (Bank* bank = BankFor(mode)) {
        bank->r13 = state_.r[13];
        bank->r14 = state_.r[14];
    }
}

void Arm7Tdmi::LoadBank(std::uint32_t mode) {
    if ((mode & kModeMask) == kModeFiq) {
        for (unsigned i = 0; i < 5; ++i) {
            state_.r[8 + i] = fiq_bank_.r8[i];
        }
    }

    if ((mode & kModeMask) == kModeUser ||
        (mode & kModeMask) == kModeSystem) {
        state_.r[13] = user_sp_;
        state_.r[14] = user_lr_;
    } else if (const Bank* bank = BankFor(mode)) {
        state_.r[13] = bank->r13;
        state_.r[14] = bank->r14;
    }
}

void Arm7Tdmi::SetCpsr(std::uint32_t value) {
    const std::uint32_t old_mode = state_.cpsr & kModeMask;
    const std::uint32_t new_mode = value & kModeMask;
    if (old_mode != new_mode) {
        SaveBank(old_mode);

        // IRQ, Supervisor, Abort, Undefined and User/System all share
        // r8-r12. FIQ alone has a private copy. Preserve the shared set when
        // crossing that boundary instead of restoring a stale user snapshot
        // on every IRQ return.
        if (old_mode == kModeFiq && new_mode != kModeFiq) {
            for (unsigned i = 0; i < 5; ++i) {
                state_.r[8 + i] = user_r8_12_[i];
            }
        } else if (old_mode != kModeFiq && new_mode == kModeFiq) {
            for (unsigned i = 0; i < 5; ++i) {
                user_r8_12_[i] = state_.r[8 + i];
            }
        }

        state_.cpsr = (state_.cpsr & ~kModeMask) | new_mode;
        LoadBank(new_mode);
    }
    state_.cpsr = value;
}

void Arm7Tdmi::Unsupported(std::uint32_t pc, std::uint32_t instruction,
                           bool thumb) const {
    std::ostringstream message;
    message << (thumb ? "unsupported Thumb" : "unsupported ARM")
            << " instruction at 0x" << std::hex << std::uppercase << pc
            << ": 0x" << std::setw(thumb ? 4 : 8) << std::setfill('0')
            << instruction;
    throw CpuError(message.str());
}

void Arm7Tdmi::HandleSoftwareInterrupt(unsigned number, std::uint32_t pc,
                                       bool thumb) {
    RecordSoftwareInterrupt(number, pc, thumb, false);
    switch (number & 0xFFu) {
    case 0x00: { // SoftReset
        // The BIOS samples this selector before clearing its 0x200-byte
        // work area: zero restarts the cartridge, non-zero restarts code in
        // EWRAM (the multiboot path). SoftReset never returns to its caller.
        const bool ram_restart = bus_.Read8(0x03007FFAu) != 0;
        bus_.ResetBiosWorkArea();
        Reset(ram_restart ? 0x02000000u : 0x08000000u);
        state_.r[13] = 0x03007F00u;
        user_sp_ = state_.r[13];
        irq_bank_.r13 = 0x03007FA0u;
        supervisor_bank_.r13 = 0x03007FE0u;
        return;
    }
    case 0x01: // RegisterRamReset
        bus_.ResetRam(static_cast<std::uint8_t>(state_.r[0]));
        return;
    case 0x02: // Halt
        bus_.RequestPower(PowerRequest::Halt);
        return;
    case 0x03: // Stop
    case 0x04: // IntrWait
    case 0x05: // VBlankIntrWait
        throw CpuError("HLE Stop/IntrWait/VBlankIntrWait not implemented; supply an external BIOS");
    case 0x06: // Div: numerator r0 / denominator r1
    case 0x07: { // DivArm: numerator r1 / denominator r0
        const bool reverse = (number & 0xFFu) == 0x07u;
        const std::int32_t numerator = static_cast<std::int32_t>(
            reverse ? state_.r[1] : state_.r[0]);
        const std::int32_t denominator = static_cast<std::int32_t>(
            reverse ? state_.r[0] : state_.r[1]);
        if (denominator == 0) {
            state_.r[0] = numerator < 0 ? 0xFFFFFFFFu : 1u;
            state_.r[1] = static_cast<std::uint32_t>(numerator);
            state_.r[3] = 1u;
        } else if (numerator == std::numeric_limits<std::int32_t>::min() &&
                   denominator == -1) {
            state_.r[0] = 0x80000000u;
            state_.r[1] = 0;
            state_.r[3] = 0x80000000u;
        } else {
            const std::int32_t quotient = numerator / denominator;
            const std::int32_t remainder = numerator % denominator;
            state_.r[0] = static_cast<std::uint32_t>(quotient);
            state_.r[1] = static_cast<std::uint32_t>(remainder);
            state_.r[3] = quotient < 0 ? 0u - static_cast<std::uint32_t>(quotient)
                                       : static_cast<std::uint32_t>(quotient);
        }
        return;
    }
    case 0x08: { // Sqrt
        const double root = std::sqrt(static_cast<double>(state_.r[0]));
        state_.r[0] = static_cast<std::uint32_t>(root);
        return;
    }
    case 0x0B: // CpuSet
    case 0x0C: { // CpuFastSet
        const bool fast = (number & 0xFFu) == 0x0Cu;
        std::uint32_t source = state_.r[0];
        std::uint32_t destination = state_.r[1];
        const std::uint32_t control = state_.r[2];
        const bool word = fast || ((control & (1u << 26)) != 0);
        const bool fixed_source = (control & (1u << 24)) != 0;
        const std::uint32_t units = control & 0x001FFFFFu;
        const std::uint32_t count = fast ? ((units + 7u) & ~7u) : units;
        if (word) {
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t value = bus_.Read32(source);
                bus_.Write32(destination, value);
                if (!fixed_source) source += 4;
                destination += 4;
            }
        } else {
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t value = bus_.Read16(source);
                bus_.Write16(destination, static_cast<std::uint16_t>(value));
                if (!fixed_source) source += 2;
                destination += 2;
            }
        }
        return;
    }
    case 0x11:
    case 0x12: {
        std::uint32_t source = state_.r[0];
        const std::uint32_t destination = state_.r[1];
        const std::uint32_t header = bus_.Read32(source);
        if ((header & 0xFFu) != 0x10u) throw CpuError("invalid BIOS LZ77 header");
        const std::uint32_t length = header >> 8;
        std::vector<std::uint8_t> output;
        output.reserve(length);
        source += 4;
        while (output.size() < length) {
            const unsigned flags = bus_.Read8(source++);
            for (unsigned bit = 0; bit < 8 && output.size() < length; ++bit) {
                if ((flags & (0x80u >> bit)) == 0) {
                    output.push_back(bus_.Read8(source++));
                } else {
                    const unsigned first = bus_.Read8(source++);
                    const unsigned second = bus_.Read8(source++);
                    const unsigned distance = ((first & 15u) << 8) + second + 1;
                    if (distance > output.size()) throw CpuError("invalid BIOS LZ77 back-reference");
                    for (unsigned count = 0; count < (first >> 4) + 3 && output.size() < length; ++count) {
                        output.push_back(output[output.size() - distance]);
                    }
                }
            }
        }
        for (std::size_t index = 0; index < output.size(); ++index) {
            bus_.Write8(destination + static_cast<std::uint32_t>(index), output[index]);
        }
        return;
    }
    default: {
        std::ostringstream message;
        message << "unsupported BIOS SWI #0x" << std::hex << std::uppercase
                << (number & 0xFFu) << " at 0x" << pc;
        throw CpuError(message.str());
    }
    }
    (void)thumb;
}

void Arm7Tdmi::ExecuteArm(std::uint32_t instruction, std::uint32_t pc) {
    if (!ConditionPassed(instruction >> 28)) {
        return;
    }

    // BX / branch exchange.
    if ((instruction & 0x0FFFFFF0u) == 0x012FFF10u) {
        const std::uint32_t target = ReadArmRegister(instruction & 0xFu, pc);
        const bool thumb = (target & 1u) != 0;
        SetFlag(kCpsrT, thumb);
        state_.r[15] = thumb ? (target & ~1u) : (target & ~3u);
        return;
    }

    // B / BL.
    if ((instruction & 0x0E000000u) == 0x0A000000u) {
        const std::int64_t offset =
            static_cast<std::int64_t>(SignExtend(instruction & 0x00FFFFFFu, 24)) * 4;
        if ((instruction & (1u << 24)) != 0) {
            state_.r[14] = pc + 4;
        }
        state_.r[15] = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(pc) + 8 + offset);
        return;
    }

    if ((instruction & 0x0F000000u) == 0x0F000000u) {
        const unsigned number = (instruction >> 16) & 0xFFu;
        if (ShouldExecuteBiosSwi(number)) {
            EnterSoftwareInterrupt(number, pc, false);
        } else {
            HandleSoftwareInterrupt(number, pc, false);
        }
        return;
    }

    // MRS Rd, CPSR/SPSR.
    if ((instruction & 0x0FBF0FFFu) == 0x010F0000u) {
        const unsigned rd = (instruction >> 12) & 0xFu;
        const bool spsr = (instruction & (1u << 22)) != 0;
        const Bank* bank = BankFor(state_.cpsr & kModeMask);
        WriteRegister(rd, spsr && bank != nullptr ? bank->spsr : state_.cpsr);
        return;
    }

    // SWP/SWPB. IntrMain uses SWP with PC as the destination to atomically
    // exchange the BIOS IRQ vector and return through the saved pointer.
    if ((instruction & 0x0FB00FF0u) == 0x01000090u) {
        const bool byte = (instruction & (1u << 22)) != 0;
        const unsigned rn = (instruction >> 16) & 0xFu;
        const unsigned rd = (instruction >> 12) & 0xFu;
        const unsigned rm = instruction & 0xFu;
        const std::uint32_t address = ReadArmRegister(rn, pc);
        const std::uint32_t old_value = byte
            ? bus_.CpuRead8(address)
            : bus_.CpuRead32(address);
        const std::uint32_t new_value = ReadArmRegister(rm, pc);
        if (byte) {
            bus_.CpuWrite8(address, static_cast<std::uint8_t>(new_value));
        } else {
            bus_.CpuWrite32(address, new_value);
        }
        WriteRegister(rd, old_value);
        return;
    }

    // MSR CPSR_<fields>, Rm / immediate.
    if ((instruction & 0x0FB0FFF0u) == 0x0120F000u ||
        (instruction & 0x0FB0F000u) == 0x0320F000u) {
        const unsigned fields = (instruction >> 16) & 0xFu;
        const bool immediate = (instruction & (1u << 25)) != 0;
        const bool spsr = (instruction & (1u << 22)) != 0;
        bool ignored_carry = false;
        const std::uint32_t value = immediate
            ? DecodeArmOperand2(instruction, pc, ignored_carry)
            : ReadArmRegister(instruction & 0xFu, pc);
        std::uint32_t mask = 0;
        if ((fields & 1u) != 0) mask |= 0x000000FFu;
        if ((fields & 2u) != 0) mask |= 0x0000FF00u;
        if ((fields & 4u) != 0) mask |= 0x00FF0000u;
        if ((fields & 8u) != 0) mask |= 0xFF000000u;
        if (spsr) {
            Bank* bank = BankFor(state_.cpsr & kModeMask);
            if (bank != nullptr) {
                bank->spsr = (bank->spsr & ~mask) | (value & mask);
            }
        } else {
            SetCpsr((state_.cpsr & ~mask) | (value & mask));
        }
        return;
    }

    // Multiply and multiply-accumulate.
    if ((instruction & 0x0FC000F0u) == 0x00000090u) {
        const unsigned rd = (instruction >> 16) & 0xFu;
        const unsigned rn = (instruction >> 12) & 0xFu;
        const unsigned rs = (instruction >> 8) & 0xFu;
        const unsigned rm = instruction & 0xFu;
        std::uint32_t result = ReadArmRegister(rm, pc) * ReadArmRegister(rs, pc);
        if ((instruction & (1u << 21)) != 0) {
            result += ReadArmRegister(rn, pc);
        }
        WriteRegister(rd, result);
        if ((instruction & (1u << 20)) != 0) {
            SetNz(result);
        }
        return;
    }

    // Halfword and signed data transfer. The IRQ handler and BIOS support
    // code use LDRH/STRH heavily for IE/IF and timer registers.
    if ((instruction & 0x0E000090u) == 0x00000090u) {
        const bool immediate = (instruction & (1u << 22)) != 0;
        const bool pre_index = (instruction & (1u << 24)) != 0;
        const bool add = (instruction & (1u << 23)) != 0;
        const bool writeback = (instruction & (1u << 21)) != 0;
        const bool load = (instruction & (1u << 20)) != 0;
        const unsigned rn = (instruction >> 16) & 0xFu;
        const unsigned rd = (instruction >> 12) & 0xFu;
        const unsigned transfer_type = (instruction >> 5) & 3u;
        const std::uint32_t offset = immediate
            ? (((instruction >> 4) & 0xF0u) | (instruction & 0x0Fu))
            : ReadArmRegister(instruction & 0xFu, pc);
        const std::uint32_t base = ReadArmRegister(rn, pc);
        const std::uint32_t adjusted = add ? base + offset : base - offset;
        const std::uint32_t address = pre_index ? adjusted : base;

        if (load) {
            std::uint32_t value = 0;
            if (transfer_type == 1u) {
                value = bus_.CpuRead16(address);
            } else if (transfer_type == 2u) {
                value = static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(static_cast<std::int8_t>(
                        bus_.CpuRead8(address))));
            } else {
                value = static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(static_cast<std::int16_t>(
                        bus_.CpuRead16(address))));
            }
            WriteRegister(rd, value);
        } else {
            bus_.CpuWrite16(address,
                            static_cast<std::uint16_t>(ReadArmRegister(rd, pc)));
        }
        if (!pre_index || writeback) {
            state_.r[rn] = adjusted;
        }
        return;
    }

    // Single data transfer: LDR/STR, immediate or shifted-register offset.
    if ((instruction & 0x0C000000u) == 0x04000000u) {
        const bool register_offset = (instruction & (1u << 25)) != 0;
        const bool pre_index = (instruction & (1u << 24)) != 0;
        const bool add = (instruction & (1u << 23)) != 0;
        const bool byte = (instruction & (1u << 22)) != 0;
        const bool writeback = (instruction & (1u << 21)) != 0;
        const bool load = (instruction & (1u << 20)) != 0;
        const unsigned rn = (instruction >> 16) & 0xFu;
        const unsigned rd = (instruction >> 12) & 0xFu;
        std::uint32_t offset = instruction & 0xFFFu;
        if (register_offset) {
            bool ignored_carry = false;
            offset = Shift(ReadArmRegister(instruction & 0xFu, pc),
                           (instruction >> 5) & 3u, (instruction >> 7) & 0x1Fu,
                           ignored_carry, false);
        }

        const std::uint32_t base = ReadArmRegister(rn, pc);
        const std::uint32_t adjusted = add ? base + offset : base - offset;
        const std::uint32_t address = pre_index ? adjusted : base;
        if (load) {
            const std::uint32_t value = byte
                ? bus_.CpuRead8(address) : bus_.CpuRead32(address);
            WriteRegister(rd, value);
        } else {
            const std::uint32_t value = ReadArmRegister(rd, pc);
            if (byte) {
                bus_.CpuWrite8(address, static_cast<std::uint8_t>(value));
            } else {
                bus_.CpuWrite32(address, value);
            }
        }
        if (!pre_index || writeback) {
            state_.r[rn] = adjusted;
        }
        return;
    }

    // LDM/STM. This is enough for common ARM prologues and is also useful for
    // making stack/register state observable before the full device model.
    if ((instruction & 0x0E000000u) == 0x08000000u) {
        const bool pre_index = (instruction & (1u << 24)) != 0;
        const bool add = (instruction & (1u << 23)) != 0;
        const bool user_transfer = (instruction & (1u << 22)) != 0 &&
                                   (instruction & (1u << 15)) == 0;
        const bool writeback = (instruction & (1u << 21)) != 0;
        const bool load = (instruction & (1u << 20)) != 0;
        const unsigned rn = (instruction >> 16) & 0xFu;
        const std::uint32_t list = instruction & 0xFFFFu;
        const bool restores_status = load && (instruction & (1u << 22)) != 0 &&
                                     (list & (1u << 15)) != 0;
        const Bank* saved_bank = restores_status
            ? BankFor(state_.cpsr & kModeMask)
            : nullptr;
        const bool restored_thumb = saved_bank != nullptr &&
                                    (saved_bank->spsr & kCpsrT) != 0;
        unsigned count = 0;
        for (unsigned i = 0; i < 16; ++i) {
            if ((list & (1u << i)) != 0) ++count;
        }
        const std::uint32_t base = ReadArmRegister(rn, pc);
        std::uint32_t address = add ? base + (pre_index ? 4u : 0u)
                                    : base - count * 4u + (pre_index ? 0u : 4u);
        const bool in_fiq = (state_.cpsr & kModeMask) == kModeFiq;
        const bool privileged = (state_.cpsr & kModeMask) != kModeUser &&
                                (state_.cpsr & kModeMask) != kModeSystem;
        auto ReadUserRegister = [this, in_fiq, privileged, pc](unsigned index) {
            if (index < 8u) return state_.r[index];
            if (index < 13u) {
                return in_fiq ? user_r8_12_[index - 8u] : state_.r[index];
            }
            if (index == 13u) {
                return in_fiq || privileged
                    ? user_sp_
                    : state_.r[13];
            }
            if (index == 14u) {
                return in_fiq || privileged
                    ? user_lr_
                    : state_.r[14];
            }
            return ReadArmRegister(index, pc);
        };
        auto WriteUserRegister = [this, in_fiq](unsigned index,
                                                 std::uint32_t value) {
            if (index < 8u) {
                state_.r[index] = value;
            } else if (index < 13u) {
                if (in_fiq) user_r8_12_[index - 8u] = value;
                else state_.r[index] = value;
            } else if (index == 13u) {
                user_sp_ = value;
                if (!in_fiq && ((state_.cpsr & kModeMask) == kModeUser ||
                                (state_.cpsr & kModeMask) == kModeSystem)) {
                    state_.r[13] = value;
                }
            } else if (index == 14u) {
                user_lr_ = value;
                if (!in_fiq && ((state_.cpsr & kModeMask) == kModeUser ||
                                (state_.cpsr & kModeMask) == kModeSystem)) {
                    state_.r[14] = value;
                }
            }
        };
        for (unsigned i = 0; i < 16; ++i) {
            if ((list & (1u << i)) == 0) continue;
            if (load) {
                const std::uint32_t value = bus_.CpuRead32(address);
                if (user_transfer && i != 15u) {
                    WriteUserRegister(i, value);
                } else {
                    WriteRegister(i, value, i == 15u && restores_status
                                             ? restored_thumb
                                             : false);
                }
            } else {
                bus_.CpuWrite32(address, user_transfer
                                       ? ReadUserRegister(i)
                                       : ReadArmRegister(i, pc));
            }
            address += 4;
        }
        if (writeback) {
            state_.r[rn] = add ? base + count * 4u : base - count * 4u;
        }
        if (restores_status) {
            RestoreSavedStatus();
        }
        return;
    }

    // Data-processing instructions.
    if ((instruction & 0x0C000000u) == 0x00000000u) {
        const unsigned opcode = (instruction >> 21) & 0xFu;
        const bool set_flags = (instruction & (1u << 20)) != 0;
        const unsigned rn = (instruction >> 16) & 0xFu;
        const unsigned rd = (instruction >> 12) & 0xFu;
        bool operand_carry = false;
        const std::uint32_t operand = DecodeArmOperand2(instruction, pc, operand_carry);
        const std::uint32_t left = ReadArmRegister(rn, pc);
        std::uint32_t result = 0;

        switch (opcode) {
        case 0x0: result = left & operand; break; // AND
        case 0x1: result = left ^ operand; break; // EOR
        case 0x2: result = SubWithBorrow(left, operand, false, set_flags); break; // SUB
        case 0x3: result = SubWithBorrow(operand, left, false, set_flags); break; // RSB
        case 0x4: result = AddWithCarry(left, operand, false, set_flags); break; // ADD
        case 0x5: result = AddWithCarry(left, operand, Flag(kCpsrC), set_flags); break; // ADC
        case 0x6: result = SubWithBorrow(left, operand, !Flag(kCpsrC), set_flags); break; // SBC
        case 0x7: result = SubWithBorrow(operand, left, !Flag(kCpsrC), set_flags); break; // RSC
        case 0x8: SetNz(left & operand); SetFlag(kCpsrC, operand_carry); return; // TST
        case 0x9: SetNz(left ^ operand); SetFlag(kCpsrC, operand_carry); return; // TEQ
        case 0xA: SubWithBorrow(left, operand, false, true); return; // CMP
        case 0xB: AddWithCarry(left, operand, false, true); return; // CMN
        case 0xC: result = left | operand; break; // ORR
        case 0xD: result = operand; break; // MOV
        case 0xE: result = left & ~operand; break; // BIC
        case 0xF: result = ~operand; break; // MVN
        default: Unsupported(pc, instruction, false);
        }

        const bool restores_status = rd == 15 && set_flags;
        if (restores_status) {
            const Bank* saved_bank = BankFor(state_.cpsr & kModeMask);
            const bool restored_thumb = saved_bank != nullptr &&
                                        (saved_bank->spsr & kCpsrT) != 0;
            WriteRegister(rd, result, restored_thumb);
            RestoreSavedStatus();
        } else {
            WriteRegister(rd, result);
        }
        if (set_flags && !restores_status &&
            (opcode == 0x0 || opcode == 0x1 || opcode == 0xC ||
             opcode == 0xD || opcode == 0xE || opcode == 0xF)) {
            SetNz(result);
            SetFlag(kCpsrC, operand_carry);
        }
        return;
    }

    Unsupported(pc, instruction, false);
}

void Arm7Tdmi::ExecuteThumb(std::uint16_t instruction, std::uint32_t pc) {
    // Move shifted register.  The add/subtract-register format shares the
    // top three bits, so exclude its 0x1800..0x1FFF range explicitly.
    if ((instruction & 0xE000u) == 0x0000u &&
        (instruction & 0x1800u) != 0x1800u) {
        const unsigned op = (instruction >> 11) & 3u;
        const unsigned amount = (instruction >> 6) & 0x1Fu;
        const unsigned rs = (instruction >> 3) & 7u;
        const unsigned rd = instruction & 7u;
        bool carry = false;
        const std::uint32_t value = Shift(state_.r[rs], op, amount, carry, false);
        state_.r[rd] = value;
        SetNz(value);
        SetFlag(kCpsrC, carry);
        return;
    }

    // Add/subtract register or small immediate.
    if ((instruction & 0xF800u) == 0x1800u) {
        const bool immediate = (instruction & (1u << 10)) != 0;
        const bool subtract = (instruction & (1u << 9)) != 0;
        const unsigned rn = (instruction >> 3) & 7u;
        const unsigned rd = instruction & 7u;
        const std::uint32_t right = immediate ? ((instruction >> 6) & 7u)
                                               : state_.r[(instruction >> 6) & 7u];
        state_.r[rd] = subtract ? SubWithBorrow(state_.r[rn], right, false, true)
                                : AddWithCarry(state_.r[rn], right, false, true);
        return;
    }

    // MOV/CMP/ADD/SUB with an 8-bit immediate.
    if ((instruction & 0xE000u) == 0x2000u) {
        const unsigned op = (instruction >> 11) & 3u;
        const unsigned rd = (instruction >> 8) & 7u;
        const std::uint32_t immediate = instruction & 0xFFu;
        switch (op) {
        case 0: state_.r[rd] = immediate; SetNz(immediate); break;
        case 1: SubWithBorrow(state_.r[rd], immediate, false, true); break;
        case 2: state_.r[rd] = AddWithCarry(state_.r[rd], immediate, false, true); break;
        case 3: state_.r[rd] = SubWithBorrow(state_.r[rd], immediate, false, true); break;
        }
        return;
    }

    // ALU operations on low registers.
    if ((instruction & 0xFC00u) == 0x4000u) {
        const unsigned op = (instruction >> 6) & 0xFu;
        const unsigned rs = (instruction >> 3) & 7u;
        const unsigned rd = instruction & 7u;
        const std::uint32_t left = state_.r[rd];
        const std::uint32_t right = state_.r[rs];
        bool carry = false;
        std::uint32_t result = 0;
        switch (op) {
        case 0x0: result = left & right; SetNz(result); break;
        case 0x1: result = left ^ right; SetNz(result); break;
        case 0x2: result = Shift(left, 0, right & 0xFFu, carry, true); SetNz(result); SetFlag(kCpsrC, carry); break;
        case 0x3: result = Shift(left, 1, right & 0xFFu, carry, true); SetNz(result); SetFlag(kCpsrC, carry); break;
        case 0x4: result = Shift(left, 2, right & 0xFFu, carry, true); SetNz(result); SetFlag(kCpsrC, carry); break;
        case 0x5: result = AddWithCarry(left, right, Flag(kCpsrC), true); break;
        case 0x6: result = SubWithBorrow(left, right, !Flag(kCpsrC), true); break;
        case 0x7: result = Shift(left, 3, right & 0xFFu, carry, true); SetNz(result); SetFlag(kCpsrC, carry); break;
        case 0x8: SetNz(left & right); return;
        case 0x9: result = SubWithBorrow(0, right, false, true); break;
        case 0xA: SubWithBorrow(left, right, false, true); return;
        case 0xB: AddWithCarry(left, right, false, true); return;
        case 0xC: result = left | right; SetNz(result); break;
        case 0xD: result = left * right; SetNz(result); break;
        case 0xE: result = left & ~right; SetNz(result); break;
        case 0xF: result = ~right; SetNz(result); break;
        }
        if (op != 0x8 && op != 0xA && op != 0xB) {
            state_.r[rd] = result;
        }
        return;
    }

    // High-register operations and BX.
    if ((instruction & 0xFC00u) == 0x4400u) {
        const unsigned op = (instruction >> 8) & 3u;
        const unsigned rs = ((instruction >> 3) & 7u) | (((instruction >> 6) & 1u) << 3);
        const unsigned rd = (instruction & 7u) | (((instruction >> 7) & 1u) << 3);
        const std::uint32_t right = ReadThumbRegister(rs, pc);
        if (op == 0) {
            WriteRegister(rd, ReadThumbRegister(rd, pc) + right, true);
        } else if (op == 1) {
            SubWithBorrow(ReadThumbRegister(rd, pc), right, false, true);
        } else if (op == 2) {
            WriteRegister(rd, right, true);
        } else {
            const bool thumb = (right & 1u) != 0;
            SetFlag(kCpsrT, thumb);
            state_.r[15] = thumb ? (right & ~1u) : (right & ~3u);
        }
        return;
    }

    // PC-relative literal load.
    if ((instruction & 0xF800u) == 0x4800u) {
        const unsigned rd = (instruction >> 8) & 7u;
        const std::uint32_t address = ((pc + 4) & ~3u) + ((instruction & 0xFFu) << 2);
        state_.r[rd] = bus_.CpuRead32(address);
        return;
    }

    // Register-offset loads and stores.
    if ((instruction & 0xF000u) == 0x5000u) {
        const unsigned op = (instruction >> 9) & 7u;
        const unsigned rm = (instruction >> 6) & 7u;
        const unsigned rn = (instruction >> 3) & 7u;
        const unsigned rd = instruction & 7u;
        const std::uint32_t address = state_.r[rn] + state_.r[rm];
        switch (op) {
        case 0: bus_.CpuWrite32(address, state_.r[rd]); break;
        case 1: bus_.CpuWrite16(address, static_cast<std::uint16_t>(state_.r[rd])); break;
        case 2: bus_.CpuWrite8(address, static_cast<std::uint8_t>(state_.r[rd])); break;
        case 3:
            state_.r[rd] = static_cast<std::int8_t>(bus_.CpuRead8(address));
            break;
        case 4: state_.r[rd] = bus_.CpuRead32(address); break;
        case 5: state_.r[rd] = bus_.CpuRead16(address); break;
        case 6: state_.r[rd] = bus_.CpuRead8(address); break;
        case 7:
            state_.r[rd] = (address & 1u) != 0
                ? static_cast<std::int8_t>(bus_.CpuRead8(address))
                : static_cast<std::int16_t>(bus_.CpuRead16(address));
            break;
        }
        return;
    }

    // Immediate word/byte load/store.
    if ((instruction & 0xE000u) == 0x6000u) {
        const unsigned op = (instruction >> 11) & 3u;
        const unsigned offset = (instruction >> 6) & 0x1Fu;
        const unsigned rn = (instruction >> 3) & 7u;
        const unsigned rd = instruction & 7u;
        const bool byte = op >= 2;
        const std::uint32_t address = state_.r[rn] + (byte ? offset : offset << 2);
        if ((op & 1u) == 0) {
            if (byte) bus_.CpuWrite8(address, static_cast<std::uint8_t>(state_.r[rd]));
            else bus_.CpuWrite32(address, state_.r[rd]);
        } else {
            state_.r[rd] = byte ? bus_.CpuRead8(address) : bus_.CpuRead32(address);
        }
        return;
    }

    // Immediate halfword load/store.
    if ((instruction & 0xF000u) == 0x8000u) {
        const bool load = (instruction & (1u << 11)) != 0;
        const std::uint32_t address = state_.r[(instruction >> 3) & 7u] +
                                      (((instruction >> 6) & 0x1Fu) << 1);
        const unsigned rd = instruction & 7u;
        if (load) state_.r[rd] = bus_.CpuRead16(address);
        else bus_.CpuWrite16(address, static_cast<std::uint16_t>(state_.r[rd]));
        return;
    }

    // SP-relative word load/store.
    if ((instruction & 0xF000u) == 0x9000u) {
        const bool load = (instruction & (1u << 11)) != 0;
        const unsigned rd = (instruction >> 8) & 7u;
        const std::uint32_t address = state_.r[13] + ((instruction & 0xFFu) << 2);
        if (load) state_.r[rd] = bus_.CpuRead32(address);
        else bus_.CpuWrite32(address, state_.r[rd]);
        return;
    }

    // ADD Rd, PC/SP, #imm.
    if ((instruction & 0xF000u) == 0xA000u) {
        const unsigned rd = (instruction >> 8) & 7u;
        const std::uint32_t base = (instruction & (1u << 11)) != 0
            ? state_.r[13]
            : ((pc + 4) & ~3u);
        state_.r[rd] = base + ((instruction & 0xFFu) << 2);
        return;
    }

    // PUSH/POP.
    if ((instruction & 0xFE00u) == 0xB400u ||
        (instruction & 0xFE00u) == 0xBC00u) {
        const bool pop = (instruction & 0x0800u) != 0;
        const bool extra = (instruction & 0x0100u) != 0;
        const std::uint32_t list = instruction & 0xFFu;
        unsigned count = 0;
        for (unsigned i = 0; i < 8; ++i) if ((list & (1u << i)) != 0) ++count;
        if (extra) ++count;
        std::uint32_t address = pop ? state_.r[13] : state_.r[13] - count * 4u;
        if (!pop) {
            for (unsigned i = 0; i < 8; ++i) {
                if ((list & (1u << i)) != 0) {
                    bus_.CpuWrite32(address, state_.r[i]);
                    address += 4;
                }
            }
            if (extra) {
                bus_.CpuWrite32(address, state_.r[14]);
                address += 4;
            }
            state_.r[13] -= count * 4u;
        } else {
            for (unsigned i = 0; i < 8; ++i) {
                if ((list & (1u << i)) != 0) {
                    state_.r[i] = bus_.CpuRead32(address);
                    address += 4;
                }
            }
            if (extra) {
                const std::uint32_t target = bus_.CpuRead32(address);
                state_.r[15] = target & ~1u;
                address += 4;
            }
            state_.r[13] += count * 4u;
        }
        return;
    }

    // ADD/SUB SP, #imm.
    if ((instruction & 0xFF00u) == 0xB000u) {
        const std::uint32_t amount = (instruction & 0x7Fu) << 2;
        if ((instruction & 0x0080u) != 0) state_.r[13] -= amount;
        else state_.r[13] += amount;
        return;
    }

    // STM/LDMIA Rn!, register list.
    if ((instruction & 0xF000u) == 0xC000u) {
        const bool load = (instruction & (1u << 11)) != 0;
        const unsigned rn = (instruction >> 8) & 7u;
        std::uint32_t address = state_.r[rn];
        for (unsigned i = 0; i < 8; ++i) {
            if ((instruction & (1u << i)) == 0) continue;
            if (load) state_.r[i] = bus_.CpuRead32(address);
            else bus_.CpuWrite32(address, state_.r[i]);
            address += 4;
        }
        if (!load || (instruction & (1u << rn)) == 0) {
            state_.r[rn] = address;
        }
        return;
    }

    // Conditional branch and SWI.
    if ((instruction & 0xF000u) == 0xD000u) {
        const unsigned condition = (instruction >> 8) & 0xFu;
        if (condition == 0xFu) {
            const unsigned number = instruction & 0xFFu;
            if (ShouldExecuteBiosSwi(number)) {
                EnterSoftwareInterrupt(number, pc, true);
            } else {
                HandleSoftwareInterrupt(number, pc, true);
            }
            return;
        }
        if (ConditionPassed(condition)) {
            const std::int64_t offset =
                static_cast<std::int64_t>(SignExtend(instruction & 0xFFu, 8)) * 2;
            state_.r[15] = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(pc) + 4 + offset);
        }
        return;
    }

    // Unconditional branch.
    if ((instruction & 0xF800u) == 0xE000u) {
            const std::int64_t offset =
                static_cast<std::int64_t>(SignExtend(instruction & 0x7FFu, 11)) * 2;
        state_.r[15] = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(pc) + 4 + offset);
        return;
    }

    // Thumb BL pair.
    if ((instruction & 0xF800u) == 0xF000u) {
        const std::int64_t offset =
            static_cast<std::int64_t>(SignExtend(instruction & 0x7FFu, 11)) * 4096;
        state_.r[14] = static_cast<std::uint32_t>(
            static_cast<std::int64_t>(pc) + 4 + offset);
        return;
    }
    if ((instruction & 0xF800u) == 0xF800u) {
        const std::uint32_t target = state_.r[14] + ((instruction & 0x7FFu) << 1);
        state_.r[14] = (pc + 2) | 1u;
        state_.r[15] = target & ~1u;
        return;
    }

    Unsupported(pc, instruction, true);
}

} // namespace ngba
