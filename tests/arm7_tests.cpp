#include "ngba/arm7.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

void WriteLe32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void WriteLe16(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void WriteAscii(std::vector<std::uint8_t>& bytes, std::size_t offset,
                const std::string& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value[i]);
    }
}

std::uint8_t Complement(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0xA0; i <= 0xBC; ++i) sum += bytes[i];
    return static_cast<std::uint8_t>(0u - sum - 0x19u);
}

} // namespace

int main() {
    std::vector<std::uint8_t> bytes(0x400, 0);
    WriteLe32(bytes, 0, 0xEA000032u);
    WriteAscii(bytes, 0xA0, "CPU TEST");
    WriteAscii(bytes, 0xAC, "TEST");
    WriteAscii(bytes, 0xB0, "01");
    bytes[0xB2] = 0x96;
    bytes[0xBD] = Complement(bytes);

    // ARM at 0xD0: MOV r0,#1; ADD r0,r0,#2; STR r0,[r1]; BX r2.
    WriteLe32(bytes, 0xD0, 0xE3A00001u);
    WriteLe32(bytes, 0xD4, 0xE2800002u);
    WriteLe32(bytes, 0xD8, 0xE5810000u);
    WriteLe32(bytes, 0xDC, 0xE12FFF12u);
    // ARM at 0xE0: SWP r0,r2,[r1].
    WriteLe32(bytes, 0xE0, 0xE1010092u);
    // Thumb at 0x100: MOVS r3,#7; ADDS r3,#1; ADD r0,r4,#0; B .
    WriteLe16(bytes, 0x100, 0x2307u);
    WriteLe16(bytes, 0x102, 0x3301u);
    WriteLe16(bytes, 0x104, 0x1C20u);
    WriteLe16(bytes, 0x106, 0xE7FEu);

    // IRQ-return alignment fixture: ARM setup switches to System+Thumb, and
    // the handler below clears IF before returning through the BIOS shim.
    WriteLe32(bytes, 0x140, 0xE129F000u); // MSR CPSR_c,r0
    WriteLe32(bytes, 0x144, 0xE1A0D001u); // MOV sp,r1
    WriteLe32(bytes, 0x148, 0xE129F002u); // MSR CPSR_c,r2 (system+Thumb)
    WriteLe16(bytes, 0x14C, 0x46C0u);    // NOP
    WriteLe32(bytes, 0x180, 0xE3A00001u); // MOV r0,#1
    WriteLe32(bytes, 0x184, 0xE59F1008u); // LDR r1,[pc,#8]
    WriteLe32(bytes, 0x188, 0xE1C100B0u); // STRH r0,[r1]
    WriteLe32(bytes, 0x18C, 0xE12FFF1Eu); // BX lr
    WriteLe32(bytes, 0x194, 0x04000202u); // IF
    WriteLe32(bytes, 0x200, 0xEF020000u); // ARM SWI 0x020000 (Halt)
    WriteLe16(bytes, 0x220, 0xDF02u);    // Thumb SWI 0x02 (Halt)
    WriteLe32(bytes, 0x240, 0xEF040000u); // ARM SWI 0x040000 (IntrWait)
    WriteLe32(bytes, 0x244, 0xEF050000u); // ARM SWI 0x050000 (VBlankIntrWait)
    WriteLe32(bytes, 0x280, 0xEF0C0000u); // ARM SWI 0x0C (CpuFastSet)

    const std::string path = "ngba_arm7_test.gba";
    {
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    const ngba::RomImage rom = ngba::RomImage::Load(path);
    ngba::MemoryBus bus(rom);

    // KEYINPUT is active-low and read-only. KEYCNT can request a keypad IRQ
    // when the selected physical buttons satisfy its OR/AND condition.
    assert(bus.Read16(0x04000130u) == 0x03FFu);
    const std::uint16_t pressed = static_cast<std::uint16_t>(
        (1u << 0) | (1u << 3) | (1u << 4)); // A, Start, Right
    bus.SetKeys(pressed);
    assert(bus.Read16(0x04000130u) ==
           static_cast<std::uint16_t>(0x03FFu & ~pressed));
    bus.Write16(0x04000130u, 0);
    assert(bus.Read16(0x04000130u) ==
           static_cast<std::uint16_t>(0x03FFu & ~pressed));
    bus.Write16(0x04000132u, 0xC001u); // IRQ + AND, wait for A.
    assert((bus.Read16(0x04000202u) & 0x1000u) != 0);
    bus.Write16(0x04000202u, 0x1000u);
    bus.SetKeys(0);
    bus.Write16(0x04000132u, 0x8001u); // IRQ + OR, wait for A.
    assert((bus.Read16(0x04000202u) & 0x1000u) == 0);
    bus.SetKeys(1u << 0);
    assert((bus.Read16(0x04000202u) & 0x1000u) != 0);
    bus.Write16(0x04000202u, 0x1000u);
    bus.Write16(0x04000132u, 0);
    bus.SetKeys(0);

    // WAITCNT is writable through its documented mask, and CPU accesses add
    // only the wait-state excess on top of the interpreter's internal model.
    assert(bus.Read16(0x04000204u) == 0);
    bus.Write16(0x04000204u, 0xFFFFu);
    assert(bus.Read16(0x04000204u) == 0x7FFFu);
    bus.Write16(0x04000204u, 0x0000u);

    bus.ResetCpuTiming();
    bus.BeginCpuInstruction();
    (void)bus.CpuFetch32(0x08000000u);
    assert(bus.EndCpuInstruction() == 5u); // 4N + 2S, minus baseline 1.

    bus.BeginCpuInstruction();
    (void)bus.CpuFetch32(0x08000004u);
    assert(bus.EndCpuInstruction() == 3u); // 2S + 2S, minus baseline 1.

    bus.BeginCpuInstruction();
    (void)bus.CpuFetch32(0x03000000u);
    (void)bus.CpuRead32(0x02000000u);
    assert(bus.EndCpuInstruction() == 4u); // EWRAM 32-bit read: 3+3 - 2.

    bus.Write16(0x04000204u, 0x4317u); // common 3/1 WS0 configuration
    bus.ResetCpuTiming();
    bus.BeginCpuInstruction();
    (void)bus.CpuFetch32(0x08000000u);
    assert(bus.EndCpuInstruction() == 3u); // 3N + 1S, minus baseline 1.
    bus.BeginCpuInstruction();
    (void)bus.CpuFetch32(0x08000004u);
    assert(bus.EndCpuInstruction() == 1u); // sequential 1S + 1S.
    bus.Write16(0x04000204u, 0x0000u);

    // With prefetch enabled, an EWRAM access gives the cartridge bus time to
    // stage later ROM halfwords. The following non-sequential CPU fetch then
    // consumes the staged data instead of paying a fresh ROM N access.
    bus.Write16(0x04000204u, 0x4000u);
    bus.ResetCpuTiming();
    bus.BeginCpuInstruction();
    (void)bus.CpuFetch32(0x08000000u);
    assert(bus.EndCpuInstruction() == 5u); // Initial 4N + 2S miss.
    bus.BeginCpuInstruction();
    (void)bus.CpuRead32(0x02000000u);
    assert(bus.EndCpuInstruction() == 4u); // Fills three 2-cycle halfwords.
    bus.BeginCpuInstruction();
    (void)bus.CpuFetch32(0x08000004u);
    assert(bus.EndCpuInstruction() == 1u); // Two prefetched halfwords.

    bus.Write16(0x04000204u, 0x0000u);
    bus.ResetCpuTiming();
    bus.BeginCpuInstruction();
    (void)bus.CpuFetch32(0x08000000u);
    assert(bus.EndCpuInstruction() == 5u);
    bus.BeginCpuInstruction();
    (void)bus.CpuRead32(0x02000000u);
    assert(bus.EndCpuInstruction() == 4u);
    bus.BeginCpuInstruction();
    (void)bus.CpuFetch32(0x08000004u);
    assert(bus.EndCpuInstruction() == 5u); // No buffer: fresh 4N + 2S.

    ngba::Arm7Tdmi cpu(bus);
    cpu.Reset(0x080000D0u);
    cpu.State().r[1] = 0x02000000u;
    cpu.State().r[2] = 0x08000101u;

    cpu.Step();
    assert(cpu.State().r[0] == 1);
    cpu.Step();
    assert(cpu.State().r[0] == 3);
    cpu.Step();
    assert(bus.Read32(0x02000000u) == 3);
    cpu.Step();
    assert(cpu.IsThumb());
    assert(cpu.State().r[15] == 0x08000100u);
    cpu.Step();
    assert(cpu.State().r[3] == 7);
    cpu.Step();
    assert(cpu.State().r[3] == 8);
    cpu.State().r[4] = 5;
    cpu.Step();
    assert(cpu.State().r[0] == 5);
    assert(cpu.State().r[15] == 0x08000106u);
    cpu.Step();
    assert(cpu.State().r[15] == 0x08000106u);

    cpu.Reset(0x080000E0u);
    cpu.State().r[1] = 0x02000000u;
    cpu.State().r[2] = 0xCAFEBABEu;
    bus.Write32(0x02000000u, 0x12345678u);
    cpu.Step();
    assert(cpu.State().r[0] == 0x12345678u);
    assert(bus.Read32(0x02000000u) == 0xCAFEBABEu);

    for (std::uint32_t index = 0; index < 80u; ++index) {
        bus.Write32(0x02000100u + index * 4u, 0xA5000000u + index);
        bus.Write32(0x02000400u + index * 4u, 0);
    }
    cpu.Reset(0x08000280u);
    cpu.State().r[0] = 0x02000100u;
    cpu.State().r[1] = 0x02000400u;
    cpu.State().r[2] = 9u;
    cpu.Step();
    for (std::uint32_t index = 0; index < 16u; ++index) {
        assert(bus.Read32(0x02000400u + index * 4u) == 0xA5000000u + index);
    }
    assert(bus.Read32(0x02000440u) == 0);

    assert(bus.VCount() == 0);
    bus.Tick(1232);
    assert(bus.VCount() == 1);
    assert(bus.Read16(0x04000006u) == 1);

    // DMA3 immediate 32-bit transfer: source in EWRAM, destination in IWRAM.
    bus.Write32(0x02000000u, 0x12345678u);
    bus.Write32(0x040000D4u, 0x02000000u);
    bus.Write32(0x040000D8u, 0x03000000u);
    bus.Write16(0x040000DCu, 1);
    bus.Write16(0x040000DEu, 0x8400u);
    assert(bus.DmaActive());
    bus.AdvanceTo(bus.NextEventCycle());
    assert(bus.Read32(0x03000000u) == 0x12345678u);
    assert(!bus.DmaActive());

    // Immediate DMA is incremental: each unit becomes visible only after
    // its own bus event, and the CPU cannot retire while the bus is owned.
    ngba::MemoryBus dma_bus(rom);
    ngba::Arm7Tdmi dma_cpu(dma_bus);
    dma_bus.Write16(0x02000000u, 0x1111u);
    dma_bus.Write16(0x02000002u, 0x2222u);
    dma_bus.Write16(0x02000004u, 0x3333u);
    dma_bus.Write32(0x040000B0u, 0x02000000u);
    dma_bus.Write32(0x040000B4u, 0x03000000u);
    dma_bus.Write16(0x040000B8u, 3);
    dma_bus.Write16(0x040000BAu, 0x8000u);
    assert(dma_bus.DmaActive());
    assert(dma_bus.Read16(0x03000000u) == 0);

    dma_cpu.Reset(0x080000D0u);
    const ngba::StepResult dma_stall = dma_cpu.StepTimed();
    assert(!dma_stall.retired);
    assert(dma_cpu.Steps() == 0);
    assert(dma_bus.Read16(0x03000000u) == 0x1111u);
    assert(dma_bus.Read16(0x03000002u) == 0);
    assert(dma_bus.DmaActive());

    dma_bus.AdvanceTo(dma_bus.NextEventCycle());
    assert(dma_bus.Read16(0x03000002u) == 0x2222u);
    assert(dma_bus.Read16(0x03000004u) == 0);
    dma_bus.AdvanceTo(dma_bus.NextEventCycle());
    assert(dma_bus.Read16(0x03000004u) == 0x3333u);
    assert(!dma_bus.DmaActive());
    assert(dma_bus.DmaTransfers() == 1);
    assert(dma_bus.DmaCycles() == 6);

    // RegisterRamReset must preserve the BIOS-owned top 0x200 bytes of
    // IWRAM, including the SoftReset selector, while clearing game IWRAM.
    bus.Write8(0x03000000u, 0xA5u);
    bus.Write8(0x03007E00u, 0x5Au);
    bus.Write8(0x03007FFAu, 0x01u);
    bus.ResetRam(0x02u);
    assert(bus.Read8(0x03000000u) == 0);
    assert(bus.Read8(0x03007E00u) == 0x5Au);
    assert(bus.Read8(0x03007FFAu) == 0x01u);
    assert(bus.Read16(0x04000130u) == 0x03FFu);

    cpu.Reset(0x08000140u);
    cpu.State().r[0] = 0x00000092u;
    cpu.State().r[1] = 0x03007FA0u;
    cpu.State().r[2] = 0x0000003Fu;
    cpu.State().r[13] = 0x03007E44u;
    bus.Write32(0x03007FFCu, 0x08000180u);
    bus.Write16(0x04000004u, 0x0008u); // VBlank IRQ source
    bus.Write16(0x04000200u, 0x0001u);
    bus.Write16(0x04000208u, 0x0000u); // enable after switching to Thumb
    bus.Tick(1232u * 159u); // advance from VCOUNT 1 to VCOUNT 160

    cpu.Step(); // switch to IRQ mode
    cpu.Step(); // set SP_irq
    cpu.Step(); // switch to System+Thumb
    bus.Write16(0x04000208u, 0x0001u);
    cpu.Step(); // Thumb NOP, then take IRQ
    assert(cpu.State().r[15] == 0x00000018u);
    assert((cpu.State().cpsr & 0x1Fu) == ngba::Arm7Tdmi::kModeIrq);

    for (int i = 0; i < 11; ++i) cpu.Step();
    assert(cpu.IsThumb());
    assert((cpu.State().cpsr & 0x1Fu) == ngba::Arm7Tdmi::kModeSystem);
    assert(cpu.State().r[15] == 0x0800014Eu);
    assert(cpu.State().r[13] == 0x03007E44u);
    assert(bus.Read16(0x04000202u) == 0);

    // HALTCNT is a write command, including when reached through a wider
    // access whose byte lanes include 0x04000301.
    bus.Write16(0x04000300u, 0x0000u);
    assert(bus.ConsumePowerRequest() == ngba::PowerRequest::Halt);
    bus.Write32(0x040002FFu, 0x00800000u);
    assert(bus.ConsumePowerRequest() == ngba::PowerRequest::Stop);

    cpu.Reset(0x08000220u);
    cpu.State().cpsr = ngba::Arm7Tdmi::kModeSystem |
                       ngba::Arm7Tdmi::kCpsrT;
    bus.Write16(0x04000200u, 0);
    cpu.Step();
    assert(cpu.IsHalted());
    assert(cpu.BiosSoftwareInterrupts() == 0);
    assert(cpu.State().r[15] == 0x08000222u);

    // External BIOS SWI fixture. The vector branches to a tiny ARM routine
    // that writes HALTCNT and returns with MOVS PC,LR. This proves the ARM
    // SWI encoding, 0x08 entry, SVC LR, real halt handshake and return state
    // without depending on the full dump for a unit test.
    const std::string bios_path = "ngba_native_swi_test_bios.bin";
    std::vector<std::uint8_t> bios(0x4000, 0);
    WriteLe32(bios, 0x08, 0xEA00000Cu); // B 0x40
    WriteLe32(bios, 0x40, 0xE3A00000u); // MOV r0,#0
    WriteLe32(bios, 0x44, 0xE59F1004u); // LDR r1,[pc,#4]
    WriteLe32(bios, 0x48, 0xE5C10000u); // STRB r0,[r1]
    WriteLe32(bios, 0x4C, 0xE1B0F00Eu); // MOVS pc,lr
    WriteLe32(bios, 0x50, 0x04000301u); // HALTCNT
    {
        std::ofstream output(bios_path.c_str(),
                             std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bios.data()),
                     static_cast<std::streamsize>(bios.size()));
    }

    ngba::MemoryBus native_bus(rom, bios_path);
    ngba::Arm7Tdmi native_cpu(native_bus);
    native_bus.Write16(0x04000004u, 0x0008u); // VBlank IRQ source
    native_bus.Write16(0x04000200u, 0x0001u);
    native_cpu.Reset(0x08000200u);
    native_cpu.Step(); // ARM SWI enters SVC through 0x08.
    assert(native_cpu.BiosSoftwareInterrupts() == 1);
    assert(native_cpu.State().r[15] == 0x00000008u);
    assert(native_cpu.State().r[14] == 0x08000204u);
    assert((native_cpu.State().cpsr & 0x1Fu) == ngba::Arm7Tdmi::kModeSupervisor);
    assert((native_cpu.State().cpsr & ngba::Arm7Tdmi::kCpsrT) == 0);
    assert((native_cpu.State().cpsr & ngba::Arm7Tdmi::kCpsrI) != 0);
    native_cpu.Step(); // branch to the test BIOS routine.
    native_cpu.Step(); // MOV r0,#0
    native_cpu.Step(); // LDR r1,=HALTCNT
    native_cpu.Step(); // STRB -> HALTED
    assert(native_cpu.IsHalted());
    assert(native_cpu.HaltEntries() == 1);
    assert(native_bus.Cycles() == 17);
    assert(native_cpu.CpuExecutionCycles() == 17);
    native_bus.Tick(1232u * 160u);
    assert(native_bus.InterruptRequested());
    assert(!native_bus.InterruptPending()); // IME is still zero.
    native_cpu.Step(); // wake without accepting an IRQ
    assert(!native_cpu.IsHalted());
    assert(native_cpu.HaltWakeups() == 1);
    assert(native_bus.Read16(0x04000202u) == 1); // wake does not clear IF
    native_cpu.Step(); // MOVS PC,LR returns to the ARM caller.
    assert((native_cpu.State().cpsr & 0x1Fu) == ngba::Arm7Tdmi::kModeSystem);
    assert(native_cpu.State().r[15] == 0x08000204u);

    // The same native gate must select IntrWait and VBlankIntrWait, while
    // their actual blocking behavior remains the responsibility of the BIOS.
    auto RunNativeWaitService = [&](std::uint32_t start) {
        ngba::MemoryBus wait_bus(rom, bios_path);
        ngba::Arm7Tdmi wait_cpu(wait_bus);
        wait_bus.Write16(0x04000004u, 0x0008u);
        wait_bus.Write16(0x04000200u, 0x0001u);
        wait_cpu.Reset(start);
        wait_cpu.Step();
        assert(wait_cpu.BiosSoftwareInterrupts() == 1);
        wait_cpu.Step();
        wait_cpu.Step();
        wait_cpu.Step();
        wait_cpu.Step();
        assert(wait_cpu.IsHalted());
        wait_bus.Tick(1232u * 160u);
        wait_cpu.Step();
        assert(!wait_cpu.IsHalted());
        wait_cpu.Step();
        assert(wait_cpu.State().r[15] == start + 4u);
    };
    RunNativeWaitService(0x08000240u);
    RunNativeWaitService(0x08000244u);

    // A cycle-bounded run must include Halt time without retiring synthetic
    // instructions. Running instructions remain atomic, so a ROM wait-state
    // stall may legitimately carry the final step past the deadline.
    ngba::MemoryBus budget_bus(rom, bios_path);
    ngba::Arm7Tdmi budget_cpu(budget_bus);
    budget_bus.Write16(0x04000004u, 0x0008u);
    budget_bus.Write16(0x04000200u, 0x0001u);
    budget_cpu.Reset(0x08000200u);
    const ngba::RunResult cycle_result =
        budget_cpu.RunForCycles(1232u * 160u + 10u);
    assert(cycle_result.reason == ngba::RunStopReason::CycleBudget);
    const ngba::Cycle cycle_deadline = 1232u * 160u + 10u;
    assert(cycle_result.elapsed_cycles >= cycle_deadline);
    assert(cycle_result.overshoot ==
           cycle_result.elapsed_cycles - cycle_deadline);
    assert(cycle_result.frames_published == 1);
    assert(cycle_result.retired_instructions == budget_cpu.Steps());
    assert(budget_cpu.HaltCycles() > 1232u);
    assert(budget_cpu.CpuExecutionCycles() + budget_cpu.HaltCycles() ==
           cycle_result.elapsed_cycles);

    // A running instruction is atomic in this stage: a deadline may be
    // crossed, but its cycles are never clipped artificially.
    ngba::MemoryBus overshoot_bus(rom, bios_path);
    ngba::Arm7Tdmi overshoot_cpu(overshoot_bus);
    overshoot_cpu.Reset(0x08000200u);
    const ngba::StepResult first_step = overshoot_cpu.StepTimed();
    assert(first_step.retired);
    assert(first_step.cycles == 8); // SWI plus the initial ROM fetch.
    assert(overshoot_bus.Cycles() == 8);
    overshoot_cpu.Reset(0x08000200u);
    const ngba::RunResult overshoot_result = overshoot_cpu.RunForCycles(1);
    assert(overshoot_result.elapsed_cycles == 8);
    assert(overshoot_result.overshoot == 7);
    assert(overshoot_result.retired_instructions == 1);

    // Frame completion is a PPU event, not a count of VBlankIntrWait returns.
    ngba::MemoryBus frame_bus(rom, bios_path);
    ngba::Arm7Tdmi frame_cpu(frame_bus);
    frame_cpu.Reset(0x08000200u);
    const ngba::RunResult frame_result = frame_cpu.RunUntilFrameReady(1);
    assert(frame_result.reason == ngba::RunStopReason::FrameReady);
    assert(frame_result.elapsed_cycles == 1232u * 160u);
    assert(frame_result.frames_published == 1);
    assert(frame_bus.Frames() == 1);
    assert(frame_cpu.IsHalted());

    ngba::MemoryBus thumb_bus(rom, bios_path);
    ngba::Arm7Tdmi thumb_cpu(thumb_bus);
    thumb_bus.Write16(0x04000004u, 0x0008u);
    thumb_bus.Write16(0x04000200u, 0x0001u);
    thumb_cpu.Reset(0x08000220u);
    thumb_cpu.State().cpsr = ngba::Arm7Tdmi::kModeSystem |
                             ngba::Arm7Tdmi::kCpsrT |
                             ngba::Arm7Tdmi::kCpsrN |
                             ngba::Arm7Tdmi::kCpsrC;
    thumb_cpu.Step();
    assert(thumb_cpu.State().r[15] == 0x00000008u);
    assert(thumb_cpu.State().r[14] == 0x08000222u);
    thumb_cpu.Step();
    thumb_cpu.Step();
    thumb_cpu.Step();
    thumb_cpu.Step();
    assert(thumb_cpu.IsHalted());
    thumb_bus.Tick(1232u * 160u);
    thumb_cpu.Step();
    thumb_cpu.Step();
    assert((thumb_cpu.State().cpsr & 0x1Fu) == ngba::Arm7Tdmi::kModeSystem);
    assert((thumb_cpu.State().cpsr & ngba::Arm7Tdmi::kCpsrT) != 0);
    assert((thumb_cpu.State().cpsr & (ngba::Arm7Tdmi::kCpsrN |
                                      ngba::Arm7Tdmi::kCpsrC)) ==
           (ngba::Arm7Tdmi::kCpsrN | ngba::Arm7Tdmi::kCpsrC));
    assert(thumb_cpu.State().r[15] == 0x08000222u);

    std::remove(bios_path.c_str());
    std::remove(path.c_str());
    return 0;
}
