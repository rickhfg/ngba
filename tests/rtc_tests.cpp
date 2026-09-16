#include "ngba/rtc.hpp"
#include "ngba/bus.hpp"
#include "ngba/rom.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

void TestPinControl() {
    ngba::Rtc rtc;
    rtc.SetEnabled(true);

    assert(rtc.ReadControl() == 0);
    rtc.WriteControl(1);
    assert(rtc.ReadControl() == 1);
    rtc.WriteControl(0xFE);
    assert(rtc.ReadControl() == 0);

    assert(rtc.ReadDirection() == 0);
    rtc.WriteDirection(0x07);
    assert(rtc.ReadDirection() == 0x07);
    rtc.WriteDirection(0xFF);
    assert(rtc.ReadDirection() == 0x0F);

    std::cout << "[PASS] TestPinControl" << std::endl;
}

// Emulate how SiiRtc in Pokemon Ruby bit-bangs GPIO pins
void SendCommand(ngba::Rtc& rtc, std::uint8_t cmd) {
    // Assert CS (Pin 2) while SCK (Pin 0) is high
    rtc.WriteDirection(0x07); // All out
    rtc.WriteData(0x01);      // SCK=1, CS=0
    rtc.WriteData(0x05);      // SCK=1, CS=1

    // Write command byte (MSB first)
    for (int i = 0; i < 8; ++i) {
        const std::uint16_t bit = (cmd >> (7 - i)) & 1u;
        rtc.WriteData((bit << 1) | 0x04);        // SIO=bit, SCK=0, CS=1
        rtc.WriteData((bit << 1) | 0x04);
        rtc.WriteData((bit << 1) | 0x05);        // SIO=bit, SCK=1, CS=1 (rising edge)
    }
}

void WriteDataByte(ngba::Rtc& rtc, std::uint8_t val) {
    // Write data byte (LSB first)
    for (int i = 0; i < 8; ++i) {
        const std::uint16_t bit = (val >> i) & 1u;
        rtc.WriteData((bit << 1) | 0x04);        // SIO=bit, SCK=0, CS=1
        rtc.WriteData((bit << 1) | 0x04);
        rtc.WriteData((bit << 1) | 0x05);        // SIO=bit, SCK=1, CS=1 (rising edge)
    }
}

std::uint8_t ReadDataByte(ngba::Rtc& rtc) {
    std::uint8_t val = 0;
    // SIO configured as input to CPU
    rtc.WriteDirection(0x05); // Pin 0 (SCK) out, Pin 1 (SIO) in, Pin 2 (CS) out

    // Read data byte (LSB first)
    for (int i = 0; i < 8; ++i) {
        rtc.WriteData(0x04); // SCK=0, CS=1 (falling edge advances bit)
        rtc.WriteData(0x05); // SCK=1, CS=1

        const std::uint16_t data = rtc.ReadData();
        const std::uint8_t bit = static_cast<std::uint8_t>((data >> 1) & 1u);
        val = static_cast<std::uint8_t>((val >> 1) | (bit << 7));
    }
    return val;
}

void EndTransaction(ngba::Rtc& rtc) {
    rtc.WriteData(0x01); // CS=0, SCK=1
    rtc.WriteData(0x01);
}

void TestSiiRtcReset() {
    ngba::Rtc rtc;
    rtc.SetEnabled(true);
    rtc.SetStatus(0x00); // clear 24-hour mode

    // Send CMD_RESET (0x60)
    SendCommand(rtc, 0x60);
    EndTransaction(rtc);

    assert((rtc.Status() & 0x40u) == 0x40u); // 24-hour mode restored
    std::cout << "[PASS] TestSiiRtcReset" << std::endl;
}

void TestSiiRtcStatusReadWrite() {
    ngba::Rtc rtc;
    rtc.SetEnabled(true);

    // Send CMD_STATUS | WR (0x62), write status = 0x4A
    SendCommand(rtc, 0x62);
    WriteDataByte(rtc, 0x4A);
    EndTransaction(rtc);

    assert(rtc.Status() == 0x4A);

    // Send CMD_STATUS | RD (0x63), read status
    SendCommand(rtc, 0x63);
    const std::uint8_t read_back = ReadDataByte(rtc);
    EndTransaction(rtc);

    assert(read_back == 0x4A);
    std::cout << "[PASS] TestSiiRtcStatusReadWrite" << std::endl;
}

