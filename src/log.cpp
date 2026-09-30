#include "log.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#ifdef KIMODO_HAVE_GGML
#include <ggml.h>
#endif

namespace kimodo {
namespace {
std::mutex sink_mutex;
log_sink current_sink;
// Nothing passes until a sink is set.
std::atomic<int> minimum_level{INT_MAX};
} // namespace

void set_log_sink(log_sink sink, log_level minimum) {
    detail::route_ggml_log();
    const std::scoped_lock lock(sink_mutex);
    current_sink = std::move(sink);
    minimum_level.store(current_sink ? static_cast<int>(minimum) : INT_MAX);
}

namespace detail {

bool log_enabled(log_level level) noexcept {
    return static_cast<int>(level) >= minimum_level.load(std::memory_order_relaxed);
}

void log(log_level level, std::string_view line) noexcept {
    if (!log_enabled(level)) return;
    try {
        // Called outside the lock, so a sink may itself set the sink.
        log_sink sink;
        {
            const std::scoped_lock lock(sink_mutex);
            sink = current_sink;
        }
        if (sink) sink(level, line);
    } catch (...) {
        // A host's logger must not take a generation down.
    }
}

void logf(log_level level, const char *format, ...) noexcept {
    if (!log_enabled(level)) return;
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
    if (written < 0) return;
    log(level, std::string_view(buffer, std::min(static_cast<size_t>(written), sizeof buffer - 1)));
}

#ifdef KIMODO_HAVE_GGML
namespace {
log_level from_ggml(ggml_log_level level) noexcept {
    switch (level) {
    case GGML_LOG_LEVEL_DEBUG: return log_level::debug;
    case GGML_LOG_LEVEL_WARN: return log_level::warning;
    case GGML_LOG_LEVEL_ERROR: return log_level::error;
    default: return log_level::info;
    }
}

// ggml writes a line in pieces, GGML_LOG_LEVEL_CONT continuing the last
// level, so lines are assembled per thread before they go to the sink.
void ggml_to_sink(ggml_log_level level, const char *text, void *) {
    thread_local std::string pending;
    thread_local log_level pending_level = log_level::info;
    if (level != GGML_LOG_LEVEL_CONT) pending_level = from_ggml(level);
    if (!text) return;
    try {
        pending += text;
        for (size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
            log(pending_level, std::string_view(pending).substr(0, end));
            pending.erase(0, end + 1);
        }
    } catch (...) {
        pending.clear();
    }
}
} // namespace

void route_ggml_log() noexcept {
    static std::once_flag once;
    try {
        std::call_once(once, [] { ggml_log_set(ggml_to_sink, nullptr); });
    } catch (...) {
    }
}
#else
void route_ggml_log() noexcept {}
#endif

} // namespace detail
} // namespace kimodo
