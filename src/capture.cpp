#include "ngba/runtime.hpp"

#include <chrono>
#include <iostream>
#include <map>

namespace {
unsigned Number(const std::string& text) {
    std::size_t end = 0;
    const auto value = std::stoul(text, &end, 0);
    if (text.empty() || text[0] == '-' || end != text.size() || value > 1000000) {
        throw std::runtime_error("invalid capture number");
    }
    return static_cast<unsigned>(value);
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 5) throw std::runtime_error("Usage: ngba_capture game.gba bios.bin output-prefix frames [--stride N] [--render-every-frame] [--bios-mode external|hle|hybrid] [--interpreter] [--keys-at FRAME MASK] [--load-state FILE]");
        const unsigned frames = Number(argv[4]);
        unsigned stride = 30;
        bool native = ngba::NativeBackend::Available();
        bool render_every_frame = false;
        std::string bios_mode;
        std::string load_state;
        std::map<unsigned, std::uint16_t> keys;
        for (int index = 5; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--interpreter") native = false;
            else if (option == "--render-every-frame") render_every_frame = true;
            else if (option == "--stride" && index + 1 < argc) stride = Number(argv[++index]);
            else if (option == "--bios-mode" && index + 1 < argc) bios_mode = argv[++index];
            else if (option == "--load-state" && index + 1 < argc) load_state = argv[++index];
            else if (option == "--keys-at" && index + 2 < argc) {
                const unsigned frame = Number(argv[++index]);
                const unsigned mask = Number(argv[++index]);
                if (mask > 1023) throw std::runtime_error("invalid GBA key mask");
                keys[frame] = static_cast<std::uint16_t>(mask);
            } else throw std::runtime_error("invalid capture argument: " + option);
        }
        if (stride == 0 || frames == 0) throw std::runtime_error("frames and stride must be positive");
        ngba::Runtime runtime(argv[1], argv[2], native);
        if (!load_state.empty()) runtime.LoadState(load_state);
        if (!bios_mode.empty()) runtime.Cpu().SetBiosMode(ngba::ParseBiosMode(bios_mode));
        const std::string prefix = argv[3];
        ngba::Framebuffer pixels;
        unsigned rendered = 0;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned frame = 1; frame <= frames; ++frame) {
            const auto input = keys.find(frame);
            if (input != keys.end()) runtime.Bus().SetKeys(input->second);
            runtime.StepFrame();
            const bool dump = frame % stride == 0 || frame == frames;
            if (render_every_frame || dump) {
                runtime.Render(pixels);
                ++rendered;
            }
            if (dump) {
                ngba::PpuRenderer::WritePpm(pixels, prefix + "-" + std::to_string(frame) + ".ppm");
                std::cout << "frame " << frame << " pc " << std::hex << runtime.Cpu().State().r[15]
                          << " dispcnt " << runtime.Bus().Read16(0x04000000) << std::dec << "\n";
            }
        }
        runtime.SaveState(prefix + ".ngbs");
        for (const auto& record : runtime.Cpu().SoftwareInterruptTrace()) {
            if (record.number != 0x11 && record.number != 0x12) continue;
            std::cout << "LZ77 cycle " << record.cycle << " source " << std::hex << record.r0
                      << " destination " << record.r1 << std::dec << "\n";
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << frames << " guest frames (" << rendered << " rendered) in " << seconds
                  << " seconds; " << frames / seconds << " guest fps\n"
                  << "Native instructions: " << runtime.Cpu().TranslatedInstructions() << "\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
