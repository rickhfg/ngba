#include "ngba/ppu.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
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

void WriteLe16(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void WriteAscii(std::vector<std::uint8_t>& bytes, std::size_t offset,
                const std::string& value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(value[i]);
    }
}

std::uint8_t Complement(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0xA0; i <= 0xBC; ++i) sum += bytes[i];
    return static_cast<std::uint8_t>(0u - sum - 0x19u);
}

void DisableObjects(ngba::MemoryBus& bus) {
    for (unsigned object = 0; object < 128; ++object) bus.Write16(0x07000000u + object * 8u, 0x200u);
}

void TestTextBackgrounds(const ngba::RomImage& rom) {
    ngba::MemoryBus bus(rom);
    ngba::PpuRenderer renderer(bus);
    ngba::Framebuffer frame;
    bus.Write16(0x04000000u, 0x100u);
    bus.Write16(0x05000002u, 0x001Fu);
    bus.Write16(0x05000004u, 0x03E0u);
    bus.Write16(0x05000006u, 0x7C00u);
    bus.Write16(0x05000008u, 0x7FFFu);
    for (unsigned size = 0; size < 4; ++size) {
        bus.Write16(0x04000008u, static_cast<std::uint16_t>((size << 14) | 4u));
        for (unsigned block = 0; block < 4; ++block) {
            bus.Write16(0x06000000u + block * 0x800u, static_cast<std::uint16_t>(block + 1u));
            bus.Write32(0x06004000u + (block + 1u) * 32u, (block + 1u) * 0x11111111u);
        }
        for (unsigned scroll_y : {0u, 256u}) {
            for (unsigned scroll_x : {0u, 256u}) {
                bus.Write16(0x04000010u, static_cast<std::uint16_t>(scroll_x));
                bus.Write16(0x04000012u, static_cast<std::uint16_t>(scroll_y));
                const unsigned blocks_wide = size == 1 || size == 3 ? 2u : 1u;
                const unsigned blocks_high = size >= 2 ? 2u : 1u;
                const unsigned block = (scroll_y / 256u % blocks_high) * blocks_wide + scroll_x / 256u % blocks_wide;
                const unsigned colors[] = {0xFFu, 0xFF00u, 0xFF0000u, 0xFFFFFFu};
                renderer.Render(frame);
                assert(frame.pixels[0] == colors[block]);
            }
        }
    }
    bus.Write16(0x04000010u, 0);
    bus.Write16(0x04000012u, 0);
    bus.Write16(0x04000008u, 4u);
    bus.Write16(0x05000022u, 0x001Fu);
    bus.Write16(0x06000000u, 0x1C01u);
    bus.Write32(0x06004020u, 0);
    bus.Write8(0x0600403Fu, 0x10u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu);
    assert(frame.pixels[1] == 0);
    bus.Write16(0x04000008u, 0x44u);
    bus.Write16(0x0400004Cu, 0x11u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu && frame.pixels[1] == 0xFFu && frame.pixels[240] == 0xFFu);
    bus.Write16(0x04000008u, 0x84u);
    bus.Write16(0x06000000u, 1);
    bus.Write8(0x06004040u, 3);
    bus.Write8(0x06004041u, 0);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFF0000u && frame.pixels[1] == 0);
}

