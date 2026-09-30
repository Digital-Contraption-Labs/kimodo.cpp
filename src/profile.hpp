#pragma once

#include "log.hpp"

#include <chrono>

namespace kimodo::detail {

// Timings go to the log at debug level (the tools print them to stderr when
// KIMODO_PROFILE is set).
inline bool profile_enabled() noexcept {
    return log_enabled(log_level::debug);
}

inline double profile_elapsed_ms(std::chrono::steady_clock::time_point started) noexcept {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
}

} // namespace kimodo::detail
