#pragma once

#include <kimodo/kimodo_capi.h>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
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
    std::vector<float> local_rotations_xyzw;  // [F,P,4] pose types
    std::vector<float> smooth_root_2d;        // [F,2] root2d; optional otherwise
    std::vector<float> root_heading;          // [F,2] (cos, sin); root2d only, optional
    // A pose may come as axis-angle instead of XYZW, [F,P,3], and with P the
    // pose's joint count: 0 is the model's; the SOMA models also take NVIDIA's
    // 77-joint SOMA order, mapped onto their 30 joints.
    std::vector<float> local_rotations_axis_angle;
    unsigned pose_joints = 0;
};

// What a model accepts; generations outside these fail with a reason.
struct limits {
    unsigned min_segment_frames = 2;
    unsigned max_segment_frames = 360;        // 12 s: the demo server's policy
    unsigned max_segments = 16;
    unsigned max_transition_frames = 60;
    unsigned max_diffusion_steps = 1000;
    unsigned max_constraints = 256;
    unsigned max_prompt_bytes = 4096;
    unsigned frame_rate = 30;
};
inline constexpr limits default_limits{};
// The most runtime_options may raise the limits to.  Positions are computed,
// not tabulated, so frames are bounded by device memory: one attention
// buffer grows with the square of a segment's frames (0.33 GB at 1800, 1.28 GB
// at 3600).  On a 16 GB GPU with the text encoder resident 1800 frames (60 s)
// generate and 3600 do not; a smaller GPU fails sooner, with a message.  Past
// 360 frames the quality is unjudged.
inline constexpr limits limit_ceilings{2, 1800, 64, 60, 1000, 256, 4096, 30};

enum class device : std::uint32_t {
    automatic = KIMODO_DEVICE_AUTO,           // the GPU when there is a usable one
    cpu = KIMODO_DEVICE_CPU,
    vulkan = KIMODO_DEVICE_VULKAN,
};

// How a model runs.  These are the library's only levers: it reads no
// environment variables (the tools translate theirs into these).
struct runtime_options {
    device backend = device::automatic;
    unsigned gpu_index = 0;                   // among the Vulkan backend's GPUs
    unsigned threads = 0;                     // CPU threads; 0 uses every hardware thread
    // Text encoder layers on the device at once: 0 decides (all 32 when the
    // device has room beside the motion model, else 8), 1..32 sets it.
    unsigned text_layer_chunk = 0;
    std::uint64_t text_resident_limit_bytes = 0; // never keep more resident; 0 no limit
    bool preload = false;                     // load every weight in load(), not at first use
    unsigned max_segment_frames = 0;          // 0: default_limits; clamped to limit_ceilings
    unsigned max_segments = 0;
    // Execution paths, for measurement and triage; the defaults are the fast ones.
    bool text_packed_lora = true;
    bool motion_packed_attention = true;
    bool motion_graph_cache = true;
    unsigned motion_layer_chunk = 0;          // 0: 8 packed, 4 unpacked
};

enum class progress_stage : std::uint32_t {
    loading = KIMODO_STAGE_LOADING,
    encoding_text = KIMODO_STAGE_ENCODING_TEXT,
    sampling = KIMODO_STAGE_SAMPLING,
};
// Called on the generating thread with how far a stage is; returning false
// cancels, and the generation then fails with `cancelled_error`.
using progress_callback = std::function<bool(progress_stage stage, unsigned done, unsigned total)>;
inline constexpr std::string_view cancelled_error = "generation cancelled";

struct generation_options {
    std::vector<motion_constraint> constraints;
    float first_heading = 0.F;                // initial facing, radians; 0 faces +Z
    // Upstream's post-processing (NVIDIA MotionCorrection): foot-skate
    // cleanup and IK/root correction onto the constraints, per segment.  Off
    // by default as in upstream's Python API; its demo enables it except on G1.
    bool post_process = false;
    float root_margin = .04F;                 // metres a root target may be missed by
    progress_callback progress;               // optional

    [[nodiscard]] bool plain() const noexcept { return constraints.empty() && first_heading == 0.F && !post_process; }
};

enum class log_level : int {
    debug = KIMODO_LOG_DEBUG,
    info = KIMODO_LOG_INFO,
    warning = KIMODO_LOG_WARNING,
    error = KIMODO_LOG_ERROR,
};
using log_sink = std::function<void(log_level level, std::string_view line)>;
// Sends the library's messages at `minimum` and above, ggml's included, to
// `sink`, for every model.  An empty sink silences them.  Until this is
// called ggml keeps its own default of printing to stderr; the C API calls it
// first, so the shared library is silent until its host asks.
void set_log_sink(log_sink sink, log_level minimum = log_level::info);

// What this build is and can do.
struct build_information {
    std::string_view version;                 // the project's version
    std::string_view commit;                  // the source commit, when known at configure time
    bool vulkan = false;                      // the Vulkan backend is built in
    bool post_processing = false;             // see model::post_processing_available
};
[[nodiscard]] build_information build() noexcept;

// The GPUs the Vulkan backend can use, in gpu_index order.
struct gpu_description {
    std::string name;
    std::uint64_t memory_total = 0;           // bytes
    std::uint64_t memory_free = 0;            // bytes free now, as the driver reports it
};
std::vector<gpu_description> list_gpus();

// A model's skeleton, from NVIDIA Kimodo's definitions.
struct skeleton_view {
    std::string_view key;                             // "soma30", "g1skel34", "smplx22"
    std::span<const std::string_view> names;
    std::span<const int> parents;                     // -1 for the root
    std::span<const std::array<float, 3>> offsets;    // rest offsets from the parent, metres
};

// The C++ API is for code linked statically with the engine (kimodo-core):
// the tools and tests.  The shared library exports the C API alone.
class model {
public:
    static std::expected<std::unique_ptr<model>, std::string> load(
        std::string_view motion_gguf, std::string_view text_bundle = {},
        const runtime_options &options = {});
    // Joints of the model's skeleton: the J of constraint and motion rotations.
    [[nodiscard]] unsigned joints() const noexcept;
    [[nodiscard]] skeleton_view skeleton() const noexcept;
    [[nodiscard]] const limits &active_limits() const noexcept;
    [[nodiscard]] bool has_text_encoder() const noexcept;
    // Brings the motion weights onto the device now rather than at the first
    // generation (the text encoder's resident layers are loaded by load()).
    std::expected<void, std::string> preload() const;
    std::expected<std::array<float, embedding_width>, std::string> encode_text(std::string_view utf8_prompt) const;
    // NVIDIA Kimodo's constraints JSON, checked for a clip of `total_frames`.
    std::expected<std::vector<motion_constraint>, std::string> constraints_from_json(
        std::string_view json, unsigned total_frames) const;
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
