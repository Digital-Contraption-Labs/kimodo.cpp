#include <kimodo/kimodo_capi.h>
#include <kimodo/kimodo.hpp>

#include "log.hpp"
#include "utf8_path.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

// The header's function pointer types match its declarations.
#define KIMODO_CHECK_FN(name) static_assert(std::is_same_v<decltype(&name), name##_fn>, #name "_fn does not match " #name)
KIMODO_CHECK_FN(kimodo_abi_version);
KIMODO_CHECK_FN(kimodo_model_load);
KIMODO_CHECK_FN(kimodo_model_free);
KIMODO_CHECK_FN(kimodo_model_last_error);
KIMODO_CHECK_FN(kimodo_model_joints);
KIMODO_CHECK_FN(kimodo_generate);
KIMODO_CHECK_FN(kimodo_generate_constrained);
KIMODO_CHECK_FN(kimodo_generate_embedding);
KIMODO_CHECK_FN(kimodo_motion_free);
KIMODO_CHECK_FN(kimodo_motion_frames);
KIMODO_CHECK_FN(kimodo_motion_joints);
KIMODO_CHECK_FN(kimodo_motion_local_rotations_xyzw);
KIMODO_CHECK_FN(kimodo_motion_root_positions);
KIMODO_CHECK_FN(kimodo_get_capabilities);
KIMODO_CHECK_FN(kimodo_set_log_callback);
KIMODO_CHECK_FN(kimodo_gpu_count);
KIMODO_CHECK_FN(kimodo_gpu_info_get);
KIMODO_CHECK_FN(kimodo_open);
KIMODO_CHECK_FN(kimodo_list_motion_models);
KIMODO_CHECK_FN(kimodo_model_skeleton);
KIMODO_CHECK_FN(kimodo_model_joint_name);
KIMODO_CHECK_FN(kimodo_model_joint_parent);
KIMODO_CHECK_FN(kimodo_model_joint_offset);
KIMODO_CHECK_FN(kimodo_model_limits);
KIMODO_CHECK_FN(kimodo_get_limit_ceilings);
KIMODO_CHECK_FN(kimodo_generation_options_init);
KIMODO_CHECK_FN(kimodo_runtime_options_init);
KIMODO_CHECK_FN(kimodo_generate_sequence);
KIMODO_CHECK_FN(kimodo_encode_text);
#undef KIMODO_CHECK_FN

