#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace kimodo::detail {

// Paths cross the API as UTF-8 on every platform.  std::filesystem::path's
// narrow constructor and string() use the Windows code page instead, so a
// folder under a user name with an accent would not open.  (ggml's own
// fopen already takes UTF-8.)
inline std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::string utf8_string(const std::filesystem::path &path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

} // namespace kimodo::detail
