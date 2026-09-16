#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ngba {

enum class EepromSize : std::uint8_t {
    Autodetect,
    Eeprom4K,
    Eeprom64K
};

class Eeprom {
public:
    Eeprom();

    void SetEnabled(bool enabled) noexcept;
    bool Enabled() const noexcept;

    void SetSize(EepromSize size) noexcept;
    EepromSize Size() const noexcept;
    std::size_t CapacityBytes() const noexcept;

    bool Dirty() const noexcept;
    void ClearDirty() noexcept;

    const std::vector<std::uint8_t>& Data() const noexcept;
    std::vector<std::uint8_t>& Data() noexcept;
    void LoadSaveData(const std::vector<std::uint8_t>& data);

    void NotifyDmaTransfer(std::uint32_t count) noexcept;

    std::uint16_t Read() noexcept;
    void Write(std::uint16_t bit) noexcept;

    void ResetState() noexcept;

    // Savestate serialization accessors
    std::uint8_t SerializedSize() const noexcept;
    void RestoreState(std::uint8_t size, const std::vector<std::uint8_t>& data,
                      bool dirty, bool reading, std::size_t read_bit_index,
                      std::uint32_t read_address,
                      const std::vector<std::uint8_t>& input_bits);
    bool Reading() const noexcept { return reading_; }
    std::size_t ReadBitIndex() const noexcept { return read_bit_index_; }
    std::uint32_t ReadAddress() const noexcept { return read_address_; }
    const std::vector<std::uint8_t>& InputBits() const noexcept { return input_bits_; }

private:
    void CommitWrite(std::size_t address_bits, std::size_t header_len);

    bool enabled_{false};
    EepromSize size_{EepromSize::Autodetect};
    std::vector<std::uint8_t> data_;
    bool dirty_{false};

    bool reading_{false};
    std::size_t read_bit_index_{0};
    std::uint32_t read_address_{0};
    std::vector<std::uint8_t> input_bits_;
    std::uint32_t dma_count_hint_{0};
};

} // namespace ngba
