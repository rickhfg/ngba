#include "ngba/file_io.hpp"
#include "ngba/runtime.hpp"

#include <cassert>
#include <cstdio>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void RemoveFile(const std::string& path) {
#ifdef _WIN32
    DeleteFileW(ngba::Utf8ToWide(path).c_str());
#else
    std::remove(path.c_str());
#endif
}

template <typename Action>
void ExpectFailure(Action action) {
    bool failed = false;
    try { action(); }
    catch (const std::exception&) { failed = true; }
    assert(failed);
}

void TestBinaryFiles() {
    const std::string path = u8"ngba-file-test-\u00E9-\u6E38\u620F-\U0001F3AE.bin";
    const std::vector<std::uint8_t> bytes{0, 128, 255, 42};
    ngba::WriteBinaryFile(path, bytes);
    assert(ngba::ReadBinaryFile(path, bytes.size()) == bytes);
    ExpectFailure([&] { ngba::ReadBinaryFile(path, bytes.size() - 1); });
    ngba::WriteBinaryFile(path, {});
    assert(ngba::ReadBinaryFile(path, 0).empty());
    RemoveFile(path);
    ExpectFailure([&] { ngba::ReadBinaryFile(path, 1024); });
    ExpectFailure([&] { ngba::WriteBinaryFile(path + "/missing/file.bin", bytes); });
#ifdef _WIN32
    assert(ngba::WideToUtf8(ngba::Utf8ToWide(path)) == path);
    assert(ngba::WideToUtf8(L"").empty());
    assert(ngba::Utf8ToWide("").empty());
    ExpectFailure([] { ngba::Utf8ToWide("\xFF"); });
    ExpectFailure([] { ngba::WideToUtf8(std::wstring(1, static_cast<wchar_t>(0xD800))); });
#endif
}

void TestUnicodeRuntimePaths() {
    const std::string rom_path = u8"ngba-rom-test-\u00E9-\u6E38\u620F.gba";
    const std::string bios_path = u8"ngba-bios-test-\u00E9-\u6E38\u620F.bin";
    const std::string state_path = rom_path + ".ngbs";
    std::vector<std::uint8_t> rom(512, 0);
    rom[0] = 0x32; rom[3] = 0xEA;
    rom[0xB2] = 0x96;
    rom[0xD0] = 1; rom[0xD2] = 0xA0; rom[0xD3] = 0xE3;
    rom[0xD4] = 0xFD; rom[0xD5] = 0xFF; rom[0xD6] = 0xFF; rom[0xD7] = 0xEA;
    ngba::WriteBinaryFile(rom_path, rom);
    ngba::WriteBinaryFile(bios_path, std::vector<std::uint8_t>(16384, 0));
    ngba::Runtime runtime(rom_path, bios_path, ngba::NativeBackend::Available());
    runtime.StepFrame();
    runtime.Bus().Write32(0x02000000, 0x12345678);
    runtime.SaveState(state_path);
    const auto saved = ngba::ReadBinaryFile(state_path, 2u * 1024u * 1024u);
    const auto saved_frame = runtime.Bus().Frames();
    runtime.StepFrame();
    runtime.Bus().Write32(0x02000000, 0x98765432);
    runtime.LoadState(state_path);
    assert(runtime.Bus().Frames() == saved_frame);
    assert(runtime.Bus().Read32(0x02000000) == 0x12345678);
    runtime.SaveState(state_path);
    assert(ngba::ReadBinaryFile(state_path, 2u * 1024u * 1024u) == saved);
    runtime.StepFrame();
    runtime.SaveState(state_path);
    runtime.StepFrame();
    runtime.LoadState(state_path);
    assert(runtime.Bus().Frames() == saved_frame + 1);
    ExpectFailure([&] { ngba::ReadBinaryFile(state_path + ".tmp", 2u * 1024u * 1024u); });
    ngba::WriteBinaryFile(bios_path, std::vector<std::uint8_t>(16383, 0));
    ExpectFailure([&] { ngba::Runtime invalid(rom_path, bios_path); });
    ngba::WriteBinaryFile(bios_path, std::vector<std::uint8_t>(16385, 0));
    ExpectFailure([&] { ngba::Runtime invalid(rom_path, bios_path); });
    ngba::WriteBinaryFile(rom_path, std::vector<std::uint8_t>(16, 0));
    ExpectFailure([&] { ngba::RomImage::Load(rom_path); });
    for (const auto& path : {rom_path, bios_path, state_path}) RemoveFile(path);
}

}

int main() {
    TestBinaryFiles();
    TestUnicodeRuntimePaths();
    return 0;
}
