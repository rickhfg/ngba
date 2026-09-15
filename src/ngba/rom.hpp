#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ngba {

constexpr std::uint32_t kGbaRomBase = 0x08000000u;

struct RomHeader {
    std::uint32_t entry_instruction{};
    std::string title;
    std::string game_code;
    std::string maker_code;
    std::uint8_t fixed_value{};
    std::uint8_t software_version{};
    std::uint8_t complement_check{};
    bool fixed_value_valid{};
    bool complement_check_valid{};
};

/// Decode an ARM state B/BL instruction whose instruction lives at @p pc.
/// The ARM PC used by the branch calculation is pc + 8.
std::uint32_t DecodeArmBranchTarget(std::uint32_t instruction,
                                    std::uint32_t pc);

bool IsArmBranch(std::uint32_t instruction) noexcept;

class RomImage {
public:
    static RomImage Load(const std::string& path);

    const std::string& Path() const noexcept;
    const std::vector<std::uint8_t>& Bytes() const noexcept;
    const RomHeader& Header() const noexcept;

    std::size_t Size() const noexcept;
    bool HasValidHeader() const noexcept;

    /// Resolve the ARM branch at the GBA cartridge reset vector.
    std::uint32_t ResetVectorAddress() const;

private:
    RomImage(std::string path, std::vector<std::uint8_t> bytes);

    static RomHeader ParseHeader(const std::vector<std::uint8_t>& bytes);

    std::string path_;
    std::vector<std::uint8_t> bytes_;
    RomHeader header_;
};

} // namespace ngba
