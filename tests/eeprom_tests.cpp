#include "ngba/eeprom.hpp"
#include "ngba/bus.hpp"
#include "ngba/rom.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

void Test4KbitWriteAndRead() {
    ngba::Eeprom eeprom;
    eeprom.SetEnabled(true);
    eeprom.SetSize(ngba::EepromSize::Eeprom4K);

    const std::uint8_t test_block = 5;
    const std::uint8_t test_data[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};

    // 1. Write Request: 2 bits '10', 6 bits address, 64 bits data, 1 bit '0' = 73 bits
    eeprom.Write(1); // cmd
    eeprom.Write(0); // cmd

    // 6-bit address MSB first
    for (int i = 5; i >= 0; --i) {
        eeprom.Write((test_block >> i) & 1u);
    }

    // 64-bit data MSB first per byte
    for (int b = 0; b < 8; ++b) {
        for (int i = 7; i >= 0; --i) {
            eeprom.Write((test_data[b] >> i) & 1u);
        }
    }

    // Stop bit
    eeprom.Write(0);

    assert(eeprom.Dirty());
    assert(eeprom.Data()[test_block * 8 + 0] == 0x12);
    assert(eeprom.Data()[test_block * 8 + 7] == 0xF0);

    // Ready polling should return 1
    assert(eeprom.Read() == 1);

    // 2. Read Request: 2 bits '11', 6 bits address, 1 bit '0' = 9 bits
    eeprom.Write(1); // cmd
    eeprom.Write(1); // cmd

    for (int i = 5; i >= 0; --i) {
        eeprom.Write((test_block >> i) & 1u);
    }
    eeprom.Write(0); // stop bit

    // 3. Read 68 bits: 4 dummy bits (0) followed by 64 data bits
    for (int i = 0; i < 4; ++i) {
        const std::uint16_t dummy = eeprom.Read();
        assert(dummy == 0);
    }

    std::uint8_t read_bytes[8] = {};
    for (int b = 0; b < 8; ++b) {
        std::uint8_t byte_val = 0;
        for (int i = 0; i < 8; ++i) {
            byte_val = static_cast<std::uint8_t>((byte_val << 1) | (eeprom.Read() & 1u));
        }
        read_bytes[b] = byte_val;
    }

    for (int b = 0; b < 8; ++b) {
        assert(read_bytes[b] == test_data[b]);
    }

    // After 68 bits, stream ends and idle returns 1
    assert(eeprom.Read() == 1);

    std::cout << "[PASS] Test4KbitWriteAndRead" << std::endl;
}

void Test64KbitWriteAndRead() {
    ngba::Eeprom eeprom;
    eeprom.SetEnabled(true);
    eeprom.SetSize(ngba::EepromSize::Eeprom64K);
    assert(eeprom.CapacityBytes() == 8192);

    const std::uint16_t test_block = 512; // Block 512 out of 1024
    const std::uint8_t test_data[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44};

    // 1. Write Request: 2 bits '10', 14 bits address, 64 bits data, 1 bit '0' = 81 bits
    eeprom.Write(1);
    eeprom.Write(0);

    // 14-bit address MSB first
    for (int i = 13; i >= 0; --i) {
        eeprom.Write((test_block >> i) & 1u);
    }

    // 64-bit data
    for (int b = 0; b < 8; ++b) {
        for (int i = 7; i >= 0; --i) {
            eeprom.Write((test_data[b] >> i) & 1u);
        }
    }

    // Stop bit
    eeprom.Write(0);

    assert(eeprom.Dirty());
    assert(eeprom.Data()[test_block * 8 + 0] == 0xAA);
    assert(eeprom.Data()[test_block * 8 + 7] == 0x44);

    // Ready polling returns 1
    assert(eeprom.Read() == 1);

    // 2. Read Request: 2 bits '11', 14 bits address, 1 bit '0' = 17 bits
    eeprom.Write(1);
    eeprom.Write(1);

    for (int i = 13; i >= 0; --i) {
        eeprom.Write((test_block >> i) & 1u);
    }
    eeprom.Write(0);

    // 3. Read 68 bits
    for (int i = 0; i < 4; ++i) {
        assert(eeprom.Read() == 0);
    }

    std::uint8_t read_bytes[8] = {};
    for (int b = 0; b < 8; ++b) {
        std::uint8_t byte_val = 0;
        for (int i = 0; i < 8; ++i) {
            byte_val = static_cast<std::uint8_t>((byte_val << 1) | (eeprom.Read() & 1u));
        }
        read_bytes[b] = byte_val;
    }

    for (int b = 0; b < 8; ++b) {
        assert(read_bytes[b] == test_data[b]);
    }

    assert(eeprom.Read() == 1);

    std::cout << "[PASS] Test64KbitWriteAndRead" << std::endl;
}

