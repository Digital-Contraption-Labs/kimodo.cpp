#pragma once

#include <kimodo/kimodo.hpp>

#include <string_view>

namespace kimodo::detail {

// The library's log (kimodo::set_log_sink).  Nothing is formatted unless a
// sink wants the level.
[[nodiscard]] bool log_enabled(log_level level) noexcept;
void log(log_level level, std::string_view line) noexcept;
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void logf(log_level level, const char *format, ...) noexcept;

// Sends ggml's messages through the sink too, which silences them while there
// is no sink.  Idempotent.
void route_ggml_log() noexcept;

} // namespace kimodo::detail
