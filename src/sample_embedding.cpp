// Replay motion generation with an explicit embedding and initial-noise tensor.
// This is the controlled boundary used by quantization comparisons.
#include "denoiser.hpp"
#include "environment_options.hpp"
#include "ggml_weights.hpp"
#include "motion_decode.hpp"
#include "skeleton.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<float> read_f32(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 ||
        input.tellg() % static_cast<std::streamoff>(sizeof(float)))
        throw std::runtime_error("invalid F32 file " + path.string());
    std::vector<float> values(static_cast<std::size_t>(input.tellg()) /
                              sizeof(float));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(values.data()),
               static_cast<std::streamsize>(values.size() * sizeof(float)));
    if (!input)
        throw std::runtime_error("short F32 file " + path.string());
    return values;
}

void write_f32(const std::filesystem::path &path,
               const std::vector<float> &values) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot write " + path.string());
    output.write(reinterpret_cast<const char *>(values.data()),
                 static_cast<std::streamsize>(values.size() * sizeof(float)));
    if (!output)
        throw std::runtime_error("short write " + path.string());
}

void write_skeleton(const std::filesystem::path &path,
                    const kimodo::detail::skeleton_spec &skeleton,
                    std::size_t frames) {
    std::ofstream output(path, std::ios::trunc);
    if (!output)
        throw std::runtime_error("cannot write " + path.string());
    output << "{\n  \"key\": \"" << skeleton.key
           << "\",\n  \"frames\": " << frames
           << ",\n  \"fps\": 30,\n  \"names\": [";
    for (std::size_t i = 0; i < skeleton.names.size(); ++i)
        output << (i ? ", " : "") << '"' << skeleton.names[i] << '"';
    output << "],\n  \"parents\": [";
    for (std::size_t i = 0; i < skeleton.parents.size(); ++i)
        output << (i ? ", " : "") << skeleton.parents[i];
    output << "],\n  \"offsets\": [";
    for (std::size_t i = 0; i < skeleton.offsets.size(); ++i) {
        const auto &v = skeleton.offsets[i];
        output << (i ? ", " : "") << '[' << v[0] << ", " << v[1] << ", " << v[2]
               << ']';
    }
    output << "]\n}\n";
    if (!output)
        throw std::runtime_error("short write " + path.string());
}

} // namespace

int main(int argc, char **argv) try {
    if (argc != 7) {
        std::cerr
            << "usage: " << argv[0]
            << " MODEL.gguf EMBEDDING.f32 NOISE.f32 FRAMES STEPS OUTPUT_DIR\n";
        return 2;
    }
    const auto frames = static_cast<std::size_t>(std::stoul(argv[4]));
    const auto steps = static_cast<unsigned>(std::stoul(argv[5]));
    if (!frames || !steps)
        throw std::runtime_error("frames and steps must be positive");
    const auto embedding = read_f32(argv[2]);
    const auto noise = read_f32(argv[3]);
    auto weights = kimodo::detail::ggml_motion_weights::load(argv[1], kimodo::tools::environment_options());
    if (!weights)
        throw std::runtime_error(weights.error());
    const auto *skeleton =
        kimodo::detail::find_skeleton((*weights)->skeleton_key());
    if (!skeleton || embedding.size() != 4096 ||
        noise.size() != frames * skeleton->motion_dim())
        throw std::runtime_error(
            "embedding, noise, or model dimensions do not match");
    auto sampled = kimodo::detail::sample_motion_from_noise(
        **weights, noise, embedding, frames, steps, 2.F, 2.F);
    if (!sampled)
        throw std::runtime_error(sampled.error());
    auto gm = (**weights).f32_values("stats.global_root.mean");
    auto gs = (**weights).f32_values("stats.global_root.std");
    auto bm = (**weights).f32_values("stats.body.mean");
    auto bs = (**weights).f32_values("stats.body.std");
    if (!gm || !gs || !bm || !bs)
        throw std::runtime_error("missing motion normalisation tensors");
    auto decoded = kimodo::detail::decode_motion(*sampled, frames, *skeleton,
                                                 *gm, *gs, *bm, *bs);
    if (!decoded)
        throw std::runtime_error(decoded.error());
    const std::filesystem::path output = argv[6];
    std::filesystem::create_directories(output);
    write_f32(output / "sampling_final_state.f32", *sampled);
    write_f32(output / "root_positions.f32", decoded->root_positions);
    write_f32(output / "local_rotations_xyzw.f32", decoded->local_xyzw);
    write_skeleton(output / "skeleton.json", *skeleton, frames);
    std::cout << "generated " << frames << " frames for " << skeleton->key
              << '\n';
    return 0;
} catch (const std::exception &error) {
    std::cerr << "controlled sampling failed: " << error.what() << '\n';
    return 1;
}
