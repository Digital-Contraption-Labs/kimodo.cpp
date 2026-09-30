#pragma once

#include <kimodo/kimodo.hpp>

#include <array>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace kimodo::detail {

class llm_text_encoder {
public:
    // A text model is either a monolithic weight GGUF beside tokenizer.gguf,
    // or a legacy component directory; `source` is UTF-8.  The options'
    // text_layer_chunk decides residency: 32 keeps the complete encoder on
    // the device (within text_resident_limit_bytes) while it still executes
    // bounded GGML graphs, fewer streams it through in groups, and 0 chooses
    // residency when the device has room.
    static std::expected<std::unique_ptr<llm_text_encoder>, std::string> load(
        std::string_view source, const runtime_options &options = {});
    std::expected<std::array<float, 4096>, std::string> encode(std::string_view utf8_prompt) const;
    ~llm_text_encoder();
    llm_text_encoder(const llm_text_encoder &) = delete;
    llm_text_encoder &operator=(const llm_text_encoder &) = delete;
private:
    llm_text_encoder() = default;
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace kimodo::detail
