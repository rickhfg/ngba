#include "ngba/file_io.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x00000080
#endif
#endif

namespace ngba {

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
    if (size == 0) throw std::runtime_error("invalid UTF-8 text");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, &result[0], size) == 0) {
        throw std::runtime_error("cannot convert UTF-8 text");
    }
    result.pop_back();
    return result;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size == 0) throw std::runtime_error("invalid Unicode text");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.c_str(), -1, &result[0], size, nullptr, nullptr) == 0) {
        throw std::runtime_error("cannot convert Unicode text");
    }
    result.pop_back();
    return result;
}
#endif

namespace {

#ifdef _WIN32
using File = std::unique_ptr<void, decltype(&CloseHandle)>;

File OpenFile(const std::string& path, bool writing) {
    HANDLE handle = CreateFileW(Utf8ToWide(path).c_str(), writing ? GENERIC_WRITE : GENERIC_READ,
        FILE_SHARE_READ, nullptr, writing ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(std::string(writing ? "cannot write file: " : "cannot open file: ") + path);
    }
    return File(handle, &CloseHandle);
}
#else
using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

File OpenFile(const std::string& path, bool writing) {
    File file(std::fopen(path.c_str(), writing ? "wb" : "rb"), &std::fclose);
    if (!file) throw std::runtime_error(std::string(writing ? "cannot write file: " : "cannot open file: ") + path);
    return file;
}
#endif

}

std::vector<std::uint8_t> ReadBinaryFile(const std::string& path, std::size_t maximum_size) {
    auto file = OpenFile(path, false);
#ifdef _WIN32
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > maximum_size) {
        throw std::runtime_error("invalid or oversized file: " + path);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
    for (std::size_t position = 0; position < bytes.size();) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - position, 1024u * 1024u));
        DWORD read = 0;
        if (!ReadFile(file.get(), bytes.data() + position, count, &read, nullptr) || read == 0) {
            throw std::runtime_error("cannot read file: " + path);
        }
        position += read;
    }
#else
    if (std::fseek(file.get(), 0, SEEK_END) != 0) throw std::runtime_error("cannot seek file: " + path);
    const long size = std::ftell(file.get());
    if (size < 0 || static_cast<unsigned long>(size) > maximum_size) {
        throw std::runtime_error("invalid or oversized file: " + path);
    }
    if (std::fseek(file.get(), 0, SEEK_SET) != 0) throw std::runtime_error("cannot seek file: " + path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && std::fread(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) {
        throw std::runtime_error("cannot read file: " + path);
    }
#endif
    return bytes;
}

void WriteBinaryFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    auto file = OpenFile(path, true);
#ifdef _WIN32
    for (std::size_t position = 0; position < bytes.size();) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - position, 1024u * 1024u));
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data() + position, count, &written, nullptr) || written == 0) {
            throw std::runtime_error("cannot write file: " + path);
        }
        position += written;
    }
    if (!FlushFileBuffers(file.get())) throw std::runtime_error("cannot finish writing file: " + path);
#else
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) {
        throw std::runtime_error("cannot write file: " + path);
    }
    if (std::fclose(file.release()) != 0) throw std::runtime_error("cannot finish writing file: " + path);
#endif
}

}
