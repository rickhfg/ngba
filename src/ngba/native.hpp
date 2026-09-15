#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>

namespace ngba {

struct CpuState;

class NativeBackend {
public:
    NativeBackend() = default;
    ~NativeBackend();
    NativeBackend(const NativeBackend&) = delete;
    NativeBackend& operator=(const NativeBackend&) = delete;

    static bool Available() noexcept;
    bool Execute(CpuState& state, std::uint32_t instruction, bool thumb);
    std::size_t CachedBlocks() const noexcept;

private:
    using Function = void (*)(CpuState*);
    std::unordered_map<std::uint64_t, Function> blocks_;
};

}
