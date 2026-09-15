#include "ngba/ppu.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace ngba {
namespace {

constexpr std::uint32_t kIoBase = 0x04000000u;
constexpr std::uint32_t kPaletteBase = 0x05000000u;
constexpr std::uint32_t kVramBase = 0x06000000u;

const std::uint32_t* GetColorTable() {
    static std::uint32_t table[32768];
    static bool initialized = false;
    if (!initialized) {
        for (std::uint32_t color = 0; color < 32768; ++color) {
            const std::uint32_t red = (color & 0x1Fu) * 255u / 31u;
            const std::uint32_t green = ((color >> 5) & 0x1Fu) * 255u / 31u;
            const std::uint32_t blue = ((color >> 10) & 0x1Fu) * 255u / 31u;
            table[color] = red | (green << 8) | (blue << 16);
        }
        initialized = true;
    }
    return table;
}

inline std::uint32_t ExpandColor(std::uint16_t color) {
    return GetColorTable()[color & 0x7FFFu];
}

int SignedReference(std::uint32_t value) {
    return static_cast<int>((value & 0x07FFFFFFu)) -
           static_cast<int>(value & 0x08000000u);
}

bool InsideWindow(unsigned position, std::uint16_t limits) {
    const unsigned start = limits >> 8;
    const unsigned end = limits & 255u;
    return start <= end ? position >= start && position < end
                        : position >= start || position < end;
}

inline std::uint16_t ColorEffect(std::uint16_t first, std::uint16_t second,
                                 unsigned effect, unsigned eva, unsigned evb, unsigned brightness) {
    std::uint16_t result = 0;
    for (unsigned shift : {0u, 5u, 10u}) {
        unsigned channel = (first >> shift) & 31u;
        if (effect == 1) {
            channel = std::min(31u, (channel * eva +
                ((second >> shift) & 31u) * evb) >> 4);
        } else if (effect == 2) {
            channel += ((31u - channel) * brightness) >> 4;
        } else if (effect == 3) {
            channel -= (channel * brightness) >> 4;
        }
        result |= static_cast<std::uint16_t>(channel << shift);
    }
    return result;
}

} // namespace

PpuRenderer::PpuRenderer(const MemoryBus& bus)
    : bus_(bus),
      vram_(bus.Vram().data()),
      palette_(bus.Palette().data()),
      oam_(bus.Oam().data()),
      io_(bus.Io().data()),
      vram_size_(bus.Vram().size()),
      palette_size_(bus.Palette().size()),
      objects_(Framebuffer::kWidth * Framebuffer::kHeight),
      object_window_(Framebuffer::kWidth * Framebuffer::kHeight) {}

bool PpuRenderer::RenderTextBackgroundPixel(const BgConfig& bg,
                                            unsigned screen_x,
                                            unsigned screen_y,
                                            std::uint16_t mosaic,
                                            std::uint16_t& color) const {
    if (bg.mosaic) {
        screen_x -= screen_x % ((mosaic & 15u) + 1u);
        screen_y -= screen_y % (((mosaic >> 4) & 15u) + 1u);
    }
    const unsigned x = (screen_x + bg.hofs) & bg.width_mask;
    const unsigned y = (screen_y + bg.vofs) & bg.height_mask;

    const unsigned block_x = x >> 8;
    const unsigned block_y = y >> 8;
    const unsigned screen_block = bg.screen_base_block +
                                  block_y * bg.blocks_wide + block_x;
    const unsigned tile_x = (x & 0xFFu) >> 3;
    const unsigned tile_y = (y & 0xFFu) >> 3;
    const std::uint32_t map_offset =
        (screen_block * 0x800u + (tile_y * 32u + tile_x) * 2u) & 0xFFFFu;
    const std::uint16_t entry = ReadVram16(map_offset);

    const unsigned tile_number = entry & 0x03FFu;
    const bool hflip = (entry & 0x0400u) != 0;
    const bool vflip = (entry & 0x0800u) != 0;
    const unsigned pixel_x = hflip ? (7u - (x & 7u)) : (x & 7u);
    const unsigned pixel_y = vflip ? (7u - (y & 7u)) : (y & 7u);

    unsigned palette_index = 0;
    if (bg.eight_bpp) {
        const std::uint32_t tile_offset = bg.char_base +
                                           tile_number * 64u + pixel_y * 8u + pixel_x;
        palette_index = ReadVram8(tile_offset);
    } else {
        const std::uint32_t tile_offset = bg.char_base +
                                           tile_number * 32u + pixel_y * 4u +
                                           (pixel_x >> 1);
        const std::uint8_t packed = ReadVram8(tile_offset);
        const unsigned nibble = (pixel_x & 1u) == 0 ? (packed & 0x0Fu)
                                                    : (packed >> 4);
        if (nibble == 0) {
            return false;
        }
        palette_index = ((entry >> 12) & 0x0Fu) * 16u + nibble;
    }

    if (palette_index == 0) {
        return false;
    }
    color = PaletteColor(palette_index);
    return true;
}

