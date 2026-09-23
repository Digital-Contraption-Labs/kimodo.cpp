// Command-line bridge for the localhost demo.  It deliberately uses only the
// public C++ model API, so the demo exercises the same text route as embedders.
#include <kimodo/kimodo.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
void write_f32(const std::filesystem::path &path, const std::vector<float> &values) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open " + path.string());
    out.write(reinterpret_cast<const char *>(values.data()),
              static_cast<std::streamsize>(values.size() * sizeof(float)));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

void write_motion(const std::filesystem::path &output, const kimodo::motion_data &motion) {
    std::filesystem::create_directories(output);
    write_f32(output / "root_positions.f32", motion.root_positions);
    write_f32(output / "local_rotations_xyzw.f32", motion.local_rotations_xyzw);
}

std::vector<std::string> split_fields(const std::string &line) {
    std::vector<std::string> fields;
    std::string field;
    std::istringstream input(line);
    while (std::getline(input, field, '\t')) fields.push_back(field);
    return fields;
}

std::string protocol_error(std::string message) {
    for (char &c : message) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return message;
}

// The demo server's line protocol (demo/main.go): one request per line,
// TAB-separated -- transition, steps, seed, text_cfg, then (frames, prompt)
// pairs with the prompt base64-encoded so a paragraph with tabs or newlines
// stays one field.  The reply is one line, "OK\tframes\tjoints", followed by
// the motion as raw little-endian float32 (frames*3 root positions, then
// frames*joints*4 local XYZW rotations), or "ERR\tmessage".  Nothing touches
// the disk.
std::string base64_decode(std::string_view text) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(text.size() * 3 / 4);
    unsigned accumulator = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') break;
        const auto index = alphabet.find(c);
        if (index == std::string_view::npos) throw std::runtime_error("invalid base64 prompt");
        accumulator = (accumulator << 6) | static_cast<unsigned>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
        }
    }
    return out;
}

void write_motion_stream(std::ostream &out, const kimodo::motion_data &motion) {
    out.write(reinterpret_cast<const char *>(motion.root_positions.data()),
              static_cast<std::streamsize>(motion.root_positions.size() * sizeof(float)));
    out.write(reinterpret_cast<const char *>(motion.local_rotations_xyzw.data()),
              static_cast<std::streamsize>(motion.local_rotations_xyzw.size() * sizeof(float)));
    out.flush();
    if (!out) throw std::runtime_error("cannot write the motion to stdout");
}

// Upstream samples with separated CFG weights [2.0, 2.0]. Text guidance is the
// one that shapes a single-prompt clip; the constraint weight only matters for
// conditioned multi-prompt hand-offs, so it stays at the upstream default.
constexpr float default_text_cfg = 2.F;
constexpr float constraint_cfg = 2.F;

float parse_text_cfg(const std::string &text) {
    std::size_t consumed = 0;
    const float value = std::stof(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value) || value < 0.F || value > 20.F)
        throw std::runtime_error("text CFG weight must be a number in 0..20");
    return value;
}

// Positional CLI modes keep their argument layout; KIMODO_TEXT_CFG overrides
// the guidance weight for scripts and experiments.
float text_cfg_from_env() {
    const char *value = std::getenv("KIMODO_TEXT_CFG");
    return value ? parse_text_cfg(value) : default_text_cfg;
}
}

int main(int argc, char **argv) try {
    if (argc == 4 && std::string_view(argv[1]) == "--server") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY); // the motion goes down stdout as bytes