struct kimodo_model { std::unique_ptr<kimodo::model> value; std::string last_error; };
struct kimodo_motion { kimodo::motion_data value; };
namespace {
// The structs as earlier ABIs laid them out.  Fields are only appended, so a
// caller's size says which it has.
struct runtime_options_v2 { uint32_t size; uint32_t threads; kimodo_device device; const char *backend_dir; };
struct generation_options_v2 {
    uint32_t size; uint64_t seed; uint32_t frames; uint32_t diffusion_steps; float text_cfg_weight;
    float constraint_cfg_weight; float first_heading; uint32_t post_process; float root_margin;
};
struct constraint_v2 {
    uint32_t size; kimodo_constraint_type type; uint32_t end_effectors; uint32_t frame_count; const uint32_t *frames;
    const float *root_positions; const float *local_rotations_xyzw; const float *smooth_root_2d; const float *root_heading;
};
// A version-1 caller's options end at constraint_cfg_weight.
constexpr uint32_t options_v1_size = offsetof(kimodo_generation_options, first_heading);
static_assert(sizeof(runtime_options_v2) == offsetof(kimodo_runtime_options, gpu_index));
static_assert(sizeof(generation_options_v2) < sizeof(kimodo_generation_options));
static_assert(sizeof(constraint_v2) == offsetof(kimodo_constraint, local_rotations_axis_angle));

constexpr const char *default_motion_model = "kimodo-soma-seed-v1.1-f32.gguf";
constexpr const char *default_text_model = "Llama-3-Kimodo-Q8_0.gguf";

void set_error(kimodo_model *model, char *buffer, int length, const std::string &message) noexcept {
    try {
        if (model) model->last_error = message;
    } catch (...) {
    }
    if (!buffer || length <= 0) return;
    const size_t n = std::min(message.size(), static_cast<size_t>(length - 1));
    std::memcpy(buffer, message.data(), n); buffer[n] = '\0';
}

// The shared library prints nothing until its host asks for messages.
void quiet() noexcept { kimodo::detail::route_ggml_log(); }

bool valid_options(const kimodo_generation_options *o, std::string &error) {
    if (!o || (o->size != sizeof(*o) && o->size != sizeof(generation_options_v2) && o->size != options_v1_size)) {
        error = "invalid kimodo_generation_options";
        return false;
    }
    return true;
}
kimodo::generation_options generation_options(const kimodo_generation_options &o) {
    kimodo::generation_options out;
    if (o.size >= sizeof(generation_options_v2)) {
        out.first_heading = o.first_heading;
        out.post_process = o.post_process != 0;
        out.root_margin = o.root_margin;
    }
    return out;
}
unsigned transition_frames(const kimodo_generation_options &o) {
    return o.size == sizeof(o) && o.transition_frames ? o.transition_frames : 5;
}

std::expected<kimodo::runtime_options, std::string> runtime_options(const kimodo_runtime_options *o) {
    kimodo::runtime_options out;
    if (!o) return out;
    if (o->size != sizeof(*o) && o->size != sizeof(runtime_options_v2)) return std::unexpected("invalid kimodo_runtime_options");
    out.threads = o->threads;
    switch (o->device) {
    case KIMODO_DEVICE_AUTO: out.backend = kimodo::device::automatic; break;
    case KIMODO_DEVICE_CPU: out.backend = kimodo::device::cpu; break;
    case KIMODO_DEVICE_VULKAN: out.backend = kimodo::device::vulkan; break;
    case KIMODO_DEVICE_METAL: out.backend = kimodo::device::metal; break;
    default: return std::unexpected("unknown kimodo_device " + std::to_string(static_cast<int>(o->device)));
    }
    if (o->size == sizeof(*o)) {
        if (o->text_layer_chunk > 32) return std::unexpected("text_layer_chunk must be 0..32");
        out.gpu_index = o->gpu_index;
        out.text_layer_chunk = o->text_layer_chunk;
        out.text_resident_limit_bytes = std::uint64_t{o->text_resident_limit_mib} * 1024 * 1024;
        out.max_segment_frames = o->max_segment_frames;
        out.max_segments = o->max_segments;
    }
    return out;
}

// Borrowed C constraint arrays -> owned C++ constraints; shapes are checked
// against the skeleton and clip when the condition is built.  The array's
// stride is the caller's struct size, which may be an earlier ABI's.
bool copy_constraints(const kimodo_constraints *in, unsigned joints, kimodo::generation_options &out, std::string &error) {
    if (!in) return true;
    if (in->size != sizeof(*in) || (in->count && !in->items)) { error = "invalid kimodo_constraints"; return false; }
    if (!in->count) return true;
    const uint32_t stride = in->items[0].size;
    if (stride != sizeof(kimodo_constraint) && stride != sizeof(constraint_v2)) { error = "invalid kimodo_constraint 0"; return false; }
    const auto *bytes = reinterpret_cast<const unsigned char *>(in->items);
    for (uint32_t index = 0; index < in->count; ++index) {
        kimodo_constraint c{};
        std::memcpy(&c, bytes + size_t{index} * stride, stride);
        if (c.size != stride || !c.frame_count || !c.frames) { error = "invalid kimodo_constraint " + std::to_string(index); return false; }
        const size_t n = c.frame_count;
        const size_t pose_joints = c.pose_joints ? c.pose_joints : joints;
        const auto copy = [](const float *values, size_t count) { return values ? std::vector<float>(values, values + count) : std::vector<float>{}; };
        kimodo::motion_constraint constraint;
        constraint.type = static_cast<kimodo::constraint_type>(c.type);
        constraint.end_effectors = c.end_effectors;
        constraint.frames.assign(c.frames, c.frames + n);
        constraint.root_positions = copy(c.root_positions, n * 3);
        constraint.local_rotations_xyzw = copy(c.local_rotations_xyzw, n * pose_joints * 4);
        constraint.local_rotations_axis_angle = copy(c.local_rotations_axis_angle, n * pose_joints * 3);
        constraint.pose_joints = c.pose_joints;
        constraint.smooth_root_2d = copy(c.smooth_root_2d, n * 2);
        constraint.root_heading = copy(c.root_heading, n * 2);
        out.constraints.push_back(std::move(constraint));
    }
    return true;
}

void fill_limits(const kimodo::limits &in, kimodo_limits &out) {
    out.min_segment_frames = in.min_segment_frames;
    out.max_segment_frames = in.max_segment_frames;
    out.max_segments = in.max_segments;
    out.max_transition_frames = in.max_transition_frames;
    out.max_diffusion_steps = in.max_diffusion_steps;
    out.max_constraints = in.max_constraints;
    out.max_prompt_bytes = in.max_prompt_bytes;
    out.frame_rate = in.frame_rate;
}

kimodo_status failure(kimodo_model *model, char *err, int len, const std::string &message) {
    set_error(model, err, len, message);
    return message == kimodo::cancelled_error ? KIMODO_CANCELLED : KIMODO_ERROR;
}
} // namespace

