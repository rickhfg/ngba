#include "ngba/rom.hpp"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {

void WriteLe32(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void WriteAscii(std::vector<std::uint8_t>& bytes, std::size_t offset,
                const std::string& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value[i]);
    }
}

std::uint8_t Complement(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0xA0; i <= 0xBC; ++i) {
        sum += bytes[i];
    }
    return static_cast<std::uint8_t>(0u - sum - 0x19u);
}

} // namespace

int main() {
    assert(ngba::IsArmBranch(0xEA000032u));
    assert(ngba::DecodeArmBranchTarget(0xEA000032u, 0x08000000u) == 0x080000D0u);
    assert(ngba::DecodeArmBranchTarget(0xEAFFFFFEu, 0x08000100u) == 0x08000100u);

    std::vector<std::uint8_t> bytes(0xC0, 0);
    WriteLe32(bytes, 0, 0xEA000032u);
    WriteAscii(bytes, 0xA0, "TEST GAME");
    WriteAscii(bytes, 0xAC, "TEST");
    WriteAscii(bytes, 0xB0, "01");
    bytes[0xB2] = 0x96;
    bytes[0xBC] = 0;
    bytes[0xBD] = Complement(bytes);

    const std::string path = "ngba_rom_test.gba";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    const ngba::RomImage rom = ngba::RomImage::Load(path);
    assert(rom.Size() == bytes.size());
    assert(rom.Header().title == "TEST GAME");
    assert(rom.Header().game_code == "TEST");
    assert(rom.HasValidHeader());
    assert(rom.ResetVectorAddress() == 0x080000D0u);

    std::remove(path.c_str());
    return 0;
}
