#include "ngba/apu.hpp"
#include "ngba/bus.hpp"
#include "ngba/rom.hpp"

#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>

namespace {

class MockAudioSink : public ngba::AudioSink {
public:
    void SubmitSamples(const std::int16_t* stereo_samples, std::size_t frame_count) override {
        total_frames_ += frame_count;
        for (std::size_t i = 0; i < frame_count * 2; ++i) {
            samples_.push_back(stereo_samples[i]);
        }
    }

    void Clear() {
        total_frames_ = 0;
        samples_.clear();
    }

    std::size_t total_frames_{0};
    std::vector<std::int16_t> samples_;
};

void TestReset() {
    ngba::Apu apu;
    assert(apu.SoundCntL() == 0);
    assert(apu.SoundCntH() == 0);
    assert(apu.SoundCntX() == 0);
    assert(apu.SoundBias() == 0x0200);
    assert(apu.FifoCountA() == 0);
    assert(apu.FifoCountB() == 0);
    assert(apu.CurrentSampleA() == 0);
    assert(apu.CurrentSampleB() == 0);
    std::cout << "[PASS] TestReset" << std::endl;
}

void TestIoReadWrite() {
    ngba::Apu apu;

    // Test SOUNDCNT_L write and read
    apu.Write16(0x080, 0x770F);
    assert(apu.Read16(0x080) == 0x770F);
    assert(apu.Read8(0x080) == 0x0F);
    assert(apu.Read8(0x081) == 0x77);

    // Test SOUNDCNT_H write and read
    apu.Write16(0x082, 0x0F02);
    assert(apu.Read16(0x082) == 0x0F02);

    // Test SOUNDBIAS write and read
    apu.Write16(0x088, 0x03FF);
    assert(apu.Read16(0x088) == 0x03FF);

    // Test SOUNDCNT_X master enable
    apu.Write8(0x084, 0x80); // Enable sound
    assert((apu.Read8(0x084) & 0x80) != 0);

    // Disabling master sound clears registers and resets APU
    apu.Write8(0x084, 0x00);
    assert(apu.Read16(0x080) == 0);
    assert(apu.Read16(0x082) == 0);
    assert(apu.Read16(0x084) == 0);

    std::cout << "[PASS] TestIoReadWrite" << std::endl;
}

void TestDirectSoundFifo() {
    ngba::Apu apu;

    // Byte writes to FIFO A
    apu.Write8(0x0A0, 0x10);
    apu.Write8(0x0A1, 0x20);
    assert(apu.FifoCountA() == 2);

    // 16-bit write to FIFO A
    apu.Write16(0x0A0, 0x3412);
    assert(apu.FifoCountA() == 4);

    // 32-bit write to FIFO A
    apu.Write32(0x0A0, 0x78563412);
    assert(apu.FifoCountA() == 8);

    // Byte writes to FIFO B
    apu.Write8(0x0A4, 0xAA);
    assert(apu.FifoCountB() == 1);
    apu.Write32(0x0A4, 0x01020304);
    assert(apu.FifoCountB() == 5);

    // Fill FIFO A up to capacity (32 bytes)
    for (int i = 0; i < 6; ++i) {
        apu.Write32(0x0A0, 0x11223344);
    }
    // 8 + 24 = 32 bytes
    assert(apu.FifoCountA() == 32);

    // Exceeding capacity should drop further pushes
    apu.Write8(0x0A0, 0x99);
    assert(apu.FifoCountA() == 32);

    // Reset FIFO A via SOUNDCNT_H bit 11
    apu.Write16(0x082, 0x0800);
    assert(apu.FifoCountA() == 0);

    // Reset FIFO B via SOUNDCNT_H bit 15
    apu.Write16(0x082, 0x8000);
    assert(apu.FifoCountB() == 0);

    std::cout << "[PASS] TestDirectSoundFifo" << std::endl;
}

void TestTimerStepping() {
    ngba::Apu apu;

    // SOUNDCNT_H:
    // Timer 0 for FIFO A (bit 10 = 0)
    // Timer 1 for FIFO B (bit 14 = 1)
    apu.Write16(0x082, 0x4000);

    // Push 20 bytes into FIFO A (5 words)
    for (int i = 0; i < 5; ++i) {
        apu.Write32(0x0A0, 0x04030201);
    }
    assert(apu.FifoCountA() == 20);

    // Stepping Timer 1 should NOT affect FIFO A
    unsigned dma = apu.StepTimer(1);
    assert((dma & 1u) == 0);
    assert(apu.FifoCountA() == 20);

    // Step Timer 0 3 times: 20 -> 19 -> 18 -> 17 (all > 16, so no DMA)
    for (int i = 0; i < 3; ++i) {
        dma = apu.StepTimer(0);
        assert((dma & 1u) == 0);
    }
    assert(apu.FifoCountA() == 17);

    // 4th pop: count becomes 16, which is <= 16! Should request DMA 1
    dma = apu.StepTimer(0);
    assert((dma & 1u) != 0);
    assert(apu.FifoCountA() == 16);
    assert(apu.CurrentSampleA() == 0x04);

    // Test FIFO B timer 1
    apu.Write32(0x0A4, 0x0B0A0908);
    assert(apu.FifoCountB() == 4);
    // Pop on timer 1: count becomes 3 (<= 16), should request DMA 2
    dma = apu.StepTimer(1);
    assert((dma & 2u) != 0);
    assert(apu.FifoCountB() == 3);
    assert(apu.CurrentSampleB() == 0x08);

    std::cout << "[PASS] TestTimerStepping" << std::endl;
}

void TestDirectSoundSampling() {
    ngba::Apu apu;
    MockAudioSink sink;
    apu.SetAudioSink(&sink);

    // SOUNDCNT_X: Master Enable
    apu.Write8(0x084, 0x80);

    // SOUNDCNT_H:
    // Bit 2: DirectSound A 100% volume
    // Bit 3: DirectSound B 100% volume
    // Bit 8: DirectSound A Right enable
    // Bit 9: DirectSound A Left enable
    // Bit 12: DirectSound B Right enable
    // Bit 13: DirectSound B Left enable
    apu.Write16(0x082, 0x330C);

    // Push known sample into FIFO A and step timer 0 to make it the current sample
    apu.Write8(0x0A0, 0x40); // Signed 64
    apu.StepTimer(0);
    assert(apu.CurrentSampleA() == 0x40);

    // Advance by ~1 frame (280,896 cycles)
    apu.AdvanceTo(280896);
    apu.FlushFrame();

    assert(sink.total_frames_ > 0);
    // Check that samples are non-zero and converge to 12288 (48 * 256)
    bool non_zero = false;
    for (auto sample : sink.samples_) {
        if (sample != 0) {
            non_zero = true;
            assert(sample > 0 && sample <= 12288);
        }
    }
    assert(non_zero);
    assert(sink.samples_.back() == 12288);

    std::cout << "[PASS] TestDirectSoundSampling" << std::endl;
}

ngba::RomImage CreateTestRom(const std::string& path) {
    std::vector<std::uint8_t> bytes(384, 0);
    bytes[0x0A0] = 'N';
    bytes[0x0A1] = 'G';
    bytes[0x0A2] = 'B';
    bytes[0x0A3] = 'A';
    bytes[0x0AC] = 'T';
    bytes[0x0AD] = 'E';
    bytes[0x0AE] = 'S';
    bytes[0x0AF] = 'T';
    bytes[0x0B2] = 0x96;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return ngba::RomImage::Load(path);
}

void TestBusDmaIntegration() {
    const auto rom = CreateTestRom("apu-test.gba");
    ngba::MemoryBus bus(rom);

    // Write test audio data in EWRAM at 0x02000000
    for (std::uint32_t i = 0; i < 16; ++i) {
        bus.Write8(0x02000000 + i, static_cast<std::uint8_t>(i + 1));
    }

    // Configure DMA channel 1:
    // Source: 0x02000000
    // Dest: 0x040000A0 (FIFO A)
    // Count: 4 (ignored by sound DMA, but set anyway)
    // Control: 0xB600:
    //   Bit 15: Enable (1)
    //   Bits 12-13: Timing 3 (Sound FIFO)
    //   Bit 10: 32-bit (1)
    //   Bit 9: Repeat (1)
    //   Bits 5-6: Dest Fixed (2)
    bus.Write32(0x040000BC, 0x02000000); // DMA1SAD
    bus.Write32(0x040000C0, 0x040000A0); // DMA1DAD
    bus.Write16(0x040000C4, 4);          // DMA1CNT_L
    bus.Write16(0x040000C6, 0xB640);     // DMA1CNT_H: Enable, Timing 3, 32-bit, Repeat, Dest Fixed

    // Configure SOUNDCNT_H: FIFO A on Timer 0, reset FIFO A
    bus.Write16(0x04000082, 0x0800); // Reset FIFO A
    assert(bus.GetApu().FifoCountA() == 0);

    // Set Timer 0 reload to 0xFF00 (overflows every 256 cycles)
    bus.Write16(0x04000100, 0xFF00);
    bus.Write16(0x04000102, 0x0080); // Prescaler 1, enabled
    bus.AdvanceTo(300);

    // After timer overflowed once and triggered DMA, FIFO A should have received 16 bytes!
    assert(bus.GetApu().FifoCountA() == 16);

    std::cout << "[PASS] TestBusDmaIntegration" << std::endl;
}

void TestSound1SweepAndDuty() {
    ngba::Apu apu;
    apu.Write8(0x084, 0x80); // Master sound enable

    // SOUND1CNT_L: sweep shift 1, dec (1), period 2
    apu.Write16(0x060, 0x0029);
    // SOUND1CNT_H: duty 2 (50%), vol 15, length 32
    apu.Write16(0x062, 0xF080 | 32);
    // SOUND1CNT_X: trigger (bit 15), length enable (bit 14), freq 1000
    apu.Write16(0x064, 0xC000 | 1000);

    assert(apu.Channel1Active());
    assert((apu.SoundCntX() & 1u) != 0);

    // Advance 65536 cycles (2 frame sequencer steps)
    apu.AdvanceTo(65536);
    assert(apu.Channel1Active());

    std::cout << "[PASS] TestSound1SweepAndDuty" << std::endl;
}

void TestSound2Envelope() {
    ngba::Apu apu;
    apu.Write8(0x084, 0x80);

    // SOUND2CNT_L: duty 1 (25%), vol 10, envelope period 1, decrease
    apu.Write16(0x068, 0xA140);
    // SOUND2CNT_H: trigger, freq 800
    apu.Write16(0x06C, 0x8000 | 800);

    assert(apu.Channel2Active());
    assert((apu.SoundCntX() & 2u) != 0);

    apu.AdvanceTo(100000);
    assert(apu.Channel2Active());

    std::cout << "[PASS] TestSound2Envelope" << std::endl;
}

void TestSound3Wave() {
    ngba::Apu apu;
    apu.Write8(0x084, 0x80);

    // Fill wave RAM with 0x01, 0x23, ...
    for (std::uint32_t i = 0; i < 16; ++i) {
        apu.Write8(0x090 + i, static_cast<std::uint8_t>(i * 17));
        assert(apu.Read8(0x090 + i) == static_cast<std::uint8_t>(i * 17));
    }

    // Enable channel 3
    apu.Write8(0x070, 0x80);
    // Volume 100% (code 1)
    apu.Write16(0x072, 0x2000);
    // Trigger
    apu.Write16(0x074, 0x8000 | 1200);

    assert(apu.Channel3Active());
    assert((apu.SoundCntX() & 4u) != 0);

    std::cout << "[PASS] TestSound3Wave" << std::endl;
}

void TestSound4Noise() {
    ngba::Apu apu;
    apu.Write8(0x084, 0x80);

    // SOUND4CNT_L: volume 12
    apu.Write16(0x078, 0xC000);
    // SOUND4CNT_H: trigger, ratio 1
    apu.Write16(0x07C, 0x8001);

    assert(apu.Channel4Active());
    assert((apu.SoundCntX() & 8u) != 0);

    std::cout << "[PASS] TestSound4Noise" << std::endl;
}

void TestRestoreFromIo() {
    ngba::Apu apu;
    std::array<std::uint8_t, 1024> io{};

    // Test backward compatibility recovery
    apu.RestoreFromIo(io, true); // sound DMA was active, but io is zero
    assert((apu.SoundCntX() & 0x80u) != 0);
    assert(apu.SoundCntH() == 0x3302);
    assert(apu.SoundCntL() == 0xFF77);
    assert(apu.SoundBias() == 0x0200);

    // Test normal restore from populated io
    io[0x084] = 0x80;
    io[0x080] = 0x55;
    io[0x081] = 0xAA;
    io[0x082] = 0x02;
    io[0x083] = 0x33;
    io[0x088] = 0x00;
    io[0x089] = 0x02;
    apu.RestoreFromIo(io, false);
    assert((apu.SoundCntX() & 0x80u) != 0);
    assert(apu.SoundCntL() == 0xAA55);
    assert(apu.SoundCntH() == 0x3302);
    assert(apu.SoundBias() == 0x0200);

    std::cout << "[PASS] TestRestoreFromIo" << std::endl;
}

} // namespace

int main() {
    TestReset();
    TestIoReadWrite();
    TestDirectSoundFifo();
    TestTimerStepping();
    TestDirectSoundSampling();
    TestBusDmaIntegration();
    TestSound1SweepAndDuty();
    TestSound2Envelope();
    TestSound3Wave();
    TestSound4Noise();
    TestRestoreFromIo();
    std::remove("apu-test.gba");
    std::cout << "All APU tests passed!" << std::endl;
    return 0;
}
