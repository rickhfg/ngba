#pragma once

#include <cstdint>
#include <ctime>
#include <array>

namespace ngba {

class Rtc {
public:
    enum class State : std::uint8_t {
        Ready,
        Command,
        Read,
        Write
    };

    Rtc() noexcept;

    void Reset() noexcept;

    bool Enabled() const noexcept { return enabled_; }
    void SetEnabled(bool enabled) noexcept { enabled_ = enabled; }

    std::uint16_t ReadData() const noexcept;
    std::uint16_t ReadDirection() const noexcept { return direction_ & 0x0Fu; }
    std::uint16_t ReadControl() const noexcept { return control_ & 0x01u; }

    void WriteData(std::uint16_t value) noexcept;
    void WriteDirection(std::uint16_t value) noexcept { direction_ = value & 0x0Fu; }
    void WriteControl(std::uint16_t value) noexcept { control_ = value & 0x01u; }

    std::int64_t TimeOffset() const noexcept { return time_offset_seconds_; }
    void SetTimeOffset(std::int64_t offset) noexcept { time_offset_seconds_ = offset; }

    std::uint8_t Status() const noexcept { return status_; }
    void SetStatus(std::uint8_t status) noexcept { status_ = status; }

    // Serialization helper fields
    State GetState() const noexcept { return state_; }
    void SetState(State state) noexcept { state_ = state; }

private:
    void ProcessClockEdge(bool sck_high, bool sio_high) noexcept;
    void CompleteCommand() noexcept;
    void PrepareDateTimeRead() noexcept;
    void PrepareTimeRead() noexcept;
    void ApplyWrittenDateTime() noexcept;
    void ApplyWrittenTime() noexcept;

    bool enabled_{false};
    std::uint16_t data_{0};
    std::uint16_t direction_{0};
    std::uint16_t control_{0};

    State state_{State::Ready};
    std::uint8_t status_{0x40}; // 24-hour mode enabled by default
    std::uint8_t command_{0};
    std::uint8_t bits_transferred_{0};
    std::uint8_t bytes_transferred_{0};
    std::uint8_t transfer_length_{0};
    std::uint8_t current_byte_{0};
    bool sio_out_{false};

    std::array<std::uint8_t, 8> buffer_{};
    std::uint8_t alarm_hour_{0};
    std::uint8_t alarm_minute_{0};
    std::int64_t time_offset_seconds_{0};
};

} // namespace ngba
