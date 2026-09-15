#pragma once

#include "ngba/bus.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ngba {

struct Framebuffer {
    enum : std::size_t {
        kWidth = 240,
        kHeight = 160,
    };

    Framebuffer() : pixels(kWidth * kHeight, 0) {}

    std::vector<std::uint32_t> pixels;
};

class PpuRenderer {
public:
    explicit PpuRenderer(const MemoryBus& bus);

    void Render(Framebuffer& framebuffer) const;
    static void WritePpm(const Framebuffer& framebuffer,
                         const std::string& path);

private:
    struct Pixel {
        std::uint16_t color{};
        std::uint8_t priority{4};
        std::uint8_t layer{5};
        std::uint8_t order{38};
        bool present{};
        bool translucent{};
    };

    static inline void Offer(Pixel& first, Pixel& second, const Pixel& candidate) {
        if (candidate.order < first.order) {
            second = first;
            first = candidate;
        } else if (candidate.order < second.order) {
            second = candidate;
        }
    }

    struct BgConfig {
        std::uint16_t control{};
        std::uint16_t hofs{};
        std::uint16_t vofs{};
        unsigned width_mask{255};
        unsigned height_mask{255};
        unsigned blocks_wide{1};
        unsigned char_base{0};
        unsigned screen_base_block{0};
        std::uint8_t order{38};
        bool enabled{};
        bool text{};
        bool affine{};
        bool bitmap{};
        bool eight_bpp{};
        bool mosaic{};
    };

    struct ScanlinePixel {
        std::uint16_t color;
        bool present;
    };

    void RenderTextBackgroundScanline(const BgConfig& bg,
                                      unsigned screen_y,
                                      std::uint16_t mosaic,
                                      ScanlinePixel* out_line) const;
    bool RenderTextBackgroundPixel(const BgConfig& bg,
                                   unsigned screen_x,
                                   unsigned screen_y,
                                   std::uint16_t mosaic,
                                   std::uint16_t& color) const;
    bool RenderAffineBackgroundPixel(unsigned background, unsigned screen_x,
                                     unsigned screen_y, std::uint16_t control,
                                     unsigned mode, std::uint32_t page,
                                     std::uint16_t mosaic,
                                     std::uint16_t& color) const;
    void RenderObjects(std::uint16_t display_control, std::vector<Pixel>& objects,
                       std::vector<bool>& object_window) const;
    inline std::uint16_t PaletteColor(unsigned palette_index) const {
        const std::size_t offset = palette_index * 2u;
        return offset + 1 < palette_size_
            ? (static_cast<std::uint16_t>(palette_[offset]) | (static_cast<std::uint16_t>(palette_[offset + 1]) << 8))
            : 0;
    }

    inline std::uint16_t ReadIo16(std::size_t offset) const {
        return static_cast<std::uint16_t>(io_[offset]) | (static_cast<std::uint16_t>(io_[offset + 1]) << 8);
    }
    inline std::uint32_t ReadIo32(std::size_t offset) const {
        return static_cast<std::uint32_t>(io_[offset]) |
               (static_cast<std::uint32_t>(io_[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(io_[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(io_[offset + 3]) << 24);
    }
    inline std::uint8_t ReadVram8(std::size_t offset) const {
        return offset < vram_size_ ? vram_[offset] : 0;
    }
    inline std::uint16_t ReadVram16(std::size_t offset) const {
        return offset + 1 < vram_size_
            ? (static_cast<std::uint16_t>(vram_[offset]) | (static_cast<std::uint16_t>(vram_[offset + 1]) << 8))
            : 0;
    }
    inline std::uint32_t ReadVram32(std::size_t offset) const {
        if (offset + 3 < vram_size_) {
            std::uint32_t val;
            std::memcpy(&val, vram_ + offset, 4);
            return val;
        }
        std::uint32_t val = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            if (offset + i < vram_size_) {
                val |= static_cast<std::uint32_t>(vram_[offset + i]) << (i * 8);
            }
        }
        return val;
    }
    inline std::uint16_t ReadOam16(std::size_t offset) const {
        return static_cast<std::uint16_t>(oam_[offset]) | (static_cast<std::uint16_t>(oam_[offset + 1]) << 8);
    }

    const MemoryBus& bus_;
    const std::uint8_t* vram_;
    const std::uint8_t* palette_;
    const std::uint8_t* oam_;
    const std::uint8_t* io_;
    std::size_t vram_size_;
    std::size_t palette_size_;
    mutable std::vector<Pixel> objects_;
    mutable std::vector<bool> object_window_;
};

} // namespace ngba