void TestAutodetection() {
    // Test autodetect from DMA counts
    {
        ngba::Eeprom eeprom;
        eeprom.SetEnabled(true);
        assert(eeprom.Size() == ngba::EepromSize::Autodetect);
        eeprom.NotifyDmaTransfer(9);
        assert(eeprom.Size() == ngba::EepromSize::Eeprom4K);
        assert(eeprom.CapacityBytes() == 512);
    }
    {
        ngba::Eeprom eeprom;
        eeprom.SetEnabled(true);
        assert(eeprom.Size() == ngba::EepromSize::Autodetect);
        eeprom.NotifyDmaTransfer(17);
        assert(eeprom.Size() == ngba::EepromSize::Eeprom64K);
        assert(eeprom.CapacityBytes() == 8192);
    }
    {
        ngba::Eeprom eeprom;
        eeprom.SetEnabled(true);
        assert(eeprom.Size() == ngba::EepromSize::Autodetect);
        eeprom.NotifyDmaTransfer(73);
        assert(eeprom.Size() == ngba::EepromSize::Eeprom4K);
    }
    {
        ngba::Eeprom eeprom;
        eeprom.SetEnabled(true);
        assert(eeprom.Size() == ngba::EepromSize::Autodetect);
        eeprom.NotifyDmaTransfer(81);
        assert(eeprom.Size() == ngba::EepromSize::Eeprom64K);
    }

    // Test autodetect without DMA hint (pure bitstream inspection)
    {
        ngba::Eeprom eeprom;
        eeprom.SetEnabled(true);
        assert(eeprom.Size() == ngba::EepromSize::Autodetect);

        // Send 4K read request (9 bits)
        eeprom.Write(1);
        eeprom.Write(1);
        for (int i = 0; i < 6; ++i) eeprom.Write(0);
        eeprom.Write(0); // stop bit

        // Read initiates autodetection
        const std::uint16_t d0 = eeprom.Read();
        assert(d0 == 0); // 1st dummy bit
        assert(eeprom.Size() == ngba::EepromSize::Eeprom4K);
    }
    {
        ngba::Eeprom eeprom;
        eeprom.SetEnabled(true);
        assert(eeprom.Size() == ngba::EepromSize::Autodetect);

        // Send 64K read request (17 bits)
        eeprom.Write(1);
        eeprom.Write(1);
        for (int i = 0; i < 14; ++i) eeprom.Write(0);
        eeprom.Write(0); // stop bit

        const std::uint16_t d0 = eeprom.Read();
        assert(d0 == 0);
        assert(eeprom.Size() == ngba::EepromSize::Eeprom64K);
        assert(eeprom.CapacityBytes() == 8192);
    }

    std::cout << "[PASS] TestAutodetection" << std::endl;
}

