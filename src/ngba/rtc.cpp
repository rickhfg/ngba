#include "ngba/rtc.hpp"

#include <chrono>

namespace ngba {

namespace {

inline std::uint8_t ToBcd(unsigned value) noexcept {
    return static_cast<std::uint8_t>(((value / 10u) << 4) | (value % 10u));
}

inline unsigned FromBcd(std::uint8_t bcd) noexcept {
    return (((bcd >> 4) & 0x0Fu) * 10u) + (bcd & 0x0Fu);
}

inline std::tm GetLocalTime(std::time_t time) noexcept {
    std::tm result{};
    const std::tm* local = std::localtime(&time);
    if (local != nullptr) {
        result = *local;
    }
    return result;
}

} // namespace

Rtc::Rtc() noexcept {
    Reset();
}

void Rtc::Reset() noexcept {
    data_ = 0;
    direction_ = 0;
    control_ = 0;
    state_ = State::Ready;
    status_ = 0x40; // 24-hour mode, power on bit cleared
    command_ = 0;
    bits_transferred_ = 0;
    bytes_transferred_ = 0;
    transfer_length_ = 0;
    current_byte_ = 0;
    sio_out_ = false;
    buffer_.fill(0);
    alarm_hour_ = 0;
    alarm_minute_ = 0;
}

std::uint16_t Rtc::ReadData() const noexcept {
    std::uint16_t result = data_ & direction_;
    // Pin 1 (SIO) is bit 1: when direction bit 1 is 0 (input to CPU), return RTC's output
    if ((direction_ & 0x02u) == 0) {
        if (sio_out_) {
            result |= 0x02u;
        }
    }
    return result;
}

void Rtc::WriteData(std::uint16_t value) noexcept {
    const bool old_cs = (data_ & 0x04u) != 0;
    const bool old_sck = (data_ & 0x01u) != 0;

    data_ = value & 0x0Fu;

    const bool new_cs = (data_ & 0x04u) != 0;
    const bool new_sck = (data_ & 0x01u) != 0;
    const bool sio_in = (data_ & 0x02u) != 0;

    // CS low: chip deselected, return to Ready
    if (!new_cs) {
        state_ = State::Ready;
        bits_transferred_ = 0;
        bytes_transferred_ = 0;
        transfer_length_ = 0;
        command_ = 0;
        sio_out_ = false;
        return;
    }

    // CS rising edge: start command phase
    if (!old_cs && new_cs) {
        state_ = State::Command;
        command_ = 0;
        bits_transferred_ = 0;
        bytes_transferred_ = 0;
        transfer_length_ = 0;
        sio_out_ = false;
    }

    if (state_ == State::Command) {
        // Clock bit into command on rising edge of SCK (MSB first)
        if (!old_sck && new_sck) {
            command_ = static_cast<std::uint8_t>((command_ << 1) | (sio_in ? 1 : 0));
            ++bits_transferred_;
            if (bits_transferred_ == 8) {
                CompleteCommand();
            }
        }
    } else if (state_ == State::Write) {
        // Clock bit into data buffer on rising edge of SCK (LSB first)
        if (!old_sck && new_sck) {
            current_byte_ |= static_cast<std::uint8_t>((sio_in ? 1 : 0) << bits_transferred_);
            ++bits_transferred_;
            if (bits_transferred_ == 8) {
                if (bytes_transferred_ < buffer_.size()) {
                    buffer_[bytes_transferred_++] = current_byte_;
                }
                bits_transferred_ = 0;
                current_byte_ = 0;
                if (bytes_transferred_ >= transfer_length_) {
                    if (command_ == 0x62u) {
                        status_ = buffer_[0] & 0x7Eu;
                    } else if (command_ == 0x64u) {
                        ApplyWrittenDateTime();
                    } else if (command_ == 0x66u) {
                        ApplyWrittenTime();
                    } else if (command_ == 0x68u) {
                        alarm_hour_ = buffer_[0];
                        alarm_minute_ = buffer_[1];
                    }
                    state_ = State::Ready;
                }
            }
        }
    } else if (state_ == State::Read) {
        // Output current bit and advance on falling edge of SCK (LSB first)
        if (old_sck && !new_sck) {
            if (bytes_transferred_ < transfer_length_) {
                sio_out_ = ((buffer_[bytes_transferred_] >> bits_transferred_) & 1) != 0;
                ++bits_transferred_;
                if (bits_transferred_ == 8) {
                    bits_transferred_ = 0;
                    ++bytes_transferred_;
                }
            } else {
                sio_out_ = false;
            }
        }
    }
}

void Rtc::CompleteCommand() noexcept {
    bits_transferred_ = 0;
    bytes_transferred_ = 0;
    current_byte_ = 0;

    switch (command_) {
    case 0x60u: // CMD_RESET
        status_ = 0x40; // 24-hour mode
        state_ = State::Ready;
        break;

    case 0x62u: // CMD_STATUS (write)
        state_ = State::Write;
        transfer_length_ = 1;
        break;

    case 0x63u: // CMD_STATUS (read)
        state_ = State::Read;
        transfer_length_ = 1;
        buffer_[0] = status_;
        sio_out_ = false;
        break;

    case 0x64u: // CMD_DATETIME (write)
        state_ = State::Write;
        transfer_length_ = 7;
        break;

    case 0x65u: // CMD_DATETIME (read)
        PrepareDateTimeRead();
        state_ = State::Read;
        transfer_length_ = 7;
        sio_out_ = false;
        break;

    case 0x66u: // CMD_TIME (write)
        state_ = State::Write;
        transfer_length_ = 3;
        break;

    case 0x67u: // CMD_TIME (read)
        PrepareTimeRead();
        state_ = State::Read;
        transfer_length_ = 3;
        sio_out_ = false;
        break;

    case 0x68u: // CMD_ALARM (write)
        state_ = State::Write;
        transfer_length_ = 2;
        break;

    case 0x69u: // CMD_ALARM (read)
        state_ = State::Read;
        transfer_length_ = 2;
        buffer_[0] = alarm_hour_;
        buffer_[1] = alarm_minute_;
        sio_out_ = false;
        break;

    default:
        state_ = State::Ready;
        break;
    }
}

void Rtc::PrepareDateTimeRead() noexcept {
    const std::time_t raw_time = std::time(nullptr) + time_offset_seconds_;
    const std::tm tm = GetLocalTime(raw_time);
    buffer_[0] = ToBcd(static_cast<unsigned>(tm.tm_year % 100));
    buffer_[1] = ToBcd(static_cast<unsigned>(tm.tm_mon + 1));
    buffer_[2] = ToBcd(static_cast<unsigned>(tm.tm_mday));
    buffer_[3] = static_cast<std::uint8_t>(tm.tm_wday & 7);
    if ((status_ & 0x40u) != 0) {
        buffer_[4] = ToBcd(static_cast<unsigned>(tm.tm_hour));
    } else {
        const unsigned h = static_cast<unsigned>(tm.tm_hour);
        const bool pm = h >= 12;
        buffer_[4] = ToBcd(h % 12) | (pm ? 0x80u : 0x00u);
    }
    buffer_[5] = ToBcd(static_cast<unsigned>(tm.tm_min));
    buffer_[6] = ToBcd(static_cast<unsigned>(tm.tm_sec));
}

void Rtc::PrepareTimeRead() noexcept {
    const std::time_t raw_time = std::time(nullptr) + time_offset_seconds_;
    const std::tm tm = GetLocalTime(raw_time);
    if ((status_ & 0x40u) != 0) {
        buffer_[0] = ToBcd(static_cast<unsigned>(tm.tm_hour));
    } else {
        const unsigned h = static_cast<unsigned>(tm.tm_hour);
        const bool pm = h >= 12;
        buffer_[0] = ToBcd(h % 12) | (pm ? 0x80u : 0x00u);
    }
    buffer_[1] = ToBcd(static_cast<unsigned>(tm.tm_min));
    buffer_[2] = ToBcd(static_cast<unsigned>(tm.tm_sec));
}

void Rtc::ApplyWrittenTime() noexcept {
    const std::time_t host_now = std::time(nullptr);
    std::tm tm = GetLocalTime(host_now);
    unsigned hour = FromBcd(buffer_[0] & 0x7Fu);
    if ((status_ & 0x40u) == 0 && (buffer_[0] & 0x80u) != 0) {
        hour += 12;
    }
    tm.tm_hour = static_cast<int>(hour % 24);
    tm.tm_min = static_cast<int>(FromBcd(buffer_[1]) % 60);
    tm.tm_sec = static_cast<int>(FromBcd(buffer_[2] & 0x7Fu) % 60);
    const std::time_t desired = std::mktime(&tm);
    if (desired != static_cast<std::time_t>(-1)) {
        time_offset_seconds_ = static_cast<std::int64_t>(desired - host_now);
    }
}

void Rtc::ApplyWrittenDateTime() noexcept {
    const std::time_t host_now = std::time(nullptr);
    std::tm tm = GetLocalTime(host_now);
    const unsigned year_2digit = FromBcd(buffer_[0]);
    tm.tm_year = static_cast<int>(100 + (year_2digit % 100));
    tm.tm_mon = static_cast<int>((FromBcd(buffer_[1]) - 1) % 12);
    tm.tm_mday = static_cast<int>(FromBcd(buffer_[2]));
    unsigned hour = FromBcd(buffer_[4] & 0x7Fu);
    if ((status_ & 0x40u) == 0 && (buffer_[4] & 0x80u) != 0) {
        hour += 12;
    }
    tm.tm_hour = static_cast<int>(hour % 24);
    tm.tm_min = static_cast<int>(FromBcd(buffer_[5]) % 60);
    tm.tm_sec = static_cast<int>(FromBcd(buffer_[6] & 0x7Fu) % 60);
    const std::time_t desired = std::mktime(&tm);
    if (desired != static_cast<std::time_t>(-1)) {
        time_offset_seconds_ = static_cast<std::int64_t>(desired - host_now);
    }
}

} // namespace ngba