void TestObjectsAndEffects(const ngba::RomImage& rom) {
    ngba::MemoryBus bus(rom);
    DisableObjects(bus);
    ngba::PpuRenderer renderer(bus);
    ngba::Framebuffer frame;
    bus.Write16(0x04000000u, 0x1100u);
    bus.Write16(0x04000008u, 4u);
    bus.Write16(0x05000000u, 0x03E0u);
    bus.Write16(0x05000002u, 0x7C00u);
    bus.Write16(0x05000202u, 0x001Fu);
    bus.Write16(0x05000204u, 0x7FFFu);
    for (unsigned offset = 0; offset < 32; offset += 4) {
        bus.Write32(0x06004000u + offset, 0x11111111u);
        bus.Write32(0x06010000u + offset, 0x11111111u);
        bus.Write32(0x06010020u + offset, 0x22222222u);
    }
    bus.Write16(0x07000000u, 0);
    bus.Write16(0x07000008u, 0);
    bus.Write16(0x0700000Cu, 1);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu && frame.pixels[8] == 0xFF0000u);
    bus.Write16(0x07000004u, 0x400u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFFFFFu);
    bus.Write16(0x07000008u, 0x200u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFF0000u);
    bus.Write16(0x07000004u, 0);
    bus.Write8(0x06010000u, 0x10u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFF0000u && frame.pixels[1] == 0xFFu);
    bus.Write8(0x06010000u, 0x11u);
    bus.Write16(0x04000050u, 0x150u);
    bus.Write16(0x04000052u, 0x0808u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0x7B007Bu);
    bus.Write16(0x04000050u, 0x100u);
    bus.Write16(0x07000000u, 0x400u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0x7B007Bu);
    bus.Write16(0x04000050u, 0x90u);
    bus.Write16(0x04000054u, 31);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFFFFFu);
    bus.Write16(0x04000050u, 0xD0u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0);
    bus.Write16(0x07000000u, 0);
    bus.Write16(0x04000050u, 0x150u);
    bus.Write16(0x04000000u, 0x3100u);
    bus.Write16(0x04000040u, 0x0004u);
    bus.Write16(0x04000044u, 0x0004u);
    bus.Write16(0x04000048u, 0x11u);
    bus.Write16(0x0400004Au, 0x21u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu && frame.pixels[4] == 0xFF0000u);
    assert(frame.pixels[4 * 240] == 0xFF0000u);
    bus.Write16(0x04000040u, 0xF004u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu && frame.pixels[4] == 0xFF0000u);
    bus.Write16(0x04000000u, 0x9100u);
    bus.Write16(0x07000000u, 0x800u);
    bus.Write16(0x0400004Au, 0x0001u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFF00u && frame.pixels[8] == 0xFF0000u);
    bus.Write16(0x04000000u, 0x1000u);
    bus.Write16(0x04000050u, 0);
    bus.Write16(0x07000000u, 255u);
    bus.Write16(0x07000002u, 511u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu && frame.pixels[7] == 0xFF00u);
    bus.Write16(0x07000000u, 0x200u);
    bus.Write16(0x04000050u, 0xC0u | 0x20u);
    bus.Write16(0x04000054u, 8);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0x8300u);
    bus.Write16(0x04000050u, 0x2060u);
    bus.Write16(0x04000052u, 0x1010u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFF00u);
    bus.Write16(0x04000000u, 0x80u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFFFFFu);
}

void TestAffineAndObjectLayout(const ngba::RomImage& rom) {
    ngba::MemoryBus bus(rom);
    DisableObjects(bus);
    ngba::PpuRenderer renderer(bus);
    ngba::Framebuffer frame;
    bus.Write16(0x04000000u, 0x401u);
    bus.Write16(0x0400000Cu, 4u);
    bus.Write16(0x04000020u, 256);
    bus.Write16(0x04000026u, 256);
    bus.Write16(0x05000002u, 0x1Fu);
    bus.Write8(0x06004000u, 1);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu && frame.pixels[1] == 0);
    bus.Write32(0x04000028u, 0x0FFFFFFFu);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0 && frame.pixels[1] == 0xFFu);
    bus.Write16(0x0400000Cu, 0x2004u);
    bus.Write8(0x06004007u, 1);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu);
    bus.Write16(0x04000000u, 0x802u);
    bus.Write16(0x0400000Eu, 4u);
    bus.Write16(0x04000030u, 256);
    bus.Write16(0x04000036u, 256);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu);
    bus.Write16(0x04000000u, 0x1040u);
    bus.Write16(0x05000202u, 0x1Fu);
    bus.Write16(0x05000204u, 0x03E0u);
    bus.Write16(0x07000000u, 0);
    bus.Write16(0x07000002u, 0x4000u);
    bus.Write8(0x06010000u, 0x11u);
    bus.Write8(0x06010040u, 0x22u);
    bus.Write8(0x06010400u, 0x11u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu && frame.pixels[8 * 240] == 0xFF00u);
    bus.Write16(0x04000000u, 0x1000u);
    renderer.Render(frame);
    assert(frame.pixels[8 * 240] == 0xFFu);
    bus.Write16(0x07000002u, 0x1000u);
    bus.Write8(0x06010003u, 0x20u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFF00u);
    bus.Write16(0x07000000u, 0x2000u);
    bus.Write16(0x07000002u, 0);
    bus.Write16(0x07000004u, 1);
    bus.Write8(0x06010000u, 2);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFF00u);
    bus.Write16(0x07000000u, 0x300u);
    bus.Write16(0x07000004u, 0);
    bus.Write16(0x07000006u, 256);
    bus.Write16(0x0700000Eu, 0);
    bus.Write16(0x07000016u, 0);
    bus.Write16(0x0700001Eu, 256);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0 && frame.pixels[4 * 240 + 4] == 0xFF00u);
    bus.Write16(0x04000000u, 0x1003u);
    bus.Write16(0x07000000u, 0);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0);
    bus.Write16(0x07000004u, 512);
    bus.Write8(0x06014000u, 0x11u);
    renderer.Render(frame);
    assert(frame.pixels[0] == 0xFFu);
}

} // namespace