void TestSiiRtcTimeWriteAndRead() {
    ngba::Rtc rtc;
    rtc.SetEnabled(true);

    // Send CMD_TIME | WR (0x66), write hour=0x15, min=0x42, sec=0x30
    SendCommand(rtc, 0x66);
    WriteDataByte(rtc, 0x15); // BCD 15
    WriteDataByte(rtc, 0x42); // BCD 42
    WriteDataByte(rtc, 0x30); // BCD 30
    EndTransaction(rtc);

    // Send CMD_TIME | RD (0x67), read back
    SendCommand(rtc, 0x67);
    const std::uint8_t h = ReadDataByte(rtc);
    const std::uint8_t m = ReadDataByte(rtc);
    const std::uint8_t s = ReadDataByte(rtc);
    EndTransaction(rtc);

    assert(h == 0x15);
    assert(m == 0x42);
    assert(s == 0x30 || s == 0x31);

    std::cout << "[PASS] TestSiiRtcTimeWriteAndRead" << std::endl;
}

ngba::RomImage CreateTestRom(const std::string& path, const std::string& signature) {
    std::vector<std::uint8_t> bytes(512, 0);
    bytes[0] = 0x32; bytes[3] = 0xEA; bytes[0xB2] = 0x96;
    if (!signature.empty()) {
        for (std::size_t i = 0; i < signature.size(); ++i) {
            bytes[0x100 + i] = static_cast<std::uint8_t>(signature[i]);
        }
    }
    bytes[0xC4] = 0xAA;
    bytes[0xC5] = 0x55;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.close();
    return ngba::RomImage::Load(path);
}

void TestBusGpioMapping() {
    const auto rom = CreateTestRom("rtc-test.gba", "SIIRTC_V001");
    ngba::MemoryBus bus(rom);

    assert(bus.GetRtc().Enabled());

    // When GPIO read is disabled (Control register = 0): reads return ROM data
    assert(bus.Read8(0x080000C4) == 0xAA);
    assert(bus.Read8(0x080000C5) == 0x55);
    assert(bus.Read16(0x080000C4) == 0x55AA);

    // Enable GPIO read: write 1 to 0x080000C8
    bus.Write16(0x080000C8, 1);
    assert(bus.Read16(0x080000C8) == 1);

    // Write direction: all out (7)
    bus.Write16(0x080000C6, 7);
    assert(bus.Read16(0x080000C6) == 7);

    // Write data: 5 (CS=1, SCK=1)
    bus.Write16(0x080000C4, 5);
    assert(bus.Read16(0x080000C4) == 5);

    std::cout << "[PASS] TestBusGpioMapping" << std::endl;
}

void TestBatterySaveDirtyTracking() {
    const auto rom = CreateTestRom("rtc-test-save.gba", "");
    ngba::MemoryBus bus(rom);

    assert(!bus.SaveMemoryDirty());

    // Write to SRAM
    bus.Write8(0x0E000000, 0x42);
    assert(bus.SaveMemoryDirty());
    assert(bus.Read8(0x0E000000) == 0x42);

    bus.ClearSaveMemoryDirty();
    assert(!bus.SaveMemoryDirty());

    // Test LoadSaveMemory
    std::vector<std::uint8_t> saved_data(64 * 1024, 0x99);
    bus.LoadSaveMemory(saved_data);
    assert(!bus.SaveMemoryDirty());
    assert(bus.Read8(0x0E000000) == 0x99);

    std::cout << "[PASS] TestBatterySaveDirtyTracking" << std::endl;
}

} // namespace

int main() {
    TestPinControl();
    TestSiiRtcReset();
    TestSiiRtcStatusReadWrite();
    TestSiiRtcTimeWriteAndRead();
    TestBusGpioMapping();
    TestBatterySaveDirtyTracking();
    std::remove("rtc-test.gba");
    std::remove("rtc-test-save.gba");
    std::cout << "All RTC tests passed!" << std::endl;
    return 0;
}
