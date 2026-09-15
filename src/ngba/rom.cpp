#include "ngba/rom.hpp"
#include "ngba/file_io.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ngba {
namespace {

constexpr std::size_t kHeaderEnd = 0xC0;
constexpr std::size_t kTitleOffset = 0xA0;
constexpr std::size_t kTitleLength = 12;
constexpr std::size_t kGameCodeOffset = 0xAC;
constexpr std::size_t kGameCodeLength = 4;
constexpr std::size_t kMakerCodeOffset = 0xB0;
constexpr std::size_t kMakerCodeLength = 2;
constexpr std::size_t kFixedValueOffset = 0xB2;
constexpr std::size_t kSoftwareVersionOffset = 0xBC;
constexpr std::size_t kComplementCheckOffset = 0xBD;

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::string ReadPaddedAscii(const std::vector<std::uint8_t>& bytes,
                            std::size_t offset, std::size_t length) {
    std::string result;
    result.reserve(length);
    for (std::size_t i = 0; i < length; ++i) {
        const char c = static_cast<char>(bytes[offset + i]);
        if (c == '\0') {
            break;
        }
        result.push_back(c);
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

std::uint8_t ComputeComplementCheck(const std::vector<std::uint8_t>& bytes) {
    // GBA header complement check: -sum(A0..BC) - 0x19, modulo 256.
    std::uint32_t sum = 0;
    for (std::size_t i = kTitleOffset; i <= kSoftwareVersionOffset; ++i) {
        sum += bytes[i];
    }
    return static_cast<std::uint8_t>(0u - sum - 0x19u);
}

} // namespace

bool IsArmBranch(std::uint32_t instruction) noexcept {
    // ARM B/BL: bits 27..25 are 101. The condition field is intentionally
    // ignored; conditional branches are still valid ARM branch instructions.
    return (instruction & 0x0E000000u) == 0x0A000000u;
}

std::uint32_t DecodeArmBranchTarget(std::uint32_t instruction,
                                    std::uint32_t pc) {
    if (!IsArmBranch(instruction)) {
        throw std::invalid_argument("instruction is not an ARM B/BL instruction");
    }

    std::int32_t immediate = static_cast<std::int32_t>(instruction & 0x00FFFFFFu);
    if ((immediate & 0x00800000) != 0) {
        immediate |= static_cast<std::int32_t>(0xFF000000u);
    }

    const std::int64_t target = static_cast<std::int64_t>(pc) + 8 +
                                (static_cast<std::int64_t>(immediate) << 2);
    if (target < 0 || target > std::numeric_limits<std::uint32_t>::max()) {
        throw std::out_of_range("decoded ARM branch target does not fit in 32 bits");
    }
    return static_cast<std::uint32_t>(target);
}

RomImage RomImage::Load(const std::string& path) {
    auto bytes = ReadBinaryFile(path, 32u * 1024u * 1024u);
    if (bytes.size() < kHeaderEnd) {
        throw std::runtime_error("ROM is too small to contain a GBA header: " + path);
    }

    return RomImage(path, std::move(bytes));
}

RomImage::RomImage(std::string path, std::vector<std::uint8_t> bytes)
    : path_(std::move(path)), bytes_(std::move(bytes)), header_(ParseHeader(bytes_)) {}

RomHeader RomImage::ParseHeader(const std::vector<std::uint8_t>& bytes) {
    RomHeader header{};
    header.entry_instruction = ReadLe32(bytes, 0);
    header.title = ReadPaddedAscii(bytes, kTitleOffset, kTitleLength);
    header.game_code = ReadPaddedAscii(bytes, kGameCodeOffset, kGameCodeLength);
    header.maker_code = ReadPaddedAscii(bytes, kMakerCodeOffset, kMakerCodeLength);
    header.fixed_value = bytes[kFixedValueOffset];
    header.software_version = bytes[kSoftwareVersionOffset];
    header.complement_check = bytes[kComplementCheckOffset];
    header.fixed_value_valid = header.fixed_value == 0x96;
    header.complement_check_valid =
        ComputeComplementCheck(bytes) == header.complement_check;
    return header;
}

const std::string& RomImage::Path() const noexcept {
    return path_;
}

const std::vector<std::uint8_t>& RomImage::Bytes() const noexcept {
    return bytes_;
}

const RomHeader& RomImage::Header() const noexcept {
    return header_;
}

std::size_t RomImage::Size() const noexcept {
    return bytes_.size();
}

bool RomImage::HasValidHeader() const noexcept {
    return header_.fixed_value_valid && header_.complement_check_valid &&
           !header_.game_code.empty();
}

std::uint32_t RomImage::ResetVectorAddress() const {
    return DecodeArmBranchTarget(header_.entry_instruction, kGbaRomBase);
}

} // namespace ngba
