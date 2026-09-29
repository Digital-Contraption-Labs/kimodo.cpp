#include <kimodo/kimodo_capi.h>
#include <kimodo/kimodo.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct kimodo_model { std::unique_ptr<kimodo::model> value; std::string last_error; };
struct kimodo_motion { kimodo::motion_data value; };
namespace {
void set_error(kimodo_model *model, char *buffer, int length, const std::string &message) noexcept {
    if (model) model->last_error = message;
    if (!buffer || length <= 0) return;
    const size_t n = std::min(message.size(), static_cast<size_t>(length - 1));
    std::memcpy(buffer, message.data(), n); buffer[n] = '\0';
}
// A version-1 caller's options end at constraint_cfg_weight.
constexpr uint32_t options_v1_size = offsetof(kimodo_generation_options, first_heading);
bool valid_options(const kimodo_generation_options *o, std::string &error) {
    if (!o || (o->size != sizeof(*o) && o->size != options_v1_size)) { error = "invalid kimodo_generation_options"; return false; }
    return true;
}
kimodo::generation_options generation_options(const kimodo_generation_options &o) {
    kimodo::generation_options out;
    if (o.size == sizeof(o)) {
        out.first_heading = o.first_heading;
        out.post_process = o.post_process != 0;
        out.root_margin = o.root_margin;
    }
    return out;
}
// Borrowed C constraint arrays -> owned C++ constraints; shapes are checked
// again, against the skeleton and clip, when the condition is built.
bool copy_constraints(const kimodo_constraints *in, unsigned joints, kimodo::generation_options &out, std::string &error) {
    if (!in) return true;
    if (in->size != sizeof(*in) || (in->count && !in->items)) { error = "invalid kimodo_constraints"; return false; }
    for (uint32_t index = 0; index < in->count; ++index) {
        const auto &c = in->items[index];
        if (c.size != sizeof(c) || !c.frame_count || !c.frames) { error = "invalid kimodo_constraint " + std::to_string(index); return false; }
        const size_t n = c.frame_count;
        const auto copy = [](const float *values, size_t count) { return values ? std::vector<float>(values, values + count) : std::vector<float>{}; };
        kimodo::motion_constraint constraint;
        constraint.type = static_cast<kimodo::constraint_type>(c.type);
        constraint.end_effectors = c.end_effectors;
        constraint.frames.assign(c.frames, c.frames + n);
        constraint.root_positions = copy(c.root_positions, n * 3);
        constraint.local_rotations_xyzw = copy(c.local_rotations_xyzw, n * joints * 4);
        constraint.smooth_root_2d = copy(c.smooth_root_2d, n * 2);
        constraint.root_heading = copy(c.root_heading, n * 2);
        out.constraints.push_back(std::move(constraint));
    }
    return true;
}
}
extern "C" {
int kimodo_abi_version(void) { return KIMODO_CAPI_ABI_VERSION; }
kimodo_model *kimodo_model_load(const char *motion, const char *text, const char *adapter, const kimodo_runtime_options *options, char *err, int err_len) {
    try {
        if (!motion || !*motion) { set_error(nullptr, err, err_len, "motion_gguf is required"); return nullptr; }
        if (options && options->size != sizeof(*options)) { set_error(nullptr, err, err_len, "invalid kimodo_runtime_options"); return nullptr; }
        if (adapter && *adapter) { set_error(nullptr, err, err_len, "separate text adapters are unsupported; convert a merged native text bundle"); return nullptr; }
        auto loaded = kimodo::model::load(motion, text ? text : "");
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
}