extern "C" {
int kimodo_abi_version(void) { return KIMODO_CAPI_ABI_VERSION; }
kimodo_model *kimodo_model_load(const char *motion, const char *text, const char *adapter, const kimodo_runtime_options *options, char *err, int err_len) {
    try {
        quiet();
        if (!motion || !*motion) { set_error(nullptr, err, err_len, "motion_gguf is required"); return nullptr; }
        auto runtime = runtime_options(options);
        if (!runtime) { set_error(nullptr, err, err_len, runtime.error()); return nullptr; }
        if (adapter && *adapter) { set_error(nullptr, err, err_len, "separate text adapters are unsupported; convert a merged native text bundle"); return nullptr; }
        auto loaded = kimodo::model::load(motion, text ? text : "", *runtime);
        if (!loaded) { set_error(nullptr, err, err_len, loaded.error()); return nullptr; }
        return new kimodo_model{std::move(*loaded), {}};
    } catch (const std::exception &e) { set_error(nullptr, err, err_len, e.what()); return nullptr; }
    catch (...) { set_error(nullptr, err, err_len, "unknown C++ exception"); return nullptr; }
}
void kimodo_model_free(kimodo_model *m) { delete m; }
const char *kimodo_model_last_error(const kimodo_model *m) { return m ? m->last_error.c_str() : "invalid model"; }
int kimodo_model_joints(const kimodo_model *m) { return m && m->value ? static_cast<int>(m->value->joints()) : 0; }
kimodo_motion *kimodo_generate_constrained(kimodo_model *m, const char *prompt, const kimodo_generation_options *o,
                                           const kimodo_constraints *c, char *err, int len) {
    try {
        std::string error;
        if (!m || !m->value) { set_error(m, err, len, "invalid model"); return nullptr; }
        if (!prompt) { set_error(m, err, len, "UTF-8 prompt is required"); return nullptr; }
        if (!valid_options(o, error)) { set_error(m, err, len, error); return nullptr; }
        auto options = generation_options(*o);
        if (!copy_constraints(c, m->value->joints(), options, error)) { set_error(m, err, len, error); return nullptr; }
        auto generated = m->value->generate_text(prompt, o->frames, o->diffusion_steps, o->seed, o->text_cfg_weight, o->constraint_cfg_weight, options);
        if (!generated) { set_error(m, err, len, generated.error()); return nullptr; }
        m->last_error.clear(); return new kimodo_motion{std::move(*generated)};
    } catch (const std::exception &x) { set_error(m, err, len, x.what()); return nullptr; }
    catch (...) { set_error(m, err, len, "unknown C++ exception"); return nullptr; }
}
kimodo_motion *kimodo_generate(kimodo_model *m, const char *prompt, const kimodo_generation_options *o, char *err, int len) {
    return kimodo_generate_constrained(m, prompt, o, nullptr, err, len);
}
kimodo_motion *kimodo_generate_embedding(kimodo_model *m, const kimodo_embedding *e, const kimodo_generation_options *o, char *err, int len) {
    try {
        std::string error;
        if (!m || !m->value) { set_error(m, err, len, "invalid model"); return nullptr; }
        if (!e || !e->data || e->values != kimodo::embedding_width) { set_error(m, err, len, "embedding must contain exactly 4096 values"); return nullptr; }
        if (!valid_options(o, error)) { set_error(m, err, len, error); return nullptr; }
        std::array<float, kimodo::embedding_width> values;
        std::copy_n(e->data, values.size(), values.data());
        auto generated = m->value->generate_embedding(values, o->frames, o->diffusion_steps, o->seed, o->text_cfg_weight, o->constraint_cfg_weight, generation_options(*o));
        if (!generated) { set_error(m, err, len, generated.error()); return nullptr; }
        m->last_error.clear(); return new kimodo_motion{std::move(*generated)};
    } catch (const std::exception &x) { set_error(m, err, len, x.what()); return nullptr; }
    catch (...) { set_error(m, err, len, "unknown C++ exception"); return nullptr; }
}
void kimodo_motion_free(kimodo_motion *m) { delete m; }
int kimodo_motion_frames(const kimodo_motion *m) { return m ? static_cast<int>(m->value.frames) : 0; }
int kimodo_motion_joints(const kimodo_motion *m) { return m ? static_cast<int>(m->value.joints) : 0; }
const float *kimodo_motion_local_rotations_xyzw(const kimodo_motion *m) { return m && !m->value.local_rotations_xyzw.empty() ? m->value.local_rotations_xyzw.data() : nullptr; }
const float *kimodo_motion_root_positions(const kimodo_motion *m) { return m && !m->value.root_positions.empty() ? m->value.root_positions.data() : nullptr; }

/* ------------------------------------------------------------------ ABI 3 */

int kimodo_get_capabilities(kimodo_capabilities *out) {
    if (!out || out->size != sizeof(*out)) return -1;
    const auto build = kimodo::build();
    out->abi = KIMODO_CAPI_ABI_VERSION;
    // Static storage: the strings are compile-time literals.
    out->version = build.version.data();
    out->commit = build.commit.data();
    out->devices = 1u << KIMODO_DEVICE_CPU;
    if (build.vulkan) out->devices |= 1u << KIMODO_DEVICE_VULKAN;
    if (build.metal) out->devices |= 1u << KIMODO_DEVICE_METAL;
    out->post_processing = build.post_processing ? 1u : 0u;
    return 0;
}

void kimodo_set_log_callback(kimodo_log_fn callback, void *user_data, kimodo_log_level min_level) {
    try {
        if (!callback) { kimodo::set_log_sink({}); return; }
        const auto minimum = static_cast<kimodo::log_level>(std::clamp(static_cast<int>(min_level), 1, 4));
        kimodo::set_log_sink([callback, user_data](kimodo::log_level level, std::string_view line) {
            const std::string text(line);
            callback(user_data, static_cast<kimodo_log_level>(level), text.c_str());
        }, minimum);
    } catch (...) {
    }
}

int kimodo_gpu_count(void) {
    try {
        quiet();
        return static_cast<int>(kimodo::list_gpus().size());
    } catch (...) { return 0; }
}

int kimodo_gpu_info_get(int gpu_index, kimodo_gpu_info *out) {
    try {
        quiet();
        if (!out || out->size != sizeof(*out) || gpu_index < 0) return -1;
        const auto gpus = kimodo::list_gpus();
        if (static_cast<size_t>(gpu_index) >= gpus.size()) return -1;
        const auto &gpu = gpus[static_cast<size_t>(gpu_index)];
        const size_t n = std::min(gpu.name.size(), sizeof(out->name) - 1);
        std::memcpy(out->name, gpu.name.data(), n);
        out->name[n] = '\0';
        out->memory_total = gpu.memory_total;
        out->memory_free = gpu.memory_free;
        return 0;
    } catch (...) { return -1; }
}

kimodo_model *kimodo_open(const char *data_dir, const char *motion_model, const char *text_model,
                          const kimodo_runtime_options *options, char *err, int err_len) {
    try {
        quiet();
        if (!data_dir || !*data_dir) { set_error(nullptr, err, err_len, "data_dir is required"); return nullptr; }
        const auto folder = kimodo::detail::utf8_path(data_dir);
        const auto motion = folder / kimodo::detail::utf8_path(motion_model ? motion_model : default_motion_model);
        std::error_code ignored;
        if (!std::filesystem::is_regular_file(motion, ignored)) {
            set_error(nullptr, err, err_len, "no motion model " + kimodo::detail::utf8_string(motion));
            return nullptr;
        }
        std::string text;
        const char *text_name = text_model ? text_model : default_text_model;
        if (*text_name) {
            const auto path = folder / kimodo::detail::utf8_path(text_name);
            if (!std::filesystem::is_regular_file(path, ignored)) {
                set_error(nullptr, err, err_len, "no text model " + kimodo::detail::utf8_string(path));
                return nullptr;
            }
            text = kimodo::detail::utf8_string(path);
        }
        auto runtime = runtime_options(options);
        if (!runtime) { set_error(nullptr, err, err_len, runtime.error()); return nullptr; }
        runtime->preload = true;
        auto loaded = kimodo::model::load(kimodo::detail::utf8_string(motion), text, *runtime);
        if (!loaded) { set_error(nullptr, err, err_len, loaded.error()); return nullptr; }
        return new kimodo_model{std::move(*loaded), {}};
    } catch (const std::exception &e) { set_error(nullptr, err, err_len, e.what()); return nullptr; }
    catch (...) { set_error(nullptr, err, err_len, "unknown C++ exception"); return nullptr; }
}

int kimodo_list_motion_models(const char *data_dir, char *buffer, int buffer_len) {
    try {
        if (!data_dir || !*data_dir) return -1;
        std::error_code error;
        std::vector<std::string> names;
        for (const auto &entry : std::filesystem::directory_iterator(kimodo::detail::utf8_path(data_dir), error)) {
            if (!entry.is_regular_file(error)) continue;
            const std::string name = kimodo::detail::utf8_string(entry.path().filename());
            if (name.starts_with("kimodo-") && name.ends_with(".gguf")) names.push_back(name);
        }
        if (error) return -1;
        std::sort(names.begin(), names.end());
        std::string list;
        for (const auto &name : names) list += (list.empty() ? "" : "\n") + name;
        if (buffer && buffer_len > 0) {
            const size_t n = std::min(list.size(), static_cast<size_t>(buffer_len - 1));
            std::memcpy(buffer, list.data(), n);
            buffer[n] = '\0';
        }
        return static_cast<int>(list.size());
    } catch (...) { return -1; }
}

const char *kimodo_model_skeleton(const kimodo_model *m) {
    // Keys and names are literals in skeleton.hpp, so NUL-terminated.
    return m && m->value ? m->value->skeleton().key.data() : "";
}
const char *kimodo_model_joint_name(const kimodo_model *m, int joint) {
    if (!m || !m->value || joint < 0 || static_cast<unsigned>(joint) >= m->value->joints()) return "";
    return m->value->skeleton().names[static_cast<size_t>(joint)].data();
}
int kimodo_model_joint_parent(const kimodo_model *m, int joint) {
    if (!m || !m->value || joint < 0 || static_cast<unsigned>(joint) >= m->value->joints()) return -1;
    return m->value->skeleton().parents[static_cast<size_t>(joint)];
}
int kimodo_model_joint_offset(const kimodo_model *m, int joint, float *out_xyz) {
    if (!m || !m->value || !out_xyz || joint < 0 || static_cast<unsigned>(joint) >= m->value->joints()) return -1;
    const auto &offset = m->value->skeleton().offsets[static_cast<size_t>(joint)];
    std::copy(offset.begin(), offset.end(), out_xyz);
    return 0;
}

int kimodo_model_limits(const kimodo_model *m, kimodo_limits *out) {
    if (!m || !m->value || !out || out->size != sizeof(*out)) return -1;
    fill_limits(m->value->active_limits(), *out);
    return 0;
}
int kimodo_get_limit_ceilings(kimodo_limits *out) {
    if (!out || out->size != sizeof(*out)) return -1;
    fill_limits(kimodo::limit_ceilings, *out);
    return 0;
}

void kimodo_generation_options_init(const kimodo_model *m, kimodo_generation_options *o) {
    if (!o) return;
    *o = kimodo_generation_options{};
    o->size = sizeof(*o);
    o->seed = 0;
    o->frames = 150;
    o->diffusion_steps = 100;
    o->text_cfg_weight = 2.F;
    o->constraint_cfg_weight = 2.F;
    o->first_heading = 0.F;
    o->root_margin = .04F;
    o->transition_frames = 5;
    // Upstream's demo post-processes by default, except on the G1 robot.
    const bool g1 = m && m->value && m->value->skeleton().key == "g1skel34";
    o->post_process = kimodo::model::post_processing_available() && !g1 ? 1u : 0u;
}
void kimodo_runtime_options_init(kimodo_runtime_options *o) {
    if (!o) return;
    *o = kimodo_runtime_options{};
    o->size = sizeof(*o);
}

kimodo_status kimodo_generate_sequence(kimodo_model *m, const kimodo_request *request,
                                       const kimodo_generation_options *o, kimodo_motion **out, char *err, int len) {
    try {
        if (out) *out = nullptr;
        std::string error;
        if (!m || !m->value) return failure(m, err, len, "invalid model");
        if (!out) return failure(m, err, len, "out is required");
        if (!request || request->size != sizeof(*request)) return failure(m, err, len, "invalid kimodo_request");
        if (!request->segments || !request->segment_count) return failure(m, err, len, "a request needs one or more segments");
        if (!valid_options(o, error)) return failure(m, err, len, error);
        auto options = generation_options(*o);
        if (!copy_constraints(request->constraints, m->value->joints(), options, error)) return failure(m, err, len, error);
        std::vector<kimodo::prompt_segment> segments;
        unsigned total_frames = 0;
        for (uint32_t index = 0; index < request->segment_count; ++index) {
            const auto &segment = request->segments[index];
            if (!segment.prompt) return failure(m, err, len, "segment " + std::to_string(index) + " has no prompt");
            segments.push_back({segment.prompt, segment.frames});
            total_frames += segment.frames;
        }
        if (request->constraints_json && *request->constraints_json) {
            auto parsed = m->value->constraints_from_json(request->constraints_json, total_frames);
            if (!parsed) return failure(m, err, len, parsed.error());
            for (auto &constraint : *parsed) options.constraints.push_back(std::move(constraint));
        }
        if (request->progress) {
            options.progress = [callback = request->progress, user = request->progress_user_data](
                                   kimodo::progress_stage stage, unsigned done, unsigned total) {
                return callback(user, static_cast<kimodo_stage>(stage), done, total) == 0;
            };
        }
        auto generated = m->value->generate_text_sequence(segments, transition_frames(*o), o->diffusion_steps, o->seed,
                                                          o->text_cfg_weight, o->constraint_cfg_weight, options);
        if (!generated) return failure(m, err, len, generated.error());
        m->last_error.clear();
        *out = new kimodo_motion{std::move(*generated)};
        return KIMODO_OK;
    } catch (const std::exception &x) { return failure(m, err, len, x.what()); }
    catch (...) { return failure(m, err, len, "unknown C++ exception"); }
}

kimodo_status kimodo_encode_text(kimodo_model *m, const char *prompt, float *out, char *err, int len) {
    try {
        if (!m || !m->value) return failure(m, err, len, "invalid model");
        if (!prompt || !out) return failure(m, err, len, "prompt and out are required");
        auto embedding = m->value->encode_text(prompt);
        if (!embedding) return failure(m, err, len, embedding.error());
        std::copy(embedding->begin(), embedding->end(), out);
        m->last_error.clear();
        return KIMODO_OK;
    } catch (const std::exception &x) { return failure(m, err, len, x.what()); }
    catch (...) { return failure(m, err, len, "unknown C++ exception"); }
}
}
