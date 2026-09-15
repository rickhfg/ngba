#include "ngba/native.hpp"
#include "ngba/arm7.hpp"

#include <cstring>
#include <cstddef>
#include <type_traits>
#include <vector>

#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__) || defined(_M_X64) || defined(__x86_64__))
#define NGBA_NATIVE_WINDOWS 1
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ngba {

static_assert(std::is_standard_layout<CpuState>::value, "native state must have standard layout");
static_assert(offsetof(CpuState, cpsr) == 64, "native flags offset must match CpuState");

bool NativeBackend::Available() noexcept {
#ifdef NGBA_NATIVE_WINDOWS
    return true;
#else
    return false;
#endif
}

NativeBackend::~NativeBackend() {
#ifdef NGBA_NATIVE_WINDOWS
    for (const auto& block : blocks_) VirtualFree(reinterpret_cast<void*>(block.second), 0, MEM_RELEASE);
#endif
}

std::size_t NativeBackend::CachedBlocks() const noexcept { return blocks_.size(); }

bool NativeBackend::Execute(CpuState& state, std::uint32_t instruction, bool thumb) {
#ifdef NGBA_NATIVE_WINDOWS
    unsigned operation = 0;
    unsigned destination = 0;
    unsigned source = 0;
    std::uint32_t immediate = 0;
    if (thumb && (instruction & 0xE000u) == 0x2000u) {
        operation = (instruction >> 11) & 3u;
        source = destination = (instruction >> 8) & 7u;
        immediate = instruction & 255u;
    } else if (!thumb && (instruction & 0xFFF00000u) == 0xE3A00000u) {
        operation = 0;
        destination = (instruction >> 12) & 15u;
        immediate = instruction & 255u;
        const unsigned rotate = ((instruction >> 8) & 15u) * 2u;
        if (rotate != 0) immediate = (immediate >> rotate) | (immediate << (32u - rotate));
    } else if (!thumb && ((instruction & 0xFFF00000u) == 0xE2800000u ||
                         (instruction & 0xFFF00000u) == 0xE2400000u)) {
        operation = (instruction & 0x00400000u) != 0 ? 3u : 2u;
        destination = (instruction >> 12) & 15u;
        source = (instruction >> 16) & 15u;
        immediate = instruction & 255u;
        const unsigned rotate = ((instruction >> 8) & 15u) * 2u;
        if (rotate != 0) immediate = (immediate >> rotate) | (immediate << (32u - rotate));
    } else {
        return false;
    }
    if (destination == 15u || source == 15u) return false;
    const std::uint64_t key = instruction | (static_cast<std::uint64_t>(thumb) << 32);
    const auto found = blocks_.find(key);
    if (found != blocks_.end()) {
        found->second(&state);
        return true;
    }
    if (blocks_.size() >= 4096u) return false;
    std::vector<std::uint8_t> code;
    const auto emit = [&](std::initializer_list<std::uint8_t> bytes) { code.insert(code.end(), bytes); };
    const auto word = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) code.push_back(static_cast<std::uint8_t>(value >> shift));
    };
#ifdef _WIN64
    emit({0x48, 0x89, 0xCA});
#else
    emit({0x8B, 0x54, 0x24, 0x04});
#endif
    emit({0x53});
    if (operation == 0) {
        emit({0xB8}); word(immediate);
    } else {
        emit({0x8B, 0x42, static_cast<std::uint8_t>(source * 4)});
        emit({static_cast<std::uint8_t>(operation == 2 ? 0x05 : 0x2D)}); word(immediate);
    }
    if (operation != 1) emit({0x89, 0x42, static_cast<std::uint8_t>(destination * 4)});
    if (thumb) {
        if (operation == 0) emit({0x85, 0xC0});
        emit({0x9C, 0x58, 0x89, 0xC3, 0x81, 0xE3}); word(0xC0);
        emit({0xC1, 0xE3, 0x18});
        if (operation != 0) {
            emit({0x89, 0xC1, 0x83, 0xE1, 0x01});
            if (operation == 1 || operation == 3) emit({0x83, 0xF1, 0x01});
            emit({0xC1, 0xE1, 0x1D, 0x09, 0xCB, 0x25}); word(0x800);
            emit({0xC1, 0xE0, 0x11, 0x09, 0xC3});
        }
        emit({0x8B, 0x42, 0x40, 0x25}); word(operation == 0 ? 0x3FFFFFFFu : 0x0FFFFFFFu);
        emit({0x09, 0xD8, 0x89, 0x42, 0x40});
    }
    emit({0x5B, 0xC3});
    void* memory = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (memory == nullptr) return false;
    std::memcpy(memory, code.data(), code.size());
    DWORD previous = 0;
    if (!VirtualProtect(memory, code.size(), PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), memory, code.size())) {
        VirtualFree(memory, 0, MEM_RELEASE);
        return false;
    }
    const auto function = reinterpret_cast<Function>(memory);
    try { blocks_.emplace(key, function); }
    catch (...) { VirtualFree(memory, 0, MEM_RELEASE); throw; }
    function(&state);
    return true;
#else
    (void)state;
    (void)instruction;
    (void)thumb;
    return false;
#endif
}

}
