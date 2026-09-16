#include "ngba/rom.hpp"
#include "ngba/version.hpp"

#include <iomanip>
#include <iostream>
#include <string>

namespace {

void PrintUsage(const char* executable) {
    std::cerr << "Usage: " << executable << " <game.gba>\n";
}

void PrintHex32(std::uint32_t value) {
    std::cout << "0x" << std::uppercase << std::hex << std::setw(8)
              << std::setfill('0') << value << std::dec << std::setfill(' ');
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        PrintUsage(argv[0]);
        return 2;
    }

    try {
        const ngba::RomImage rom = ngba::RomImage::Load(argv[1]);
        const ngba::RomHeader& header = rom.Header();

        std::cout << "NGBA v" << ngba::kVersionString << " ROM probe\n"
                  << "  path: " << rom.Path() << '\n'
                  << "  size: " << rom.Size() << " bytes\n"
                  << "  title: " << header.title << '\n'
                  << "  game code: " << header.game_code << '\n'
                  << "  maker code: " << header.maker_code << '\n'
                  << "  software version: " << static_cast<unsigned>(header.software_version)
                  << '\n'
                  << "  fixed header byte: " << (header.fixed_value_valid ? "valid" : "invalid")
                  << '\n'
                  << "  complement check: "
                  << (header.complement_check_valid ? "valid" : "invalid") << '\n'
                  << "  header: " << (rom.HasValidHeader() ? "valid" : "invalid") << '\n'
                  << "  reset vector instruction: ";
        PrintHex32(header.entry_instruction);
        std::cout << '\n' << "  reset vector target: ";
        PrintHex32(rom.ResetVectorAddress());
        std::cout << '\n';
    } catch (const std::exception& error) {
        std::cerr << "NGBA probe error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