void PpuRenderer::RenderTextBackgroundScanline(const BgConfig& bg,
                                              unsigned screen_y,
                                              std::uint16_t mosaic,
                                              ScanlinePixel* out_line) const {
    if (bg.mosaic) {
        for (unsigned screen_x = 0; screen_x < Framebuffer::kWidth; ++screen_x) {
            out_line[screen_x].present = RenderTextBackgroundPixel(bg, screen_x, screen_y, mosaic, out_line[screen_x].color);
        }
        return;
    }

    const unsigned y = (screen_y + bg.vofs) & bg.height_mask;
    const unsigned block_y = y >> 8;
    const unsigned tile_y = (y & 0xFFu) >> 3;
    const unsigned screen_block_y = bg.screen_base_block + block_y * bg.blocks_wide;
    const unsigned y_in_tile = y & 7u;

    unsigned screen_x = 0;
    while (screen_x < Framebuffer::kWidth) {
        const unsigned x = (screen_x + bg.hofs) & bg.width_mask;
        const unsigned block_x = x >> 8;
        const unsigned tile_x = (x & 0xFFu) >> 3;
        const unsigned in_tile_x = x & 7u;
        const unsigned span_len = std::min(8u - in_tile_x, Framebuffer::kWidth - screen_x);

        const unsigned screen_block = screen_block_y + block_x;
        const std::uint32_t map_offset =
            (screen_block * 0x800u + (tile_y * 32u + tile_x) * 2u) & 0xFFFFu;
        const std::uint16_t entry = ReadVram16(map_offset);
        const unsigned tile_number = entry & 0x03FFu;
        const bool hflip = (entry & 0x0400u) != 0;
        const bool vflip = (entry & 0x0800u) != 0;
        const unsigned pixel_y = vflip ? (7u - y_in_tile) : y_in_tile;

        if (bg.eight_bpp) {
            const std::uint32_t tile_offset = bg.char_base +
                                              tile_number * 64u + pixel_y * 8u;
            for (unsigned i = 0; i < span_len; ++i) {
                const unsigned cur_x = in_tile_x + i;
                const unsigned pixel_x = hflip ? (7u - cur_x) : cur_x;
                const unsigned palette_index = ReadVram8(tile_offset + pixel_x);
                if (palette_index != 0) {
                    out_line[screen_x + i].color = PaletteColor(palette_index);
                    out_line[screen_x + i].present = true;
                } else {
                    out_line[screen_x + i].present = false;
                }
            }
        } else {
            const std::uint32_t tile_offset = bg.char_base +
                                              tile_number * 32u + pixel_y * 4u;
            const std::uint32_t packed = ReadVram32(tile_offset);
            if (packed == 0) {
                for (unsigned i = 0; i < span_len; ++i) {
                    out_line[screen_x + i].present = false;
                }
            } else {
                const unsigned palette_bank = ((entry >> 12) & 0x0Fu) * 16u;
                for (unsigned i = 0; i < span_len; ++i) {
                    const unsigned cur_x = in_tile_x + i;
                    const unsigned pixel_x = hflip ? (7u - cur_x) : cur_x;
                    const unsigned nibble = (packed >> (pixel_x * 4u)) & 0x0Fu;
                    if (nibble != 0) {
                        out_line[screen_x + i].color = PaletteColor(palette_bank + nibble);
                        out_line[screen_x + i].present = true;
                    } else {
                        out_line[screen_x + i].present = false;
                    }
                }
            }
        }
        screen_x += span_len;
    }
}

