#include "ngba/runtime.hpp"
#include "ngba/native.hpp"
#include <chrono>
#include <iostream>

using Clock = std::chrono::high_resolution_clock;

int main() {
    try {
        const bool native = ngba::NativeBackend::Available();
        std::cout << "Backend: " << (native ? "Native + fallback" : "Interpreter") << "\n";
        ngba::Runtime runtime("Pokemon - Ruby Version (USA, Europe) (Rev 1).gba", "gba_bios.bin", native);
        runtime.LoadState("Pokemon - Ruby Version (USA, Europe) (Rev 1).gba.ngbs");

        ngba::Framebuffer fb;
        const unsigned kFrames = 300;

        auto start_steps = runtime.Cpu().Steps();
        auto start_halt_cycles = runtime.Cpu().HaltCycles();
        auto start_cpu_cycles = runtime.Cpu().CpuExecutionCycles();
        auto start_translated = runtime.Cpu().TranslatedInstructions();
        auto start_bus_cycles = runtime.Bus().Cycles();

        auto total_start = Clock::now();
        std::chrono::nanoseconds cpu_time(0);
        std::chrono::nanoseconds render_time(0);

        for (unsigned i = 0; i < kFrames; ++i) {
            auto t0 = Clock::now();
            runtime.StepFrame();
            auto t1 = Clock::now();
            runtime.Render(fb);
            auto t2 = Clock::now();

            cpu_time += (t1 - t0);
            render_time += (t2 - t1);
        }
        auto total_end = Clock::now();

        auto end_steps = runtime.Cpu().Steps();
        auto end_halt_cycles = runtime.Cpu().HaltCycles();
        auto end_cpu_cycles = runtime.Cpu().CpuExecutionCycles();
        auto end_translated = runtime.Cpu().TranslatedInstructions();
        auto end_bus_cycles = runtime.Bus().Cycles();

        double total_s = std::chrono::duration<double>(total_end - total_start).count();
        double cpu_s = std::chrono::duration<double>(cpu_time).count();
        double render_s = std::chrono::duration<double>(render_time).count();

        std::cout << "Frames: " << kFrames << "\n";
        std::cout << "Total time:  " << total_s << " s (" << (kFrames / total_s) << " FPS)\n";
        std::cout << "CPU time:    " << cpu_s << " s (" << (cpu_s / kFrames * 1000.0) << " ms/frame, " << (kFrames / cpu_s) << " max FPS)\n";
        std::cout << "Render time: " << render_s << " s (" << (render_s / kFrames * 1000.0) << " ms/frame, " << (kFrames / render_s) << " max FPS)\n";
        std::cout << "Bus cycles: " << (end_bus_cycles - start_bus_cycles) << " (" << ((end_bus_cycles - start_bus_cycles) / kFrames) << " per frame)\n";
        std::cout << "CPU execution cycles: " << (end_cpu_cycles - start_cpu_cycles) << " (" << ((end_cpu_cycles - start_cpu_cycles) / kFrames) << " per frame)\n";
        std::cout << "Halt cycles: " << (end_halt_cycles - start_halt_cycles) << " (" << ((end_halt_cycles - start_halt_cycles) / kFrames) << " per frame)\n";
        std::cout << "Instructions retired: " << (end_steps - start_steps) << " (" << ((end_steps - start_steps) / kFrames) << " per frame)\n";
        std::cout << "Native instructions: " << (end_translated - start_translated) << " (" << ((end_translated - start_translated) / kFrames) << " per frame)\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
