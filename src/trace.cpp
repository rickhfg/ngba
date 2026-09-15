#include "ngba/arm7.hpp"

#include "ngba/rom.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

std::uint64_t ParseCount(const char* text) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
        throw std::runtime_error("instruction count must be a positive integer");
    }
    return static_cast<std::uint64_t>(value);
}

void PrintHex(std::uint32_t value) {
    std::cout << "0x" << std::uppercase << std::hex << std::setw(8)
              << std::setfill('0') << value << std::dec << std::setfill(' ');
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <game.gba> [instructions] [bios.bin] [--bios-boot] [--bios-mode external|hle|hybrid]\n";
        return 2;
    }

    try {
        const ngba::RomImage rom = ngba::RomImage::Load(argv[1]);
        std::string bios_path;
        bool bios_boot = false;
        std::string bios_mode;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--bios-boot") {
                bios_boot = true;
            } else if (std::string(argv[i]) == "--bios-mode") {
                if (++i >= argc) throw std::runtime_error("--bios-mode requires a value");
                bios_mode = argv[i];
            } else if (bios_path.empty()) {
                bios_path = argv[i];
            } else {
                throw std::runtime_error("unexpected extra argument: " +
                                         std::string(argv[i]));
            }
        }
        ngba::MemoryBus bus(rom, bios_path);
        if (bios_boot && !bus.UsingExternalBios()) {
            throw std::runtime_error("--bios-boot requires an external BIOS image");
        }
        ngba::Arm7Tdmi cpu(bus);
        if (!bios_mode.empty()) cpu.SetBiosMode(ngba::ParseBiosMode(bios_mode));
        if (bios_boot) {
            cpu.ResetBios();
        } else {
            cpu.Reset(rom.ResetVectorAddress());
        }
        const std::uint64_t count = argc >= 3 ? ParseCount(argv[2]) : 64;

        std::cout << "NGBA CPU trace\n"
                  << "  BIOS mode: " << ngba::BiosModeName(cpu.GetBiosMode()) << "\n"
                  << "  bios: " << (bus.UsingExternalBios() ? "external" : "synthetic")
                  << "\n  boot: " << (bios_boot ? "BIOS reset" : "cartridge entry")
                  << '\n';
        for (std::uint64_t i = 0; i < count; ++i) {
            const ngba::TraceRecord record = cpu.Step();
            const ngba::CpuState& state = cpu.State();
            std::cout << std::setw(5) << i << " "
                      << (record.executed ? (record.thumb ? "T" : "A") : "H")
                      << " pc=";
            PrintHex(record.pc);
            std::cout << " op=";
            PrintHex(record.instruction);
            std::cout << " r0=";
            PrintHex(state.r[0]);
            std::cout << " r1=";
            PrintHex(state.r[1]);
            std::cout << " r2=";
            PrintHex(state.r[2]);
            std::cout << " r3=";
            PrintHex(state.r[3]);
            std::cout << " r4=";
            PrintHex(state.r[4]);
            std::cout << " r5=";
            PrintHex(state.r[5]);
            std::cout << " r6=";
            PrintHex(state.r[6]);
            std::cout << " r7=";
            PrintHex(state.r[7]);
            std::cout << " r12=";
            PrintHex(state.r[12]);
            std::cout << " lr=";
            PrintHex(state.r[14]);
            std::cout << " cpsr=";
            PrintHex(state.cpsr);
            std::cout << " sp=";
            PrintHex(state.r[13]);
            std::cout << " next=";
            PrintHex(state.r[15]);
            std::cout << '\n';
        }
    } catch (const ngba::CpuError& error) {
        std::cerr << "NGBA CPU trace stopped: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "NGBA trace error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