bool PpuRenderer::RenderAffineBackgroundPixel(unsigned background, unsigned screen_x,
                                               unsigned screen_y, std::uint16_t control,
                                               unsigned mode, std::uint32_t page,
                                               std::uint16_t mosaic,
                                               std::uint16_t& color) const {
    if ((control & 0x40u) != 0) {
        screen_x -= screen_x % ((mosaic & 15u) + 1u);
        screen_y -= screen_y % (((mosaic >> 4) & 15u) + 1u);
    }
    const unsigned registers = 0x20u + (background - 2u) * 16u;
    const int horizontal_step = static_cast<std::int16_t>(ReadIo16(registers));
    const int horizontal_line = static_cast<std::int16_t>(ReadIo16(registers + 2u));
    const int vertical_step = static_cast<std::int16_t>(ReadIo16(registers + 4u));
    const int vertical_line = static_cast<std::int16_t>(ReadIo16(registers + 6u));
    int texture_x = (SignedReference(ReadIo32(registers + 8u)) +
        horizontal_step * static_cast<int>(screen_x) + horizontal_line * static_cast<int>(screen_y)) >> 8;
    int texture_y = (SignedReference(ReadIo32(registers + 12u)) +
        vertical_step * static_cast<int>(screen_x) + vertical_line * static_cast<int>(screen_y)) >> 8;
    const bool bitmap = mode >= 3u;
    const int width = bitmap ? (mode == 5u ? 160 : 240) : (128 << (control >> 14));
    const int height = bitmap ? (mode == 5u ? 128 : 160) : width;
    if (!bitmap && (control & 0x2000u) != 0) {
        texture_x &= width - 1;
        texture_y &= height - 1;
    } else if (texture_x < 0 || texture_x >= width || texture_y < 0 || texture_y >= height) {
        return false;
    }
    if (bitmap) {
        const unsigned pixel = static_cast<unsigned>(texture_y * width + texture_x);
        if (mode != 4u) {
            color = ReadVram16(page + pixel * 2u);
            return true;
        }
        const unsigned index = ReadVram8(page + pixel);
        if (index == 0) return false;
        color = PaletteColor(index);
        return true;
    }
    const unsigned map = ((control >> 8) & 31u) * 0x800u +
        static_cast<unsigned>((texture_y / 8) * (width / 8) + texture_x / 8);
    const unsigned tile = ReadVram8(map & 0xFFFFu);
    const unsigned address = ((control >> 2) & 3u) * 0x4000u + tile * 64u +
        static_cast<unsigned>((texture_y & 7) * 8 + (texture_x & 7));
    const unsigned index = ReadVram8(address);
    if (index == 0) return false;
    color = PaletteColor(index);
    return true;
}

