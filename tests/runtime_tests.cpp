#include "ngba/runtime.hpp"

#include <cassert>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>

namespace {

void WriteRom(const std::string& path, bool flash = true) {
    std::vector<std::uint8_t> bytes(512, 0);
    bytes[0] = 0x32; bytes[3] = 0xEA;
    bytes[0xB2] = 0x96;
    bytes[0xD0] = 1; bytes[0xD2] = 0xA0; bytes[0xD3] = 0xE3;
    bytes[0xD4] = 0xFD; bytes[0xD5] = 0xFF; bytes[0xD6] = 0xFF; bytes[0xD7] = 0xEA;
    if (flash) {
        const std::string marker = "FLASH1M_V103";
        std::copy(marker.begin(), marker.end(), bytes.begin() + 0x180);
    }
    std::ofstream output(path.c_str(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<std::uint8_t> ReadFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void Command(ngba::MemoryBus& bus, std::uint8_t command) {
    bus.Write8(0x0E005555, 0xAA);
    bus.Write8(0x0E002AAA, 0x55);
    bus.Write8(0x0E005555, command);
}

void TestFlash(ngba::MemoryBus& bus) {
    assert(bus.Read8(0x0E000000) == 0xFF);
    bus.Write8(0x0E000000, 0x90);
    assert(bus.Read8(0x0E000000) == 0xFF);
    Command(bus, 0x90);
    assert(bus.Read8(0x0E000000) == 0xC2);
    assert(bus.Read8(0x0E000001) == 9);
    bus.Write8(0x0E000000, 0xF0);
    Command(bus, 0xA0); bus.Write8(0x0E001234, 0x52);
    assert(bus.Read8(0x0E001234) == 0x52);
    Command(bus, 0xA0); bus.Write8(0x0E001234, 0xFF);
    assert(bus.Read8(0x0E001234) == 0x52);
    Command(bus, 0xB0); bus.Write8(0x0E000000, 1);
    assert(bus.Read8(0x0E001234) == 0xFF);
    Command(bus, 0xA0); bus.Write8(0x0E001234, 0xF0);
    assert(bus.Read8(0x0E001234) == 0xF0);
    Command(bus, 0x80);
    bus.Write8(0x0E005555, 0xAA); bus.Write8(0x0E002AAA, 0x55); bus.Write8(0x0E001000, 0x30);
    assert(bus.Read8(0x0E001234) == 0xFF);
    Command(bus, 0xB0); bus.Write8(0x0E000000, 0);
    assert(bus.Read8(0x0E001234) == 0x52);
    Command(bus, 0x80); Command(bus, 0x10);
    assert(bus.Read8(0x0E001234) == 0xFF);
}

void TestDecoder(ngba::MemoryBus& bus, ngba::Arm7Tdmi& cpu) {
    for (unsigned operation = 0; operation < 8; ++operation) {
        bus.Write32(0x02002000, 0xFEDCBA98);
        bus.Write16(0x03000000, static_cast<std::uint16_t>(0x5000 | (operation << 9) | (2 << 6) | (1 << 3)));
        cpu.Reset(0x03000000);
        cpu.State().cpsr |= ngba::Arm7Tdmi::kCpsrT;
        cpu.State().r[0] = 0x12345678; cpu.State().r[1] = 0x02002000; cpu.State().r[2] = 0;
        cpu.Step();
        const std::uint32_t expected[] = {0x12345678, 0xFEDC5678, 0xFEDCBA78, 0xFFFFFF98,
                                         0xFEDCBA98, 0xBA98, 0x98, 0xFFFFBA98};
        assert((operation < 3 ? bus.Read32(0x02002000) : cpu.State().r[0]) == expected[operation]);
    }
    bus.Write16(0x03000000, 0xC803);
    bus.Write32(0x02002000, 0x1234); bus.Write32(0x02002004, 0x5678);
    cpu.Reset(0x03000000); cpu.State().cpsr |= ngba::Arm7Tdmi::kCpsrT;
    cpu.State().r[0] = 0x02002000;
    cpu.Step();
    assert(cpu.State().r[0] == 0x1234 && cpu.State().r[1] == 0x5678);
    bus.Write16(0x03000000, 0xDF12);
    bus.Write32(0x02002000, 0x00000910);
    const std::uint8_t compressed[] = {0x10, 'A', 'B', 'C', 0x30, 0x02};
    for (unsigned index = 0; index < sizeof(compressed); ++index) bus.Write8(0x02002004 + index, compressed[index]);
    cpu.Reset(0x03000000); cpu.State().cpsr |= ngba::Arm7Tdmi::kCpsrT;
    cpu.State().r[0] = 0x02002000; cpu.State().r[1] = 0x06000000;
    cpu.Step();
    for (unsigned index = 0; index < 9; ++index) assert(bus.Read8(0x06000000 + index) == "ABC"[index % 3]);
}

void TestDivision(ngba::MemoryBus& bus, ngba::Arm7Tdmi& cpu) {
    struct DivisionCase {
        std::uint32_t numerator;
        std::uint32_t denominator;
        std::uint32_t quotient;
        std::uint32_t remainder;
        std::uint32_t magnitude;
    };
    const DivisionCase cases[] = {
        {0u, 0u, 1u, 0u, 1u},
        {1u, 0u, 1u, 1u, 1u},
        {0xFFFFFFFFu, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu, 1u},
        {2u, 0u, 1u, 2u, 1u},
        {0xFFFFFFFEu, 0u, 0xFFFFFFFFu, 0xFFFFFFFEu, 1u},
        {0x7FFFFFFFu, 0u, 1u, 0x7FFFFFFFu, 1u},
        {0x80000000u, 0u, 0xFFFFFFFFu, 0x80000000u, 1u},
        {0x80000000u, 0xFFFFFFFFu, 0x80000000u, 0u, 0x80000000u},
        {0x80000000u, 1u, 0x80000000u, 0u, 0x80000000u},
        {7u, 3u, 2u, 1u, 2u},
        {0xFFFFFFF9u, 3u, 0xFFFFFFFEu, 0xFFFFFFFFu, 2u},
        {7u, 0xFFFFFFFDu, 0xFFFFFFFEu, 1u, 2u},
        {0xFFFFFFF9u, 0xFFFFFFFDu, 2u, 0xFFFFFFFFu, 2u},
        {0u, 0xFFFFFFFFu, 0u, 0u, 0u},
        {1u, 0x80000000u, 0u, 1u, 0u},
        {0x80000000u, 0x80000000u, 1u, 0u, 1u},
    };
    for (const bool thumb : {false, true}) {
        for (const unsigned service : {6u, 7u}) {
            if (thumb) bus.Write16(0x03000000u, static_cast<std::uint16_t>(0xDF00u | service));
            else bus.Write32(0x03000000u, 0xEF000000u | (service << 16));
            for (const auto& test : cases) {
                cpu.Reset(0x03000000u);
                if (thumb) cpu.State().cpsr |= ngba::Arm7Tdmi::kCpsrT;
                cpu.State().r[0] = service == 6u ? test.numerator : test.denominator;
                cpu.State().r[1] = service == 6u ? test.denominator : test.numerator;
                cpu.State().r[3] = 0xDEADBEEFu;
                cpu.Step();
                assert(cpu.State().r[0] == test.quotient);
                assert(cpu.State().r[1] == test.remainder);
                assert(cpu.State().r[3] == test.magnitude);
            }
        }
    }
}

void TestRegisterRotate(ngba::MemoryBus& bus, ngba::Arm7Tdmi& cpu) {
    const std::uint32_t instructions[] = {0xE1B00271u, 0xE1A00271u, 0x41C8u};
    const std::uint32_t amounts[] = {
        0u, 1u, 31u, 32u, 33u, 63u, 64u, 96u, 128u, 160u, 192u, 224u,
        255u, 256u, 257u, 288u, 511u, 512u, 0xFFFFFF00u, 0xFFFFFFFFu,
    };
    const std::uint32_t operands[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu};
    for (unsigned format = 0; format < 3; ++format) {
        const bool thumb = format == 2;
        bus.Write32(0x03000000u, instructions[format]);
        for (const auto amount : amounts) {
            for (const auto operand : operands) {
                for (unsigned flags = 0; flags < 16; ++flags) {
                    cpu.Reset(0x03000000u);
                    const std::uint32_t initial_status = ngba::Arm7Tdmi::kModeSystem |
                        (thumb ? ngba::Arm7Tdmi::kCpsrT : 0u) | (flags << 28);
                    cpu.State().cpsr = initial_status;
                    cpu.State().r[0] = thumb ? operand : 0xDEADBEEFu;
                    cpu.State().r[1] = thumb ? amount : operand;
                    cpu.State().r[2] = amount;
                    std::uint32_t expected = operand;
                    bool expected_carry = (initial_status & ngba::Arm7Tdmi::kCpsrC) != 0;
                    for (unsigned shift = 0; shift < (amount & 255u); ++shift) {
                        expected_carry = (expected & 1u) != 0;
                        expected = (expected >> 1) | (expected_carry ? 0x80000000u : 0u);
                    }
                    std::uint32_t expected_status = initial_status;
                    if (format != 1) {
                        expected_status &= ~(ngba::Arm7Tdmi::kCpsrN | ngba::Arm7Tdmi::kCpsrZ | ngba::Arm7Tdmi::kCpsrC);
                        if ((expected & 0x80000000u) != 0) expected_status |= ngba::Arm7Tdmi::kCpsrN;
                        if (expected == 0) expected_status |= ngba::Arm7Tdmi::kCpsrZ;
                        if (expected_carry) expected_status |= ngba::Arm7Tdmi::kCpsrC;
                    }
                    cpu.Step();
                    assert(cpu.State().r[0] == expected);
                    assert(cpu.State().cpsr == expected_status);
                    assert(cpu.State().r[1] == (thumb ? amount : operand));
                    assert(cpu.State().r[2] == amount);
                }
            }
        }
    }
}

void TestNative(const ngba::RomImage& rom) {
    if (!ngba::NativeBackend::Available()) return;
    ngba::MemoryBus reference_bus(rom), translated_bus(rom);
    ngba::Arm7Tdmi reference(reference_bus), translated(translated_bus);
    translated.EnableNative(true);
    std::mt19937 random(0xBADC0DE);
    for (unsigned trial = 0; trial < 4000; ++trial) {
        const bool thumb = trial < 3000;
        const unsigned operation = trial % (thumb ? 4 : 3);
        const unsigned destination = random() % 8;
        const std::uint32_t instruction = thumb
            ? 0x2000 | (operation << 11) | (destination << 8) | (random() & 255)
            : (operation == 0 ? 0xE3A00000u : operation == 1 ? 0xE2800000u : 0xE2400000u) |
              (destination << 12) | ((random() % 8) << 16) | (random() & 0xFFF);
        reference_bus.Write32(0x03000000, instruction);
        translated_bus.Write32(0x03000000, instruction);
        reference.Reset(0x03000000); translated.Reset(0x03000000);
        reference.State().cpsr = ngba::Arm7Tdmi::kModeSystem | (thumb ? 32 : 0) | (random() & 0xF0000000);
        for (unsigned index = 0; index < 15; ++index) reference.State().r[index] = random();
        translated.State() = reference.State();
        const auto expected = reference.StepTimed();
        const auto actual = translated.StepTimed();
        assert(translated.TranslatedInstructions() == 1);
        assert(actual.cycles == expected.cycles);
        for (unsigned index = 0; index < 16; ++index) assert(reference.State().r[index] == translated.State().r[index]);
        assert(reference.State().cpsr == translated.State().cpsr);
    }
    for (unsigned operation = 0; operation < 4; ++operation) {
        for (const std::uint32_t initial : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
            for (const unsigned immediate : {0u, 1u, 127u, 255u}) {
                for (unsigned flags = 0; flags < 16; ++flags) {
                    const std::uint16_t instruction = static_cast<std::uint16_t>(0x2000 | (operation << 11) | immediate);
                    reference_bus.Write16(0x03000000, instruction);
                    translated_bus.Write16(0x03000000, instruction);
                    reference.Reset(0x03000000); translated.Reset(0x03000000);
                    reference.State().cpsr = ngba::Arm7Tdmi::kModeSystem | 32u | (flags << 28);
                    reference.State().r[0] = initial;
                    translated.State() = reference.State();
                    reference.Step(); translated.Step();
                    assert(translated.TranslatedInstructions() == 1);
                    assert(reference.State().r[0] == translated.State().r[0]);
                    assert(reference.State().cpsr == translated.State().cpsr);
                }
            }
        }
    }
    reference_bus.Write16(0x03000000, 0xE7FE);
    translated_bus.Write16(0x03000000, 0xE7FE);
    reference.Reset(0x03000000); translated.Reset(0x03000000);
    reference.State().cpsr |= ngba::Arm7Tdmi::kCpsrT;
    translated.State() = reference.State();
    reference.Step(); translated.Step();
    assert(translated.TranslatedInstructions() == 0);
    assert(reference.State().r[15] == translated.State().r[15]);
}

void TestState(const std::string& path) {
    ngba::Runtime runtime(path, "", ngba::NativeBackend::Available());
    runtime.Bus().Write16(0x04000100, 0xFFE0);
    runtime.Bus().Write16(0x04000102, 0x0080);
    runtime.RunForCycles(1234);
    runtime.SetPaused(true);
    const auto paused_cycle = runtime.Bus().Cycles();
    runtime.RunForCycles(50000);
    assert(runtime.Bus().Cycles() == paused_cycle);
    runtime.Bus().Write32(0x02000000, 0xABCDEF);
    runtime.Bus().Write32(0x040000D4, 0x02000000);
    runtime.Bus().Write32(0x040000D8, 0x03001000);
    runtime.Bus().Write16(0x040000DC, 4);
    runtime.Bus().Write16(0x040000DE, 0x8400);
    runtime.SaveState("runtime-before.ngbs");
    runtime.SetPaused(false);
    runtime.RunForCycles(5000);
    runtime.SaveState("runtime-after.ngbs");
    runtime.LoadState("runtime-before.ngbs");
    runtime.RunForCycles(5000);
    runtime.SaveState("runtime-replayed.ngbs");
    assert(ReadFile("runtime-after.ngbs") == ReadFile("runtime-replayed.ngbs"));
    runtime.SetPaused(true);
    const auto before = runtime.Bus().Frames();
    runtime.StepFrame();
    assert(runtime.Paused() && runtime.Bus().Frames() == before + 1);
    auto bytes = ReadFile("runtime-after.ngbs");
    bytes.back() ^= 1;
    {
        std::ofstream output("runtime-corrupt.ngbs", std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    const auto cycles = runtime.Bus().Cycles();
    bool rejected = false;
    try { runtime.LoadState("runtime-corrupt.ngbs"); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && runtime.Bus().Cycles() == cycles);
    {
        std::ofstream output("runtime-truncated.ngbs", std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), 100);
    }
    rejected = false;
    try { runtime.LoadState("runtime-truncated.ngbs"); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && runtime.Bus().Cycles() == cycles);
    std::remove("runtime-truncated.ngbs");
    WriteRom("runtime-other.gba", false);
    ngba::Runtime other("runtime-other.gba");
    rejected = false;
    try { other.LoadState("runtime-before.ngbs"); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    runtime.LoadState("runtime-before.ngbs");
    assert(runtime.Bus().DmaActive());
    runtime.SetPaused(false);
    runtime.RunForCycles(1000);
    runtime.Bus().RequestPower(ngba::PowerRequest::Halt);
    runtime.Cpu().Step();
    assert(runtime.Cpu().IsHalted());
    runtime.SaveState("runtime-halted.ngbs");
    runtime.RunForCycles(10000);
    runtime.SaveState("runtime-halt-after.ngbs");
    runtime.LoadState("runtime-halted.ngbs");
    assert(runtime.Cpu().IsHalted());
    runtime.RunForCycles(10000);
    runtime.SaveState("runtime-halt-replayed.ngbs");
    assert(ReadFile("runtime-halt-after.ngbs") == ReadFile("runtime-halt-replayed.ngbs"));
    for (const auto file : {"runtime-halted.ngbs", "runtime-halt-after.ngbs", "runtime-halt-replayed.ngbs"}) std::remove(file);
    for (const auto file : {"runtime-before.ngbs", "runtime-after.ngbs", "runtime-replayed.ngbs", "runtime-corrupt.ngbs", "runtime-other.gba"}) std::remove(file);
}

}

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string bios = argc > 2 ? argv[2] : "";
        ngba::Runtime reference(argv[1], bios, false);
        ngba::Runtime translated(argv[1], bios, true);
        for (unsigned checkpoint = 0; checkpoint < 5; ++checkpoint) {
            reference.RunForCycles(10000000);
            translated.RunForCycles(10000000);
            reference.SaveState("rom-reference.ngbs");
            translated.SaveState("rom-translated.ngbs");
            assert(ReadFile("rom-reference.ngbs") == ReadFile("rom-translated.ngbs"));
        }
        assert(translated.Cpu().TranslatedInstructions() > 0);
        translated.LoadState("rom-translated.ngbs");
        reference.RunForCycles(1000000);
        translated.RunForCycles(1000000);
        reference.SaveState("rom-reference.ngbs");
        translated.SaveState("rom-translated.ngbs");
        assert(ReadFile("rom-reference.ngbs") == ReadFile("rom-translated.ngbs"));
        for (const auto file : {"rom-reference.ngbs", "rom-translated.ngbs"}) std::remove(file);
        return 0;
    }
    const std::string path = "runtime-test.gba";
    WriteRom(path);
    const auto rom = ngba::RomImage::Load(path);
    ngba::MemoryBus bus(rom);
    ngba::Arm7Tdmi cpu(bus);
    TestFlash(bus);
    TestDecoder(bus, cpu);
    TestDivision(bus, cpu);
    TestRegisterRotate(bus, cpu);
    TestNative(rom);
    TestState(path);
    std::remove(path.c_str());
}
