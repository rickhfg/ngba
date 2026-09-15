#include "ngba/runtime.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>

int main() {
    const auto write = [](const char* name, const std::vector<std::uint8_t>& bytes) {
        std::ofstream output(name, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        assert(output.good());
    };
    std::vector<std::uint8_t> bytes(512, 0);
    bytes[0] = 0x32; bytes[3] = 0xEA; bytes[0xB2] = 0x96;
    write("bios-policy.gba", bytes);
    const auto rom = ngba::RomImage::Load("bios-policy.gba");
    std::vector<std::uint8_t> bios(16384, 0);
    bios[8] = 0x0E; bios[9] = 0xF0; bios[10] = 0xB0; bios[11] = 0xE1;
    write("bios-policy.bin", bios);
    ngba::MemoryBus bus(rom, "bios-policy.bin");
    ngba::Arm7Tdmi cpu(bus);
    assert(cpu.GetBiosMode() == ngba::BiosMode::External);
    for (const auto mode : {ngba::BiosMode::External, ngba::BiosMode::Hle, ngba::BiosMode::Hybrid}) {
        for (const bool thumb : {false, true}) {
            for (unsigned service = 0; service < 256; ++service) {
                cpu.SetBiosMode(mode);
                cpu.Reset(0x03000000u);
                if (thumb) cpu.State().cpsr |= ngba::Arm7Tdmi::kCpsrT;
                bus.Write32(0x03000000u, thumb ? (0xDF00u | service) : (0xEF000000u | (service << 16)));
                const bool external = mode == ngba::BiosMode::External ||
                    (mode == ngba::BiosMode::Hybrid && (service == 2 || service == 4 || service == 5));
                bool failed = false;
                try { cpu.Step(); } catch (const ngba::CpuError&) { failed = true; }
                assert(cpu.BiosSoftwareInterrupts() == (external ? 1u : 0u));
                if (external) {
                    assert(!failed);
                    assert(cpu.State().r[15] == 8);
                    assert(cpu.State().r[14] == 0x03000000u + (thumb ? 2 : 4));
                    assert((cpu.State().cpsr & 0xBFu) == 0x93u);
                    cpu.Step();
                    assert(cpu.State().r[15] == 0x03000000u + (thumb ? 2 : 4));
                    assert(cpu.IsThumb() == thumb);
                }
                if (!external && (service == 3 || service == 4 || service == 5)) assert(failed);
            }
        }
    }
    ngba::MemoryBus no_bios(rom);
    ngba::Arm7Tdmi hle(no_bios);
    assert(hle.GetBiosMode() == ngba::BiosMode::Hle);
    for (const auto mode : {ngba::BiosMode::External, ngba::BiosMode::Hybrid, static_cast<ngba::BiosMode>(255)}) {
        bool rejected = false;
        try { hle.SetBiosMode(mode); } catch (const ngba::CpuError&) { rejected = true; }
        assert(rejected && hle.GetBiosMode() == ngba::BiosMode::Hle);
    }
    ngba::Runtime runtime("bios-policy.gba", "bios-policy.bin");
    runtime.Cpu().SetBiosMode(ngba::BiosMode::Hybrid);
    runtime.SaveState("bios-policy.ngbs");
    runtime.Cpu().SetBiosMode(ngba::BiosMode::External);
    runtime.LoadState("bios-policy.ngbs");
    assert(runtime.Cpu().GetBiosMode() == ngba::BiosMode::Hybrid);
    for (const char* file : {"bios-policy.gba", "bios-policy.bin", "bios-policy.ngbs"}) std::remove(file);
}