std::vector<std::uint8_t> CreateMockEepromRom() {
    std::vector<std::uint8_t> bytes(1024 * 1024, 0);
    // GBA Cartridge header
    bytes[0x00] = 0x2E; // Branch opcode
    bytes[0x01] = 0x00;
    bytes[0x02] = 0x00;
    bytes[0x03] = 0xEA;
    // Title: TEST_EEPROM
    const char title[] = "TEST_EEPROM";
    for (std::size_t i = 0; i < sizeof(title) - 1; ++i) bytes[0xA0 + i] = static_cast<std::uint8_t>(title[i]);
    // Game code: TEST
    bytes[0xAC] = 'T'; bytes[0xAD] = 'E'; bytes[0xAE] = 'S'; bytes[0xAF] = 'T';
    bytes[0xB2] = 0x96; // fixed value
    // Embed "EEPROM_V124" signature
    const char sig[] = "EEPROM_V124";
    for (std::size_t i = 0; i < sizeof(sig); ++i) bytes[0x1000 + i] = static_cast<std::uint8_t>(sig[i]);

    // Calculate complement check
    std::uint8_t checksum = 0;
    for (std::size_t i = 0xA0; i <= 0xBC; ++i) checksum -= bytes[i];
    bytes[0xBD] = checksum - 0x19;

    return bytes;
}

