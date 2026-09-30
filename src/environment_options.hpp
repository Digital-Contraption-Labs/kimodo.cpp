#pragma once

// For the command-line tools and tests only.  The library reads no
// environment variables (docs/SHARED_LIBRARY_PLAN.md, section 5), so the
// tools translate the ones they have always honoured into runtime_options,
// and print the library's log to stderr:
//
//   KIMODO_BACKEND=cpu|vulkan            KIMODO_THREADS=N
//   KIMODO_TEXT_LAYER_CHUNK=1..32        KIMODO_TEXT_RESIDENT_LIMIT_MIB=N
//   KIMODO_TEXT_PACKED_LORA=0            KIMODO_MOTION_PACKED_ATTENTION=0
//   KIMODO_MOTION_GRAPH_CACHE=0          KIMODO_MOTION_LAYER_CHUNK=N
//   KIMODO_PROFILE=1                     timings on stderr
#include <kimodo/kimodo.hpp>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kimodo::tools {

inline bool environment_parse(const char *value, std::uint64_t &out) {
    const auto [end, error] = std::from_chars(value, value + std::strlen(value), out);
    return error == std::errc{} && *end == '\0';
}

inline runtime_options environment_options() {
    runtime_options options;
    std::uint64_t number = 0;
    if (const char *value = std::getenv("KIMODO_BACKEND")) {
        const std::string_view backend(value);
        options.backend = backend == "cpu" ? device::cpu : backend == "vulkan" ? device::vulkan : device::automatic;
    }
    if (const char *value = std::getenv("KIMODO_THREADS"); value && environment_parse(value, number) && number > 0 && number < 65536)
        options.threads = static_cast<unsigned>(number);
    if (const char *value = std::getenv("KIMODO_TEXT_LAYER_CHUNK")) {
        if (!environment_parse(value, number) || number < 1 || number > 32)
            throw std::runtime_error("KIMODO_TEXT_LAYER_CHUNK must be in 1..32");
        options.text_layer_chunk = static_cast<unsigned>(number);
    }
    if (const char *value = std::getenv("KIMODO_TEXT_RESIDENT_LIMIT_MIB")) {
        if (!environment_parse(value, number) || number == 0 || number > (UINT64_MAX >> 20))
            throw std::runtime_error("KIMODO_TEXT_RESIDENT_LIMIT_MIB must be a positive integer");
        options.text_resident_limit_bytes = number << 20;
    }
    const auto off = [](const char *name) {
        const char *value = std::getenv(name);
        return value && std::string_view(value) == "0";
    };
    options.text_packed_lora = !off("KIMODO_TEXT_PACKED_LORA");
    options.motion_packed_attention = !off("KIMODO_MOTION_PACKED_ATTENTION");
    options.motion_graph_cache = !off("KIMODO_MOTION_GRAPH_CACHE");
    if (const char *value = std::getenv("KIMODO_MOTION_LAYER_CHUNK"); value && environment_parse(value, number) && number <= 16)
        options.motion_layer_chunk = static_cast<unsigned>(number);
    return options;
}

// The library's messages, ggml's included, on stderr: from info up, and
// the timings too with KIMODO_PROFILE.
inline void log_to_stderr() {
    set_log_sink([](log_level, std::string_view line) {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
    }, std::getenv("KIMODO_PROFILE") ? log_level::debug : log_level::info);
}

} // namespace kimodo::tools