void PpuRenderer::RenderObjects(std::uint16_t display_control, std::vector<Pixel>& objects,
                                std::vector<bool>& object_window) const {
    if ((display_control & 0x1000u) == 0) return;
    const unsigned widths[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    const unsigned heights[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    const unsigned mosaic = ReadIo16(0x4Cu);
    for (unsigned object = 0; object < 128; ++object) {
        const unsigned address = object * 8u;
        const unsigned attributes = ReadOam16(address);
        const unsigned position = ReadOam16(address + 2u);
        const unsigned tile_attributes = ReadOam16(address + 4u);
        const unsigned shape = attributes >> 14;
        const unsigned kind = (attributes >> 10) & 3u;
        const bool affine = (attributes & 0x100u) != 0;
        if (shape == 3u || kind == 3u || (!affine && (attributes & 0x200u) != 0)) continue;
        const unsigned width = widths[shape][position >> 14];
        const unsigned height = heights[shape][position >> 14];
        const bool double_size = affine && (attributes & 0x200u) != 0;
        const unsigned bound_width = double_size ? width * 2u : width;
        const unsigned bound_height = double_size ? height * 2u : height;
        const bool eight_bpp = (attributes & 0x2000u) != 0;
        const unsigned tile_units = eight_bpp ? 2u : 1u;
        const unsigned first_tile = tile_attributes & (eight_bpp ? 0x3FEu : 0x3FFu);
        const unsigned row_stride = (display_control & 0x40u) != 0 ? width / 8u * tile_units : 32u;
        const unsigned priority = (tile_attributes >> 10) & 3u;
        const unsigned matrix = 6u + ((position >> 9) & 31u) * 32u;
        const int horizontal_step = affine ? static_cast<std::int16_t>(ReadOam16(matrix)) : 256;
        const int horizontal_line = affine ? static_cast<std::int16_t>(ReadOam16(matrix + 8u)) : 0;
        const int vertical_step = affine ? static_cast<std::int16_t>(ReadOam16(matrix + 16u)) : 0;
        const int vertical_line = affine ? static_cast<std::int16_t>(ReadOam16(matrix + 24u)) : 256;
        for (unsigned local_y = 0; local_y < bound_height; ++local_y) {
            const unsigned screen_y = ((attributes & 255u) + local_y) & 255u;
            if (screen_y >= Framebuffer::kHeight) continue;
            for (unsigned local_x = 0; local_x < bound_width; ++local_x) {
                const unsigned screen_x = ((position & 511u) + local_x) & 511u;
                if (screen_x >= Framebuffer::kWidth) continue;
                int texture_x = static_cast<int>(local_x);
                int texture_y = static_cast<int>(local_y);
                if ((attributes & 0x1000u) != 0) {
                    texture_x -= texture_x % static_cast<int>(((mosaic >> 8) & 15u) + 1u);
                    texture_y -= texture_y % static_cast<int>(((mosaic >> 12) & 15u) + 1u);
                }
                if (affine) {
                    const int centered_x = texture_x - static_cast<int>(bound_width / 2u);
                    const int centered_y = texture_y - static_cast<int>(bound_height / 2u);
                    texture_x = ((horizontal_step * centered_x + horizontal_line * centered_y) >> 8) +
                        static_cast<int>(width / 2u);
                    texture_y = ((vertical_step * centered_x + vertical_line * centered_y) >> 8) +
                        static_cast<int>(height / 2u);
                } else {
                    if ((position & 0x1000u) != 0) texture_x = static_cast<int>(width) - 1 - texture_x;
                    if ((position & 0x2000u) != 0) texture_y = static_cast<int>(height) - 1 - texture_y;
                }
                if (texture_x < 0 || texture_x >= static_cast<int>(width) ||
                    texture_y < 0 || texture_y >= static_cast<int>(height)) continue;
                const unsigned tile = (first_tile + static_cast<unsigned>(texture_y / 8) * row_stride +
                    static_cast<unsigned>(texture_x / 8) * tile_units) & 1023u;
                if ((display_control & 7u) >= 3u && tile < 512u) continue;
                const unsigned pixel = static_cast<unsigned>((texture_y & 7) * 8 + (texture_x & 7));
                const unsigned packed = ReadVram8(0x10000u + tile * 32u +
                    (eight_bpp ? pixel : pixel / 2u));
                const unsigned index = eight_bpp ? packed : ((packed >> ((pixel & 1u) * 4u)) & 15u);
                if (index == 0) continue;
                const unsigned output = screen_y * Framebuffer::kWidth + screen_x;
                if (kind == 2u) {
                    object_window[output] = true;
                    continue;
                }
                auto& candidate = objects[output];
                if (candidate.present && priority >= candidate.priority) continue;
                candidate.color = PaletteColor(256u + index + (eight_bpp ? 0u : (tile_attributes >> 12) * 16u));
                candidate.priority = priority;
                candidate.layer = 4;
                candidate.order = priority * 8u;
                candidate.present = true;
                candidate.translucent = kind == 1u;
            }
        }
    }
}

void PpuRenderer::Render(Framebuffer& framebuffer) const {
    framebuffer.pixels.resize(Framebuffer::kWidth * Framebuffer::kHeight);
    const std::uint16_t display_control = ReadIo16(0);
    const unsigned mode = display_control & 7u;
    const std::uint16_t backdrop = PaletteColor(0);
    if ((display_control & 0x80u) != 0) {
        std::fill(framebuffer.pixels.begin(), framebuffer.pixels.end(), 0xFFFFFFu);
        return;
    }

    const bool objects_enabled = (display_control & 0x1000u) != 0;
    if (objects_enabled) {
        std::fill(objects_.begin(), objects_.end(), Pixel{});
        std::fill(object_window_.begin(), object_window_.end(), false);
        RenderObjects(display_control, objects_, object_window_);
    }

    const unsigned blend = ReadIo16(0x50u);
    const unsigned alpha = ReadIo16(0x52u);
    const unsigned eva = std::min(alpha & 31u, 16u);
    const unsigned evb = std::min((alpha >> 8) & 31u, 16u);
    const unsigned brightness = std::min(ReadIo16(0x54u) & 31u, 16u);
    const unsigned window_inside = ReadIo16(0x48u);
    const unsigned window_outside = ReadIo16(0x4Au);
    const unsigned win0h = ReadIo16(0x40u);
    const unsigned win1h = ReadIo16(0x42u);
    const unsigned win0v = ReadIo16(0x44u);
    const unsigned win1v = ReadIo16(0x46u);
    const unsigned mosaic = ReadIo16(0x4Cu);
    const unsigned page = mode != 3u && (display_control & 0x10u) != 0 ? 0xA000u : 0u;

    BgConfig bg_data[4];
    for (unsigned bg = 0; bg < 4; ++bg) {
        bg_data[bg].enabled = (display_control & (0x100u << bg)) != 0;
        if (bg_data[bg].enabled) {
            const std::uint16_t control = ReadIo16(8u + bg * 2u);
            bg_data[bg].control = control;
            bg_data[bg].hofs = static_cast<std::uint16_t>(ReadIo16(0x10u + bg * 4u) & 0x01FFu);
            bg_data[bg].vofs = static_cast<std::uint16_t>(ReadIo16(0x12u + bg * 4u) & 0x01FFu);
            bg_data[bg].text = mode == 0u || (mode == 1u && bg < 2u);
            bg_data[bg].affine = (mode == 1u && bg == 2u) || (mode == 2u && bg >= 2u);
            bg_data[bg].bitmap = mode >= 3u && mode <= 5u && bg == 2u;
            bg_data[bg].eight_bpp = (control & 0x0080u) != 0;
            bg_data[bg].mosaic = (control & 0x0040u) != 0;
            bg_data[bg].char_base = ((control >> 2) & 0x03u) * 0x4000u;
            bg_data[bg].screen_base_block = (control >> 8) & 0x1Fu;
            const unsigned size_code = (control >> 14) & 0x03u;
            const unsigned width = (size_code == 0u || size_code == 2u) ? 256u : 512u;
            const unsigned height = (size_code == 0u || size_code == 1u) ? 256u : 512u;
            bg_data[bg].width_mask = width - 1u;
            bg_data[bg].height_mask = height - 1u;
            bg_data[bg].blocks_wide = width / 256u;
            bg_data[bg].order = static_cast<std::uint8_t>((control & 3u) * 8u + (bg + 1u));
        }
    }

    const auto* color_table = GetColorTable();

    for (unsigned screen_y = 0; screen_y < Framebuffer::kHeight; ++screen_y) {
        ScanlinePixel bg_pixels[4][Framebuffer::kWidth];
        for (unsigned bg = 0; bg < 4; ++bg) {
            if (bg_data[bg].enabled && bg_data[bg].text) {
                RenderTextBackgroundScanline(bg_data[bg], screen_y, mosaic, bg_pixels[bg]);
            }
        }

        const bool in_win0_y = InsideWindow(screen_y, win0v);
        const bool in_win1_y = InsideWindow(screen_y, win1v);

        for (unsigned screen_x = 0; screen_x < Framebuffer::kWidth; ++screen_x) {
            const unsigned output = screen_y * Framebuffer::kWidth + screen_x;
            unsigned mask = 0x3Fu;
            if ((display_control & 0xE000u) != 0) {
                mask = window_outside;
                if ((display_control & 0x8000u) != 0 && objects_enabled && object_window_[output]) mask = window_outside >> 8;
                if (in_win1_y && (display_control & 0x4000u) != 0 && InsideWindow(screen_x, win1h)) {
                    mask = window_inside >> 8;
                }
                if (in_win0_y && (display_control & 0x2000u) != 0 && InsideWindow(screen_x, win0h)) {
                    mask = window_inside;
                }
            }
            Pixel first;
            first.color = backdrop;
            first.order = 38;
            first.layer = 5;
            first.present = true;
            first.translucent = false;
            Pixel second;
            second.order = 38;
            second.layer = 5;
            second.present = false;
            second.translucent = false;

            for (unsigned background = 0; background < 4; ++background) {
                const auto& bg = bg_data[background];
                if (!bg.enabled || (mask & (1u << background)) == 0) continue;
                if (bg.order >= second.order) continue;
                Pixel candidate;
                if (bg.text) {
                    if (!bg_pixels[background][screen_x].present) continue;
                    candidate.color = bg_pixels[background][screen_x].color;
                } else if (bg.affine || bg.bitmap) {
                    if (!RenderAffineBackgroundPixel(background, screen_x, screen_y, bg.control, mode, page, mosaic, candidate.color)) continue;
                } else {
                    continue;
                }
                candidate.order = bg.order;
                candidate.layer = background;
                candidate.present = true;
                candidate.translucent = false;
                Offer(first, second, candidate);
            }
            if (objects_enabled && (mask & 0x10u) != 0 && objects_[output].present) {
                Offer(first, second, objects_[output]);
            }
            std::uint16_t color = first.color;
            if ((mask & 0x20u) != 0) {
                const bool target = (blend & (1u << first.layer)) != 0;
                const bool second_target = second.present && (blend & (0x100u << second.layer)) != 0;
                const unsigned effect = (blend >> 6) & 3u;
                if ((first.translucent || (target && effect == 1u)) && second_target) {
                    color = ColorEffect(first.color, second.color, 1, eva, evb, brightness);
                } else if (target && effect >= 2u) {
                    color = ColorEffect(first.color, 0, effect, eva, evb, brightness);
                }
            }
            framebuffer.pixels[output] = color_table[color & 0x7FFFu];
        }
    }
}

void PpuRenderer::WritePpm(const Framebuffer& framebuffer,
                           const std::string& path) {
    if (framebuffer.pixels.size() != Framebuffer::kWidth * Framebuffer::kHeight) {
        throw std::invalid_argument("framebuffer has an invalid size");
    }

    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open framebuffer output: " + path);
    }
    output << "P6\n" << Framebuffer::kWidth << " " << Framebuffer::kHeight
           << "\n255\n";
    for (std::size_t i = 0; i < framebuffer.pixels.size(); ++i) {
        const std::uint32_t color = framebuffer.pixels[i];
        const char bytes[3] = {
            static_cast<char>(color & 0xFFu),
            static_cast<char>((color >> 8) & 0xFFu),
            static_cast<char>((color >> 16) & 0xFFu),
        };
        output.write(bytes, sizeof(bytes));
    }
    if (!output) {
        throw std::runtime_error("failed while writing framebuffer output: " + path);
    }
}

} // namespace ngba