int main() {
    std::vector<std::uint8_t> bytes(0x200, 0);
    WriteLe32(bytes, 0, 0xEA000032u);
    WriteAscii(bytes, 0xA0, "PPU TEST");
    WriteAscii(bytes, 0xAC, "TEST");
    WriteAscii(bytes, 0xB0, "01");
    bytes[0xB2] = 0x96;
    bytes[0xBD] = Complement(bytes);

    const std::string path = "ngba_ppu_test.gba";
    {
        std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    const ngba::RomImage rom = ngba::RomImage::Load(path);
    TestTextBackgrounds(rom);
    TestObjectsAndEffects(rom);
    TestAffineAndObjectLayout(rom);
    ngba::MemoryBus bus(rom);

    // Mode 0, BG0 enabled, 256x256, 4bpp, screen block 0, char block 1.
    bus.Write16(0x04000000u, 0x0100u);
    bus.Write16(0x04000008u, 0x0004u);
    bus.Write16(0x05000000u, 0x0000u);
    bus.Write16(0x05000002u, 0x001Fu); // red, palette index 1
    bus.Write16(0x06000000u, 0x0001u); // map entry 0 points to tile 1
    bus.Write8(0x06004000u + 32u, 0x01u); // first pixel of tile 1 is palette 1

    ngba::Framebuffer framebuffer;
    ngba::PpuRenderer renderer(bus);
    renderer.Render(framebuffer);

    assert(framebuffer.pixels[0] == 0x000000FFu);
    assert(framebuffer.pixels[1] == 0x00000000u);
    assert(framebuffer.pixels[8] == 0x00000000u);
    assert(framebuffer.pixels[240] == 0x00000000u);

    // Bitmap modes use VRAM directly. Mode 3 is 16bpp and covers the whole
    // surface; mode 4 is 8bpp and selects the alternate page with bit 4.
    ngba::MemoryBus bitmap_bus(rom);
    bitmap_bus.Write16(0x04000000u, 0x0403u);
    bitmap_bus.Write16(0x04000020u, 256);
    bitmap_bus.Write16(0x04000026u, 256);
    bitmap_bus.Write16(0x06000000u, 0x001Fu); // red at (0, 0)
    bitmap_bus.Write16(0x06000002u, 0x03E0u); // green at (1, 0)
    ngba::PpuRenderer bitmap_renderer(bitmap_bus);
    bitmap_renderer.Render(framebuffer);
    assert(framebuffer.pixels[0] == 0x000000FFu);
    assert(framebuffer.pixels[1] == 0x0000FF00u);

    bitmap_bus.Write16(0x05000002u, 0x7C00u); // blue, palette index 1
    bitmap_bus.Write8(0x0600A000u, 1u);        // alternate page, (0, 0)
    bitmap_bus.Write16(0x04000000u, 0x0414u);
    bitmap_renderer.Render(framebuffer);
    assert(framebuffer.pixels[0] == 0x00FF0000u);

    // Mode 5 exposes only 160x128 pixels; the rest remains the backdrop.
    bitmap_bus.Write16(0x0600A000u, 0x001Fu);
    bitmap_bus.Write16(0x04000000u, 0x0415u);
    bitmap_renderer.Render(framebuffer);
    assert(framebuffer.pixels[0] == 0x000000FFu);
    assert(framebuffer.pixels[160] == 0x00000000u);
    assert(framebuffer.pixels[128u * ngba::Framebuffer::kWidth] ==
           0x00000000u);

    // The bus exposes one absolute master timeline. HBlank and line edges
    // must be identical whether time is advanced in a jump or by slices.
    assert(bus.NextEventCycle() == 960u);
    bus.AdvanceTo(959u);
    assert(bus.VCount() == 0);
    assert((bus.Read16(0x04000004u) & 0x0002u) == 0);
    assert(bus.NextEventCycle() == 960u);

    bus.AdvanceTo(960u);
    assert((bus.Read16(0x04000004u) & 0x0002u) != 0);
    assert(bus.NextEventCycle() == 1232u);

    bus.AdvanceTo(1232u);
    assert(bus.VCount() == 1);
    assert((bus.Read16(0x04000004u) & 0x0002u) == 0);

    bus.AdvanceTo(1232u * 160u);
    assert(bus.Frames() == 1);
    assert(bus.VCount() == 160);
    assert((bus.Read16(0x04000004u) & 0x0001u) != 0);

    // DISPSTAT.VBlank is active on lines 160..226, but not on line 227.
    bus.AdvanceTo(1232u * 227u);
    assert(bus.VCount() == 227);
    assert((bus.Read16(0x04000004u) & 0x0001u) == 0);

    bus.AdvanceTo(1232u * 228u);
    assert(bus.Frames() == 1);
    assert(bus.VCount() == 0);
    assert((bus.Read16(0x04000004u) & 0x0007u) == 0x0004u);

    // HBlank DMA is a PPU trigger, not a consequence of enabling the HBlank
    // IRQ. It starts on visible lines and is absent from the VBlank interval.
    ngba::MemoryBus hblank_bus(rom);
    hblank_bus.Write16(0x02000000u, 0xCAFEu);
    hblank_bus.Write32(0x040000B0u, 0x02000000u);
    hblank_bus.Write32(0x040000B4u, 0x03000000u);
    hblank_bus.Write16(0x040000B8u, 1);
    hblank_bus.Write16(0x040000BAu, 0xA000u); // enable + HBlank timing
    hblank_bus.AdvanceTo(960u);
    assert(hblank_bus.DmaActive());
    assert(hblank_bus.Read16(0x04000202u) == 0);
    hblank_bus.AdvanceTo(hblank_bus.NextEventCycle());
    assert(hblank_bus.Read16(0x03000000u) == 0xCAFEu);
    assert(!hblank_bus.DmaActive());

    ngba::MemoryBus repeated_hblank_bus(rom);
    repeated_hblank_bus.Write16(0x02000000u, 0x1234u);
    repeated_hblank_bus.Write32(0x040000B0u, 0x02000000u);
    repeated_hblank_bus.Write32(0x040000B4u, 0x03000000u);
    repeated_hblank_bus.Write16(0x040000B8u, 1);
    repeated_hblank_bus.Write16(0x040000BAu, 0xA200u); // repeat + HBlank
    repeated_hblank_bus.AdvanceTo(1232u * 160u);
    assert(repeated_hblank_bus.DmaTransfers() == 160);
    assert(repeated_hblank_bus.Read32(0x040000B4u) == 0x03000140u);
    repeated_hblank_bus.AdvanceTo(1232u * 160u + 960u);
    assert(repeated_hblank_bus.DmaTransfers() == 160);
    assert(!repeated_hblank_bus.DmaActive());

    for (const bool word : {false, true}) {
        ngba::MemoryBus reload_bus(rom);
        const unsigned unit = word ? 4u : 2u;
        for (unsigned index = 0; index < 4; ++index) {
            reload_bus.Write16(0x02000000u + index * unit, static_cast<std::uint16_t>(0x1100u + index));
        }
        reload_bus.Write32(0x040000B0u, 0x02000000u);
        reload_bus.Write32(0x040000B4u, 0x03000000u);
        reload_bus.Write16(0x040000B8u, 2);
        reload_bus.Write16(0x040000BAu, static_cast<std::uint16_t>(0xA260u | (word ? 0x400u : 0u)));
        reload_bus.AdvanceTo(1000u);
        assert(reload_bus.DmaTransfers() == 1);
        assert(reload_bus.Read16(0x03000000u) == 0x1100u);
        assert(reload_bus.Read16(0x03000000u + unit) == 0x1101u);
        reload_bus.AdvanceTo(2232u);
        assert(reload_bus.DmaTransfers() == 2);
        assert(reload_bus.Read16(0x03000000u) == 0x1102u);
        assert(reload_bus.Read16(0x03000000u + unit) == 0x1103u);
        assert(reload_bus.Read16(0x03000000u + unit * 2u) == 0);
    }

    // VBlank DMA is also independent of the VBlank IRQ bit in DISPSTAT.
    ngba::MemoryBus vblank_bus(rom);
    vblank_bus.Write16(0x02000000u, 0xBEEFu);
    vblank_bus.Write32(0x040000B0u, 0x02000000u);
    vblank_bus.Write32(0x040000B4u, 0x03000000u);
    vblank_bus.Write16(0x040000B8u, 1);
    vblank_bus.Write16(0x040000BAu, 0x9000u); // enable + VBlank timing
    vblank_bus.AdvanceTo(1232u * 160u);
    assert(vblank_bus.DmaActive());
    assert(vblank_bus.Read16(0x04000202u) == 0);
    vblank_bus.AdvanceTo(vblank_bus.NextEventCycle());
    assert(vblank_bus.Read16(0x03000000u) == 0xBEEFu);
    assert(!vblank_bus.DmaActive());

    // Timers run on the same master timeline as the PPU. Their overflow
    // requests IF directly; IE and IME only decide whether the CPU may later
    // accept the request as an IRQ.
    ngba::MemoryBus timer_bus(rom);
    timer_bus.Write16(0x04000100u, 0xFFFEu);
    timer_bus.Write16(0x04000102u, 0x00C1u); // IRQ + enable, /64 prescaler
    assert(timer_bus.NextEventCycle() == 128u);
    timer_bus.AdvanceTo(127u);
    assert(timer_bus.Read16(0x04000100u) == 0xFFFFu);
    assert((timer_bus.Read16(0x04000202u) & 0x0008u) == 0);
    timer_bus.AdvanceTo(128u);
    assert(timer_bus.Read16(0x04000100u) == 0xFFFEu);
    assert((timer_bus.Read16(0x04000202u) & 0x0008u) != 0);
    assert(timer_bus.Read16(0x04000200u) == 0);
    assert(!timer_bus.InterruptRequested());

    // Timer 1 in cascade mode advances only when timer 0 overflows. The
    // second timer's overflow raises its own interrupt source and reloads it.
    ngba::MemoryBus cascade_bus(rom);
    cascade_bus.Write16(0x04000100u, 0xFFFFu);
    cascade_bus.Write16(0x04000104u, 0xFFFEu);
    cascade_bus.Write16(0x04000102u, 0x0080u); // timer 0 enabled
    cascade_bus.Write16(0x04000106u, 0x00C4u); // timer 1 IRQ + enable + cascade
    cascade_bus.AdvanceTo(1u);
    assert(cascade_bus.Read16(0x04000104u) == 0xFFFFu);
    assert((cascade_bus.Read16(0x04000202u) & 0x0010u) == 0);
    cascade_bus.AdvanceTo(2u);
    assert(cascade_bus.Read16(0x04000104u) == 0xFFFEu);
    assert((cascade_bus.Read16(0x04000202u) & 0x0010u) != 0);

    std::remove(path.c_str());
    return 0;
}
