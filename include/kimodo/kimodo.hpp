#pragma once

#include <kimodo/kimodo_capi.h>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kimodo {

inline constexpr unsigned embedding_width = 4096;

struct motion_data {
    unsigned frames = 0;
    unsigned joints = 0;
    std::vector<float> local_rotations_xyzw;
    std::vector<float> root_positions;
};

struct prompt_segment {
    std::string prompt;
    unsigned frames = 0;
};

// NVIDIA Kimodo's kinematic constraint sets; see kimodo_capi.h for the
// canonical space and what each type constrains.
enum class constraint_type : std::uint32_t {
    root2d = KIMODO_CONSTRAINT_ROOT2D,
    fullbody = KIMODO_CONSTRAINT_FULLBODY,
    end_effector = KIMODO_CONSTRAINT_END_EFFECTOR,
};

struct motion_constraint {
    constraint_type type = constraint_type::fullbody;
    std::uint32_t end_effectors = 0;          // KIMODO_END_EFFECTOR_* bits
    std::vector<unsigned> frames;             // [F], across the whole clip
    std::vector<float> root_positions;        // [F,3] pose types
    std::vector<float> local_rotations_xyzw;  // [F,J,4] pose types
    std::vector<float> smooth_root_2d;        // [F,2] root2d; optional otherwise
    std::vector<float> root_heading;          // [F,2] (cos, sin); root2d only, optional
};

struct generation_options {
    std::vector<motion_constraint> constraints;
    float first_heading = 0.F;                // initial facing, radians; 0 faces +Z
    // Upstream's post-processing (NVIDIA MotionCorrection): foot-skate
    // cleanup and IK/root correction onto the constraints, per segment.  Off
    // by default as in upstream's Python API; its demo enables it except on G1.
    bool post_process = false;
    float root_margin = .04F;                 // metres a root target may be missed by

    [[nodiscard]] bool plain() const noexcept { return constraints.empty() && first_heading == 0.F && !post_process; }
};

class KIMODO_API model {
public:
    static std::expected<std::unique_ptr<model>, std::string> load(
        std::string_view motion_gguf, std::string_view text_bundle = {});
    // Joints of the model's skeleton: the J of constraint and motion rotations.
    [[nodiscard]] unsigned joints() const noexcept;
    std::expected<motion_data, std::string> generate_embedding(
        const std::array<float, embedding_width> &embedding,
        unsigned frames, unsigned steps, std::uint64_t seed,
        float text_cfg, float constraint_cfg,
        const generation_options &options = {}) const;
    std::expected<motion_data, std::string> generate_text(
        std::string_view utf8_prompt, unsigned frames, unsigned steps, std::uint64_t seed,
        float text_cfg, float constraint_cfg,
        const generation_options &options = {}) const;
    // Constraint frames index the joined clip: segment k starts at the sum of
    // the earlier segments' frames.
    std::expected<motion_data, std::string> generate_text_sequence(
        std::span<const prompt_segment> segments, unsigned transition_frames,
        unsigned steps, std::uint64_t seed, float text_cfg, float constraint_cfg,
        const generation_options &options = {}) const;
    // Whether this build includes post-processing (x86 with Eigen).
    [[nodiscard]] static bool post_processing_available() noexcept;
    ~model();
    model(const model &) = delete;
    model &operator=(const model &) = delete;
private:
    struct impl;
    explicit model(std::unique_ptr<impl> impl);
    std::unique_ptr<impl> impl_;
};

} // namespace kimodo