#endif
        auto model = kimodo::model::load(argv[2], argv[3]);
        if (!model) throw std::runtime_error(model.error());
        std::string line;
        while (std::getline(std::cin, line)) {
            try {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto fields = split_fields(line);
                if (fields.size() < 6 || (fields.size() - 4) % 2 != 0)
                    throw std::runtime_error("invalid server request");
                const auto transition = static_cast<unsigned>(std::stoul(fields[0]));
                const auto steps = static_cast<unsigned>(std::stoul(fields[1]));
                const auto seed = static_cast<std::uint64_t>(std::stoull(fields[2]));
                const float text_cfg = parse_text_cfg(fields[3]);
                std::vector<kimodo::prompt_segment> segments;
                for (size_t index = 4; index < fields.size(); index += 2) {
                    std::string prompt = base64_decode(fields[index + 1]);
                    if (prompt.empty()) throw std::runtime_error("empty sequence prompt");
                    segments.push_back({std::move(prompt), static_cast<unsigned>(std::stoul(fields[index]))});
                }
                auto motion = (*model)->generate_text_sequence(segments, transition, steps, seed, text_cfg, constraint_cfg);
                if (!motion) throw std::runtime_error(motion.error());
                std::cout << "OK\t" << motion->frames << '\t' << motion->joints << '\n' << std::flush;
                write_motion_stream(std::cout, *motion);
            } catch (const std::exception &error) {
                std::cout << "ERR\t" << protocol_error(error.what()) << '\n' << std::flush;
            }
        }
        return 0;
    }
    if (argc >= 10 && std::string_view(argv[3]) == "--sequence") {
        if ((argc - 8) % 2 != 0) throw std::runtime_error("sequence requires FRAME PROMPT.txt pairs");
        const auto transition = static_cast<unsigned>(std::stoul(argv[4]));
        const auto steps = static_cast<unsigned>(std::stoul(argv[5]));
        const auto seed = static_cast<std::uint64_t>(std::stoull(argv[6]));
        std::vector<kimodo::prompt_segment> segments;
        for (int index=8; index<argc; index+=2) {
            std::ifstream prompt_file(argv[index+1]);
            const std::string prompt{std::istreambuf_iterator<char>(prompt_file), {}};
            if (!prompt_file && prompt.empty()) throw std::runtime_error("cannot read sequence prompt");
            segments.push_back({prompt, static_cast<unsigned>(std::stoul(argv[index]))});
        }
        auto model = kimodo::model::load(argv[1], argv[2]);
        if (!model) throw std::runtime_error(model.error());
        auto motion = (*model)->generate_text_sequence(segments, transition, steps, seed, text_cfg_from_env(), constraint_cfg);
        if (!motion) throw std::runtime_error(motion.error());
        write_motion(argv[7], *motion);
        std::cout << "generated " << motion->frames << " frames with " << motion->joints << " joints\n";
        return 0;
    }
    if (argc != 8) {
        std::cerr << "usage: " << argv[0] << " MOTION.gguf TEXT_BUNDLE PROMPT.txt FRAMES STEPS SEED OUTPUT_DIR\n"
                  << "   or: " << argv[0] << " MOTION.gguf TEXT_BUNDLE --sequence TRANSITION STEPS SEED OUTPUT_DIR FRAME PROMPT.txt [FRAME PROMPT.txt ...]\n"
                  << "KIMODO_TEXT_CFG overrides the text guidance weight (default 2.0)\n";
        return 2;
    }
    std::ifstream prompt_file(argv[3]);
    const std::string prompt{std::istreambuf_iterator<char>(prompt_file), {}};
    if (!prompt_file && prompt.empty()) throw std::runtime_error("cannot read prompt");
    const auto frames = static_cast<unsigned>(std::stoul(argv[4]));
    const auto steps = static_cast<unsigned>(std::stoul(argv[5]));
    const auto seed = static_cast<std::uint64_t>(std::stoull(argv[6]));
    auto model = kimodo::model::load(argv[1], argv[2]);
    if (!model) throw std::runtime_error(model.error());
    auto motion = (*model)->generate_text(prompt, frames, steps, seed, text_cfg_from_env(), constraint_cfg);
    if (!motion) throw std::runtime_error(motion.error());
    write_motion(argv[7], *motion);
    std::cout << "generated " << motion->frames << " frames with " << motion->joints << " joints\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
