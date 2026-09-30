#include "constraints.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

// NVIDIA's constraints JSON reaches the library from its host, and from the
// files a user picks, so every byte sequence must fail cleanly or parse.
// The accepted ones are then built into a condition, as a generation would.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size > 1024 * 1024) return 0;
    const std::string_view text(reinterpret_cast<const char *>(data), size);
    for (const auto *skeleton : {&kimodo::detail::soma30_spec, &kimodo::detail::g1skel34_spec}) {
        auto parsed = kimodo::detail::constraints_from_json(text, *skeleton, 150, 256);
        if (parsed) (void) kimodo::detail::build_constraint_condition(*skeleton, *parsed, 150);
    }
    return 0;
}