void TestMemoryBusEepromIntegration() {
    const auto rom_bytes = CreateMockEepromRom();
    // Write temporary ROM file
    const std::string tmp_rom = "test_eeprom.gba";
    {
        std::ofstream file(tmp_rom, std::ios::binary);
        file.write(reinterpret_cast<const char*>(rom_bytes.data()), rom_bytes.size());
    }

    ngba::RomImage rom = ngba::RomImage::Load(tmp_rom);
    ngba::MemoryBus bus(rom);

    assert(bus.GetEeprom().Enabled());
    assert(bus.IsEepromAddress(0x0D000000u));
    assert(bus.IsEepromAddress(0x0D000010u));

    // SRAM region should be unmapped
    assert(bus.Read8(0x0E000000u) == 0xFFu);
    assert(bus.Read16(0x0E000000u) == 0xFFFFu);
    assert(bus.Read32(0x0E000000u) == 0xFFFFFFFFu);
    bus.Write8(0x0E000000u, 0x42);
    assert(!bus.SaveMemoryDirty());
    assert(bus.Read8(0x0E000000u) == 0xFFu);

    // Test DMA3 round-trip through MemoryBus
    // We prepare a 73-halfword buffer in EWRAM (0x02000000) for a 4K write to block 3
    const std::uint8_t block = 3;
    const std::uint8_t payload[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};

    std::uint32_t ewram_src = 0x02000000u;
    std::vector<std::uint16_t> write_stream;
    write_stream.push_back(1); // cmd 1
    write_stream.push_back(0); // cmd 0
    for (int i = 5; i >= 0; --i) write_stream.push_back((block >> i) & 1u);
    for (int b = 0; b < 8; ++b) {
        for (int i = 7; i >= 0; --i) write_stream.push_back((payload[b] >> i) & 1u);
    }
    write_stream.push_back(0); // stop
    assert(write_stream.size() == 73);

    for (std::size_t i = 0; i < write_stream.size(); ++i) {
        bus.Write16(ewram_src + static_cast<std::uint32_t>(i * 2), write_stream[i]);
    }

    // Configure DMA3 for immediate halfword transfer to 0x0D000000
    // DMA3SAD (0x040000D4)
    bus.Write32(0x040000D4u, ewram_src);
    // DMA3DAD (0x040000D8)
    bus.Write32(0x040000D8u, 0x0D000000u);
    // DMA3CNT_L (0x040000DC) = 73
    bus.Write16(0x040000DCu, 73);
    // DMA3CNT_H (0x040000DE) = 0x8000 (enable, immediate, 16-bit, increment)
    bus.Write16(0x040000DEu, 0x8000u);

    // Advance bus to run the DMA transfer
    while (bus.DmaActive()) {
        bus.AdvanceTo(bus.NextEventCycle());
    }

    assert(bus.SaveMemoryDirty());
    assert(bus.SaveMemory()[block * 8 + 0] == 0xDE);
    assert(bus.SaveMemory()[block * 8 + 3] == 0xEF);
    assert(bus.SaveMemory()[block * 8 + 7] == 0xBE);

    // Poll ready
    assert((bus.Read16(0x0D000000u) & 1u) == 1);

    // Now send 9-bit Read Request via DMA3
    std::vector<std::uint16_t> read_req_stream;
    read_req_stream.push_back(1);
    read_req_stream.push_back(1);
    for (int i = 5; i >= 0; --i) read_req_stream.push_back((block >> i) & 1u);
    read_req_stream.push_back(0);
    assert(read_req_stream.size() == 9);

    std::uint32_t ewram_read_req = 0x02000200u;
    for (std::size_t i = 0; i < read_req_stream.size(); ++i) {
        bus.Write16(ewram_read_req + static_cast<std::uint32_t>(i * 2), read_req_stream[i]);
    }

    bus.Write32(0x040000D4u, ewram_read_req);
    bus.Write32(0x040000D8u, 0x0D000000u);
    bus.Write16(0x040000DCu, 9);
    bus.Write16(0x040000DEu, 0x8000u);
    while (bus.DmaActive()) {
        bus.AdvanceTo(bus.NextEventCycle());
    }

    // Now read 68 halfwords from 0x0D000000 to EWRAM buffer (0x02000400)
    std::uint32_t ewram_dest = 0x02000400u;
    bus.Write32(0x040000D4u, 0x0D000000u);
    bus.Write32(0x040000D8u, ewram_dest);
    bus.Write16(0x040000DCu, 68);
    bus.Write16(0x040000DEu, 0x8000u);
    while (bus.DmaActive()) {
        bus.AdvanceTo(bus.NextEventCycle());
    }

    // Verify received buffer
    std::vector<std::uint16_t> dma_received(68);
    for (std::size_t i = 0; i < 68; ++i) {
        dma_received[i] = bus.Read16(ewram_dest + static_cast<std::uint32_t>(i * 2));
    }

    // Check dummy bits
    for (int i = 0; i < 4; ++i) {
        assert((dma_received[i] & 1u) == 0);
    }

    // Unpack data bits
    std::uint8_t unpacked[8] = {};
    for (int b = 0; b < 8; ++b) {
        std::uint8_t byte_val = 0;
        for (int i = 0; i < 8; ++i) {
            byte_val = static_cast<std::uint8_t>((byte_val << 1) | (dma_received[4 + b * 8 + i] & 1u));
        }
        unpacked[b] = byte_val;
    }

    for (int b = 0; b < 8; ++b) {
        assert(unpacked[b] == payload[b]);
    }

    // Clean up temporary ROM file
    std::remove(tmp_rom.c_str());

    std::cout << "[PASS] TestMemoryBusEepromIntegration" << std::endl;
}

void TestSavePersistence() {
    ngba::Eeprom eeprom;
    eeprom.SetEnabled(true);

    // Test 512B save
    std::vector<std::uint8_t> save512(512, 0x55);
    eeprom.LoadSaveData(save512);
    assert(eeprom.CapacityBytes() == 512);
    assert(eeprom.Data()[0] == 0x55);
    assert(!eeprom.Dirty());

    // Test 8KB save
    std::vector<std::uint8_t> save8k(8192, 0xAA);
    eeprom.LoadSaveData(save8k);
    assert(eeprom.CapacityBytes() == 8192);
    assert(eeprom.Data()[1000] == 0xAA);
    assert(!eeprom.Dirty());

    std::cout << "[PASS] TestSavePersistence" << std::endl;
}

} // namespace

int main() {
    std::cout << "Starting NGBA EEPROM Tests..." << std::endl;
    Test4KbitWriteAndRead();
    Test64KbitWriteAndRead();
    TestAutodetection();
    TestMemoryBusEepromIntegration();
    TestSavePersistence();
    std::cout << "All NGBA EEPROM Tests Passed Successfully!" << std::endl;
    return 0;
}
