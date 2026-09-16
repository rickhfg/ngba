#pragma once

#include "ngba/arm7.hpp"
#include "ngba/ppu.hpp"

#include <memory>
#include <string>

namespace ngba {

class Runtime {
public:
    Runtime(const std::string& rom_path, const std::string& bios_path = "", bool native = false);
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    MemoryBus& Bus() noexcept;
    Arm7Tdmi& Cpu() noexcept;
    void SetPaused(bool paused) noexcept;
    bool Paused() const noexcept;
    RunResult RunForCycles(Cycle cycles);
    void StepFrame();
    void Render(Framebuffer& framebuffer) const;
    void SaveState(const std::string& path) const;
    void LoadState(const std::string& path);
    void FlushBatterySave();
    const std::string& SavePath() const noexcept { return save_path_; }
    ~Runtime();

private:
    std::string rom_path_;
    std::string save_path_;
    RomImage rom_;
    std::unique_ptr<MemoryBus> bus_;
    std::unique_ptr<Arm7Tdmi> cpu_;
    bool native_{};
    bool paused_{};
};

}
