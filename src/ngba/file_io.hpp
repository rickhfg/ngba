#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ngba {

std::vector<std::uint8_t> ReadBinaryFile(const std::string& path, std::size_t maximum_size);
void WriteBinaryFile(const std::string& path, const std::vector<std::uint8_t>& bytes);

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);
#endif

}
