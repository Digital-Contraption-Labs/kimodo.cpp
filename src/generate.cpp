// Command-line bridge for the localhost demo.  It deliberately uses only the
// public C++ model API, so the demo exercises the same text route as embedders.
#include <kimodo/kimodo.hpp>
#include "environment_options.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
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
// TAB-separated -- transition, steps, seed, text_cfg, constraint_cfg,
// first_heading, post_process (0/1), root_margin, constraints, then
// (frames, prompt) pairs with the prompt
// base64-encoded so a paragraph with tabs or newlines stays one field.
// `constraints` is "-" for none, else the base64 of the binary block that
// decode_constraints reads.  The reply is one line, "OK\tframes\tjoints",
// followed by the motion as raw little-endian float32 (frames*3 root
// positions, then frames*joints*4 local XYZW rotations), or "ERR\tmessage".
// Nothing touches the disk.
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
        if (index == std::string_view::npos) throw std::runtime_error("invalid base64 field");
        accumulator = (accumulator << 6) | static_cast<unsigned>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
        }
    }
    return out;
}

// The constraint block, all little-endian: u32 joints, u32 count, then per
// constraint u32 type, u32 end_effectors, u32 frame_count F, u32 flags
// (1 pose, 2 smooth_root_2d, 4 root_heading), u32 frames[F], and as flagged
// f32 root_positions[F*3] + local_rotations_xyzw[F*joints*4],
// smooth_root_2d[F*2], root_heading[F*2].
class constraint_reader {
public:
    explicit constraint_reader(std::string_view bytes) : bytes_(bytes) {}
    std::uint32_t u32() {
        need(4);
        std::uint32_t value = 0;
        for (int i = 3; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(bytes_[at_ + static_cast<size_t>(i)]);
        at_ += 4;
        return value;
    }
    std::vector<float> f32(size_t count) {
        need(count * 4);
        std::vector<float> values(count);
        for (float &value : values) value = std::bit_cast<float>(u32());
        return values;
    }
    [[nodiscard]] bool done() const noexcept { return at_ == bytes_.size(); }
private:
    void need(size_t count) const {
        if (count > bytes_.size() - at_) throw std::runtime_error("truncated constraint block");
    }
    std::string_view bytes_;
    size_t at_ = 0;
};

// Fills `result.constraints` from the protocol's constraint field.
void decode_constraints(std::string_view field, unsigned joints, kimodo::generation_options &result) {
    if (field == "-") return;
    const std::string bytes = base64_decode(field);
    constraint_reader in(bytes);
    if (in.u32() != joints) throw std::runtime_error("constraints were built for another skeleton");
    const std::uint32_t count = in.u32();
    for (std::uint32_t index = 0; index < count; ++index) {
        kimodo::motion_constraint c;
        c.type = static_cast<kimodo::constraint_type>(in.u32());
        c.end_effectors = in.u32();
        const std::uint32_t frames = in.u32(), flags = in.u32();
        if (frames == 0 || frames > 100000) throw std::runtime_error("invalid constraint frame count");
        for (std::uint32_t frame = 0; frame < frames; ++frame) c.frames.push_back(in.u32());
        if (flags & 1U) {
            c.root_positions = in.f32(size_t{frames} * 3);
            c.local_rotations_xyzw = in.f32(size_t{frames} * joints * 4);
        }
        if (flags & 2U) c.smooth_root_2d = in.f32(size_t{frames} * 2);
        if (flags & 4U) c.root_heading = in.f32(size_t{frames} * 2);
        result.constraints.push_back(std::move(c));
    }
    if (!in.done()) throw std::runtime_error("trailing bytes after the constraint block");
}

float parse_finite(const std::string &text, float low, float high, const char *what) {
    std::size_t consumed = 0;
    const float value = std::stof(text, &consumed);
    if (consumed != text.size() || !std::isfinite(value) || value < low || value > high)
        throw std::runtime_error(std::string(what) + " is out of range");
    return value;
}

void write_motion_stream(std::ostream &out, const kimodo::motion_data &motion) {
    out.write(reinterpret_cast<const char *>(motion.root_positions.data()),
              static_cast<std::streamsize>(motion.root_positions.size() * sizeof(float)));
    out.write(reinterpret_cast<const char *>(motion.local_rotations_xyzw.data()),
              static_cast<std::streamsize>(motion.local_rotations_xyzw.size() * sizeof(float)));
    out.flush();
    if (!out) throw std::runtime_error("cannot write the motion to stdout");
}

// Upstream samples with separated CFG weights [2.0, 2.0].  The positional CLI
// modes have no constraints, so their constraint weight only shapes
// multi-prompt hand-offs and stays at the upstream default.
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

// The environment's options, with room for the longest clip the library
// takes: the demo server enforces its own 12 s policy per segment.
kimodo::runtime_options tool_options() {
    auto options = kimodo::tools::environment_options();
    options.max_segment_frames = kimodo::limit_ceilings.max_segment_frames;
    return options;
}
}

int main(int argc, char **argv) try {
    kimodo::tools::log_to_stderr();
    if (argc == 4 && std::string_view(argv[1]) == "--server") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY); // the motion goes down stdout as bytes
#endif
        auto model = kimodo::model::load(argv[2], argv[3], tool_options());
        if (!model) throw std::runtime_error(model.error());
        std::string line;
        while (std::getline(std::cin, line)) {
            try {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto fields = split_fields(line);
                constexpr size_t header = 9;
                if (fields.size() < header + 2 || (fields.size() - header) % 2 != 0)
                    throw std::runtime_error("invalid server request");
                const auto transition = static_cast<unsigned>(std::stoul(fields[0]));
                const auto steps = static_cast<unsigned>(std::stoul(fields[1]));
                const auto seed = static_cast<std::uint64_t>(std::stoull(fields[2]));
                const float text_cfg = parse_text_cfg(fields[3]);
                const float request_constraint_cfg = parse_finite(fields[4], 0.F, 20.F, "constraint CFG weight");
                kimodo::generation_options options;
                options.first_heading = parse_finite(fields[5], -1000.F, 1000.F, "first heading");
                if (fields[6] != "0" && fields[6] != "1") throw std::runtime_error("post_process must be 0 or 1");
                options.post_process = fields[6] == "1";
                options.root_margin = parse_finite(fields[7], 0.F, 10.F, "root margin");
                decode_constraints(fields[8], (*model)->joints(), options);
                std::vector<kimodo::prompt_segment> segments;
                for (size_t index = header; index < fields.size(); index += 2) {
                    std::string prompt = base64_decode(fields[index + 1]);
                    if (prompt.empty()) throw std::runtime_error("empty sequence prompt");
                    segments.push_back({std::move(prompt), static_cast<unsigned>(std::stoul(fields[index]))});
                }
                auto motion = (*model)->generate_text_sequence(segments, transition, steps, seed, text_cfg, request_constraint_cfg, options);
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
        auto model = kimodo::model::load(argv[1], argv[2], tool_options());
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
    auto model = kimodo::model::load(argv[1], argv[2], tool_options());
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
