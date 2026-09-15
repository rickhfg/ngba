#include "ngba/arm7.hpp"

#include "ngba/ppu.hpp"
#include "ngba/rom.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

enum class BudgetMode {
    Instructions,
    Cycles,
    Frames,
};

std::uint64_t ParseCount(const char* text, const char* label) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
        throw std::runtime_error(std::string(label) +
                                 " must be a positive integer");
    }
    return static_cast<std::uint64_t>(value);
}

std::uint16_t ParseKeys(const char* text) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value > 0x03FFu) {
        throw std::runtime_error(
            "key mask must be an integer from 0 through 0x3FF");
    }
    return static_cast<std::uint16_t>(value);
}

std::uint32_t ParseAddress(const char* text) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value > 0xFFFFFFFFul) {
        throw std::runtime_error(
            "watch address must be a 32-bit integer");
    }
    return static_cast<std::uint32_t>(value);
}

const char* StopReasonName(ngba::RunStopReason reason) {
    switch (reason) {
    case ngba::RunStopReason::CycleBudget: return "cycle budget";
    case ngba::RunStopReason::FrameReady: return "frame ready";
    case ngba::RunStopReason::CpuStopped: return "CPU stopped";
    }
    return "unknown";
}

void PrintHex(std::uint32_t value) {
    std::cout << "0x" << std::uppercase << std::hex << std::setw(8)
              << std::setfill('0') << value << std::dec << std::setfill(' ');
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Add --native to enable the Windows native CPU subset with interpreter fallback.\n";
        std::cerr << "Usage: " << argv[0]
                  << " <game.gba> <frame.ppm> [instructions] [bios.bin] [--keys MASK] [--bios-mode external|hle|hybrid] [--bios-boot] [--trace] [--watch ADDRESS] [--watch-range ADDRESS SIZE]\n"
                  << "       " << argv[0]
                  << " <game.gba> <frame.ppm> --cycles N [bios.bin] [--keys MASK] [--bios-mode external|hle|hybrid] [--bios-boot] [--trace] [--watch ADDRESS] [--watch-range ADDRESS SIZE]\n"
                  << "       " << argv[0]
                  << " <game.gba> <frame.ppm> --frames N [bios.bin] [--keys MASK] [--bios-mode external|hle|hybrid] [--bios-boot] [--trace] [--watch ADDRESS] [--watch-range ADDRESS SIZE]\n";
        return 2;
    }

    try {
        const ngba::RomImage rom = ngba::RomImage::Load(argv[1]);
        BudgetMode mode = BudgetMode::Instructions;
        std::uint64_t budget = 1000000;
        std::string bios_path;
        std::uint16_t pressed_keys = 0;
        std::string bios_mode;
        bool bios_boot = false;
        bool diagnostics = false;
        bool native_cpu = false;
        std::uint32_t watch_address = 0;
        std::uint32_t watch_size = 0;
        int next_argument = 4;
        if (argc >= 4) {
            const std::string selector = argv[3];
            if (selector == "--cycles" || selector == "--frames") {
                mode = selector == "--cycles"
                    ? BudgetMode::Cycles
                    : BudgetMode::Frames;
                if (argc < 5) {
                    throw std::runtime_error(selector +
                                             " requires a positive budget");
                }
                budget = ParseCount(argv[4],
                                    mode == BudgetMode::Cycles
                                        ? "cycle budget"
                                        : "frame budget");
                next_argument = 5;
            } else {
                budget = ParseCount(argv[3], "instruction count");
                next_argument = 4;
            }
        }

        for (int i = next_argument; i < argc;) {
            const std::string argument = argv[i];
            if (argument == "--keys") {
                if (i + 1 >= argc) {
                    throw std::runtime_error("--keys requires a mask");
                }
                pressed_keys = ParseKeys(argv[i + 1]);
                i += 2;
            } else if (argument == "--native-memory-swis") {
                throw std::runtime_error("--native-memory-swis was replaced by --bios-mode external");
            } else if (argument == "--bios-mode") {
                if (i + 1 >= argc) throw std::runtime_error("--bios-mode requires external, hle, or hybrid");
                bios_mode = argv[i + 1];
                i += 2;
            } else if (argument == "--bios-boot") {
                bios_boot = true;
                ++i;
            } else if (argument == "--native") {
                native_cpu = true;
                ++i;
            } else if (argument == "--trace") {
                diagnostics = true;
                ++i;
            } else if (argument == "--watch") {
                if (i + 1 >= argc) {
                    throw std::runtime_error("--watch requires an address");
                }
                watch_address = ParseAddress(argv[i + 1]);
                watch_size = 4;
                diagnostics = true;
                i += 2;
            } else if (argument == "--watch-range") {
                if (i + 2 >= argc) {
                    throw std::runtime_error(
                        "--watch-range requires an address and size");
                }
                watch_address = ParseAddress(argv[i + 1]);
                watch_size = ParseCount(argv[i + 2], "watch size");
                diagnostics = true;
                i += 3;
            } else if (bios_path.empty()) {
                bios_path = argument;
                ++i;
            } else {
                throw std::runtime_error(
                    "unexpected extra argument: " + argument);
            }
        }

        ngba::MemoryBus bus(rom, bios_path);
        if (bios_boot && !bus.UsingExternalBios()) {
            throw std::runtime_error("--bios-boot requires an external BIOS image");
        }
        bus.SetKeys(pressed_keys);
        if (watch_size != 0) {
            bus.WatchIwramWrites(watch_address, watch_size);
            bus.WatchMemoryWrites(watch_address, watch_size);
        }
        ngba::Arm7Tdmi cpu(bus);
        cpu.EnableNative(native_cpu);
        if (!bios_mode.empty()) cpu.SetBiosMode(ngba::ParseBiosMode(bios_mode));
        if (bios_boot) {
            cpu.ResetBios();
        } else {
            cpu.Reset(rom.ResetVectorAddress());
        }
        ngba::RunResult run_result;
        if (mode == BudgetMode::Instructions) {
            for (std::uint64_t i = 0; i < budget; ++i) cpu.Step();
        } else if (mode == BudgetMode::Cycles) {
            run_result = cpu.RunForCycles(budget);
        } else {
            run_result = cpu.RunUntilFrameReady(bus.Frames() + budget);
        }

        ngba::Framebuffer framebuffer;
        ngba::PpuRenderer renderer(bus);
        renderer.Render(framebuffer);
        ngba::PpuRenderer::WritePpm(framebuffer, argv[2]);

        std::cout << "NGBA frame dump\n"
                  << "  translated instructions: " << cpu.TranslatedInstructions() << "\n"
                  << "  native blocks: " << cpu.NativeBlocks() << "\n"
                  << "  bios: " << (bus.UsingExternalBios() ? "external" : "synthetic") << "\n"
                  << "  budget mode: "
                  << (mode == BudgetMode::Instructions
                          ? "instructions"
                          : (mode == BudgetMode::Cycles ? "cycles" : "frames"))
                  << "\n"
                  << "  budget: " << budget << "\n"
                  << "  instructions retired: " << cpu.Steps() << "\n"
                  << "  master cycles: " << bus.Cycles() << "\n"
                  << "  cpu execution cycles: " << cpu.CpuExecutionCycles() << "\n"
                  << "  halt cycles: " << cpu.HaltCycles() << "\n"
                  << "  frames: " << bus.Frames() << "\n"
                  << "  dma transfers: " << bus.DmaTransfers() << "\n"
                  << "  dma cycles: " << bus.DmaCycles() << "\n"
                  << "  halt entries: " << cpu.HaltEntries() << "\n"
                  << "  halt wakeups: " << cpu.HaltWakeups() << "\n"
                  << "  BIOS SWIs: " << cpu.BiosSoftwareInterrupts() << "\n"
                  << "  BIOS mode: " << ngba::BiosModeName(cpu.GetBiosMode()) << "\n"
                  << "  display enabled: " << (bus.DisplayWasEnabled() ? "yes" : "no") << "\n"
                  << "  vcount: " << bus.VCount() << "\n"
                  << "  dispcnt: ";
        PrintHex(bus.Read16(0x04000000u));
        std::cout << "  dispcnt writes: " << bus.DisplayControlWrites()
                  << "  last write pc: ";
        PrintHex(bus.LastDisplayControlWritePc());
        std::cout << "  last write value: ";
        PrintHex(bus.LastDisplayControlWriteValue());
        std::cout << "\n  bgcnt: ";
        PrintHex(bus.Read16(0x04000008u));
        std::cout << " ";
        PrintHex(bus.Read16(0x0400000Au));
        std::cout << " ";
        PrintHex(bus.Read16(0x0400000Cu));
        std::cout << " ";
        PrintHex(bus.Read16(0x0400000Eu));
        std::cout << "\n"
                  << "  dispstat: ";
        PrintHex(bus.Read16(0x04000004u));
        std::cout << "  waitcnt: ";
        PrintHex(bus.Read16(0x04000204u));
        std::cout << "  ie: ";
        PrintHex(bus.Read16(0x04000200u));
        std::cout << "  if: ";
        PrintHex(bus.Read16(0x04000202u));
        std::cout << "  ime: ";
        PrintHex(bus.Read16(0x04000208u));
        std::cout << "\n  keys pressed: ";
        PrintHex(bus.PressedKeys());
        std::cout << "\n"
                  << "  pc: ";
        PrintHex(cpu.State().r[15]);
        std::cout << "\n  irq handler: ";
        PrintHex(bus.IrqHandlerAddress());
        std::cout << "\n  bios flags: ";
        PrintHex(bus.Read16(0x03007FF8u));
        std::cout << "\n  iwram 0x03003070: ";
        PrintHex(bus.Read32(0x03003070u));
        std::cout << "\n  iwram 0x03007FF8: ";
        PrintHex(bus.Read32(0x03007FF8u));
        std::cout << "\n  iwram guest writes: " << bus.IwramWriteCount();
        if (mode != BudgetMode::Instructions) {
            std::cout << "\n  run stop: " << StopReasonName(run_result.reason)
                      << "\n  run overshoot: " << run_result.overshoot;
        }
        if (diagnostics) {
            std::cout << "\n  software interrupt trace (first 64):\n";
            std::size_t printed_swis = 0;
            for (const ngba::SoftwareInterruptRecord& record :
                 cpu.SoftwareInterruptTrace()) {
                if (printed_swis++ >= 64) break;
                std::cout << "    cycle=" << record.cycle
                          << " " << (record.bios ? "bios" : "hle")
                          << " swi=0x" << std::uppercase << std::hex
                          << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned>(record.number)
                          << std::dec << std::setfill(' ')
                          << " " << (record.thumb ? "T" : "A")
                          << " pc=";
                PrintHex(record.pc);
                std::cout << " lr=";
                PrintHex(record.lr);
                std::cout << " stack_top=";
                PrintHex(record.stack_top);
                std::cout << " r0=";
                PrintHex(record.r0);
                std::cout << " r1=";
                PrintHex(record.r1);
                std::cout << " r2=";
                PrintHex(record.r2);
                std::cout << " sp=";
                PrintHex(record.sp);
                std::cout << " sp_sys=";
                PrintHex(record.sp_user_system);
                std::cout << " sp_irq=";
                PrintHex(record.sp_irq);
                std::cout << " sp_svc=";
                PrintHex(record.sp_svc);
                std::cout << " cpsr=";
                PrintHex(record.cpsr);
                std::cout << '\n';
            }
            if (cpu.SoftwareInterruptTrace().size() > 64) {
                std::cout << "    ... "
                          << (cpu.SoftwareInterruptTrace().size() - 64)
                          << " additional records omitted\n";
            }

            std::cout << "  IWRAM write trace (first 256 bytes):\n";
            std::size_t printed_writes = 0;
            for (const ngba::IwramWriteRecord& record :
                 bus.IwramWriteRecords()) {
                if (printed_writes++ >= 256) break;
                std::cout << "    cycle=" << record.cycle << " pc=";
                PrintHex(record.pc);
                std::cout << " address=";
                PrintHex(record.address);
                std::cout << " value=";
                PrintHex(record.value);
                std::cout << '\n';
            }
            if (bus.IwramWriteRecords().size() > 256) {
                std::cout << "    ... "
                          << (bus.IwramWriteRecords().size() - 256)
                          << " additional records omitted\n";
            }

            if (watch_size != 0) {
                std::cout << "  watched IWRAM writes at ";
                PrintHex(watch_address);
                std::cout << " (" << watch_size << " bytes):\n";
                for (const ngba::IwramWriteRecord& record :
                     bus.WatchedIwramWriteRecords()) {
                    std::cout << "    cycle=" << record.cycle << " pc=";
                    PrintHex(record.pc);
                    std::cout << " address=";
                    PrintHex(record.address);
                    std::cout << " value=";
                    PrintHex(record.value);
                    std::cout << '\n';
                }

                std::cout << "  watched memory writes at ";
                PrintHex(watch_address);
                std::cout << " (" << watch_size << " bytes):\n";
                for (const ngba::MemoryWriteRecord& record :
                     bus.WatchedMemoryWriteRecords()) {
                    std::cout << "    cycle=" << record.cycle << " pc=";
                    PrintHex(record.pc);
                    std::cout << " address=";
                    PrintHex(record.address);
                    std::cout << " value=";
                    PrintHex(record.value);
                    std::cout << '\n';
                }
            }
        }
        std::cout << "\n  output: " << argv[2] << "\n";
    } catch (const ngba::CpuError& error) {
        std::cerr << "NGBA frame CPU stopped: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "NGBA frame error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
