#include "ngba/eeprom.hpp"

#include <algorithm>

namespace ngba {

Eeprom::Eeprom()
    : data_(512, 0xFF) {
}

void Eeprom::SetEnabled(bool enabled) noexcept {
    enabled_ = enabled;
}

bool Eeprom::Enabled() const noexcept {
    return enabled_;
}

void Eeprom::SetSize(EepromSize size) noexcept {
    size_ = size;
    const std::size_t target_size = (size == EepromSize::Eeprom64K) ? 8192u : 512u;
    if (data_.size() < target_size) {
        data_.resize(target_size, 0xFF);
    }
}

EepromSize Eeprom::Size() const noexcept {
    return size_;
}

std::size_t Eeprom::CapacityBytes() const noexcept {
    return (size_ == EepromSize::Eeprom64K) ? 8192u : 512u;
}

bool Eeprom::Dirty() const noexcept {
    return dirty_;
}

void Eeprom::ClearDirty() noexcept {
    dirty_ = false;
}

const std::vector<std::uint8_t>& Eeprom::Data() const noexcept {
    return data_;
}

std::vector<std::uint8_t>& Eeprom::Data() noexcept {
    return data_;
}

void Eeprom::LoadSaveData(const std::vector<std::uint8_t>& data) {
    if (data.empty()) return;
    if (data.size() >= 8192u) {
        SetSize(EepromSize::Eeprom64K);
        std::copy_n(data.begin(), 8192u, data_.begin());
    } else {
        SetSize(EepromSize::Eeprom4K);
        std::copy_n(data.begin(), std::min(data.size(), std::size_t(512u)), data_.begin());
    }
    dirty_ = false;
}

void Eeprom::NotifyDmaTransfer(std::uint32_t count) noexcept {
    dma_count_hint_ = count;
    if (count == 9u || count == 73u) {
        if (size_ == EepromSize::Autodetect) {
            SetSize(EepromSize::Eeprom4K);
        }
    } else if (count == 17u || count == 81u) {
        SetSize(EepromSize::Eeprom64K);
    }
}

void Eeprom::ResetState() noexcept {
    reading_ = false;
    read_bit_index_ = 0;
    read_address_ = 0;
    input_bits_.clear();
    dma_count_hint_ = 0;
}

std::uint8_t Eeprom::SerializedSize() const noexcept {
    return static_cast<std::uint8_t>(size_);
}

void Eeprom::RestoreState(std::uint8_t size, const std::vector<std::uint8_t>& data,
                          bool dirty, bool reading, std::size_t read_bit_index,
                          std::uint32_t read_address,
                          const std::vector<std::uint8_t>& input_bits) {
    size_ = static_cast<EepromSize>(size);
    data_ = data;
    dirty_ = dirty;
    reading_ = reading;
    read_bit_index_ = read_bit_index;
    read_address_ = read_address;
    input_bits_ = input_bits;
}

void Eeprom::CommitWrite(std::size_t address_bits, std::size_t header_len) {
    std::uint32_t addr = 0;
    for (std::size_t i = 2; i < header_len; ++i) {
        addr = (addr << 1) | input_bits_[i];
    }
    if (address_bits == 14u) {
        addr &= 0x3FFu; // 1024 blocks (lower 10 bits)
    } else {
        addr &= 0x3Fu;  // 64 blocks (lower 6 bits)
    }

    const std::size_t base_offset = static_cast<std::size_t>(addr) * 8u;
    if (base_offset + 8u <= data_.size()) {
        for (std::size_t byte = 0; byte < 8u; ++byte) {
            std::uint8_t val = 0;
            for (std::size_t bit = 0; bit < 8u; ++bit) {
                val = static_cast<std::uint8_t>((val << 1) | input_bits_[header_len + byte * 8u + bit]);
            }
            data_[base_offset + byte] = val;
        }
        dirty_ = true;
    }

    input_bits_.clear();
    dma_count_hint_ = 0;
}

void Eeprom::Write(std::uint16_t bit) noexcept {
    const std::uint8_t bit_val = static_cast<std::uint8_t>(bit & 1u);

    if (reading_) {
        // Any write during reading cancels read mode and starts a new command
        reading_ = false;
        read_bit_index_ = 0;
        input_bits_.clear();
    }

    if (input_bits_.empty() && bit_val == 0) {
        // Bus idle line is high; leading 0s before start bit are ignored
        return;
    }

    input_bits_.push_back(bit_val);

    // If writing command (10):
    if (input_bits_.size() >= 2 && input_bits_[0] == 1 && input_bits_[1] == 0) {
        // Check if write stream has reached completion:
        if (size_ == EepromSize::Eeprom4K && input_bits_.size() == 73u) {
            CommitWrite(6u, 8u);
        } else if (size_ == EepromSize::Eeprom64K && input_bits_.size() == 81u) {
            CommitWrite(14u, 16u);
        } else if (size_ == EepromSize::Autodetect) {
            if (dma_count_hint_ == 73u && input_bits_.size() == 73u) {
                SetSize(EepromSize::Eeprom4K);
                CommitWrite(6u, 8u);
            } else if (input_bits_.size() == 81u) {
                SetSize(EepromSize::Eeprom64K);
                CommitWrite(14u, 16u);
            }
        }
    }
}

std::uint16_t Eeprom::Read() noexcept {
    // Check if an autodetected 4K write (73 bits) was waiting for completion
    if (input_bits_.size() == 73u && input_bits_[0] == 1 && input_bits_[1] == 0) {
        SetSize(EepromSize::Eeprom4K);
        CommitWrite(6u, 8u);
        return 1u; // Ready
    }

    if (!reading_) {
        // Check if a Read Request (11) is queued
        if (input_bits_.size() >= 2 && input_bits_[0] == 1 && input_bits_[1] == 1) {
            std::size_t addr_bits = 0;
            if (input_bits_.size() == 9u || dma_count_hint_ == 9u || size_ == EepromSize::Eeprom4K) {
                addr_bits = 6u;
                SetSize(EepromSize::Eeprom4K);
            } else if (input_bits_.size() == 17u || dma_count_hint_ == 17u || size_ == EepromSize::Eeprom64K) {
                addr_bits = 14u;
                SetSize(EepromSize::Eeprom64K);
            }

            if (addr_bits > 0 && input_bits_.size() >= 2u + addr_bits) {
                std::uint32_t addr = 0;
                for (std::size_t i = 2; i < 2u + addr_bits; ++i) {
                    addr = (addr << 1) | input_bits_[i];
                }
                read_address_ = (addr_bits == 14u) ? (addr & 0x3FFu) : (addr & 0x3Fu);
                reading_ = true;
                read_bit_index_ = 0;
                input_bits_.clear();
                dma_count_hint_ = 0;
            }
        }
    }

    if (reading_) {
        // First 4 bits are dummy bits (0)
        if (read_bit_index_ < 4u) {
            ++read_bit_index_;
            return 0u;
        }

        // Next 64 bits are data bits
        const std::size_t data_bit_idx = read_bit_index_ - 4u;
        if (data_bit_idx < 64u) {
            const std::size_t byte_idx = data_bit_idx / 8u;
            const std::size_t bit_in_byte = 7u - (data_bit_idx % 8u);
            const std::size_t mem_offset = static_cast<std::size_t>(read_address_) * 8u + byte_idx;

            std::uint16_t bit_val = 1u;
            if (mem_offset < data_.size()) {
                bit_val = static_cast<std::uint16_t>((data_[mem_offset] >> bit_in_byte) & 1u);
            }

            ++read_bit_index_;
            if (read_bit_index_ >= 68u) {
                reading_ = false;
                read_bit_index_ = 0;
            }
            return bit_val;
        }

        reading_ = false;
        read_bit_index_ = 0;
        return 1u;
    }

    // When idle or after write, data line is pulled high / signals Ready
    return 1u;
}

} // namespace ngba
