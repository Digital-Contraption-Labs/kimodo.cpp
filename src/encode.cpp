// Small developer utility for capturing a portable F32 LLM2Vec embedding.
// It uses the same serial GGML text session as the public prompt API.
#include "llm_text_encoder.hpp"
#include "environment_options.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc, char **argv) try {
    if (argc != 4 && argc != 5) {
        std::cerr << "usage: " << argv[0] << " TEXT_BUNDLE PROMPT.txt OUTPUT.f32 [REPETITIONS]\n";
        return 2;
    }
    const unsigned repetitions = argc == 5 ? static_cast<unsigned>(std::stoul(argv[4])) : 1U;
    if (repetitions == 0) throw std::runtime_error("REPETITIONS must be positive");
    std::ifstream prompt_file(argv[2]);
    const std::string prompt{std::istreambuf_iterator<char>(prompt_file), {}};
    if (!prompt_file && prompt.empty()) throw std::runtime_error("cannot read prompt");
    const bool profile = std::getenv("KIMODO_PROFILE") != nullptr || repetitions > 1;
    kimodo::tools::log_to_stderr();
    const auto load_started = std::chrono::steady_clock::now();
    auto encoder = kimodo::detail::llm_text_encoder::load(argv[1], kimodo::tools::environment_options());
    if (!encoder) throw std::runtime_error(encoder.error());
    if (profile) {
        const auto elapsed = std::chrono::steady_clock::now() - load_started;
        std::cerr << "profile text.load_ms="
                  << std::chrono::duration<double, std::milli>(elapsed).count() << '\n';
    }
    std::array<float, 4096> result{};
    for (unsigned iteration = 0; iteration < repetitions; ++iteration) {
        const auto encode_started = std::chrono::steady_clock::now();
        auto embedding = (*encoder)->encode(prompt);
        if (!embedding) throw std::runtime_error(embedding.error());
        result = *embedding;
        if (profile) {
            const auto elapsed = std::chrono::steady_clock::now() - encode_started;
            std::cerr << "profile text.encode_ms iteration=" << iteration + 1 << " value="
                      << std::chrono::duration<double, std::milli>(elapsed).count() << '\n';
        }
    }
    std::ofstream output(argv[3], std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open output");
    output.write(reinterpret_cast<const char *>(result.data()),
                 static_cast<std::streamsize>(result.size() * sizeof(float)));
    if (!output) throw std::runtime_error("cannot write output");
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
