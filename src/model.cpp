#include <kimodo/kimodo.hpp>
#include "constraints.hpp"
#include "gguf.hpp"
#include "log.hpp"
#ifdef KIMODO_HAVE_POSTPROCESS
#include "postprocess.hpp"
#endif
#include "skeleton.hpp"
#include "utf8_path.hpp"
#ifdef KIMODO_HAVE_GGML
#include "ggml_weights.hpp"
#include "denoiser.hpp"
#include "motion_decode.hpp"
#include "llm_text_encoder.hpp"
#include "profile.hpp"
#endif

#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <optional>
#include <random>

namespace kimodo {
struct model::impl {
    detail::gguf_file motion;
    std::string motion_path; // UTF-8
    const detail::skeleton_spec *skeleton = nullptr;
    runtime_options options;
    limits active;
#ifdef KIMODO_HAVE_GGML
    mutable std::unique_ptr<detail::ggml_motion_weights> weights;
    std::unique_ptr<detail::llm_text_encoder> text;
    mutable std::mutex inference_mutex;

    // Weight residency is deferred until inference, or preload(), so loading
    // a model stays a bounded metadata operation.  The graph integration
    // consumes this exact session; no separate unchecked tensor loader exists
    // in the runtime.  The caller holds inference_mutex.
    std::expected<void, std::string> ensure_weights(const progress_callback &progress) const {
        if (weights) return {};
        if (progress && !progress(progress_stage::loading, 0, 1)) return std::unexpected(std::string(cancelled_error));
        const auto started = std::chrono::steady_clock::now();
        auto loaded = detail::ggml_motion_weights::load(motion_path, options);
        if (!loaded) return std::unexpected(loaded.error());
        weights = std::move(*loaded);
        detail::logf(log_level::info, "motion model: %s weights on the device in %.1f s",
                     std::string(skeleton->key).c_str(), detail::profile_elapsed_ms(started) / 1000.0);
        if (progress && !progress(progress_stage::loading, 1, 1)) return std::unexpected(std::string(cancelled_error));
        return {};
    }
#endif
};
model::model(std::unique_ptr<impl> state) : impl_(std::move(state)) {}
model::~model() = default;

unsigned model::joints() const noexcept { return static_cast<unsigned>(impl_->skeleton->joints()); }
skeleton_view model::skeleton() const noexcept {
    const auto &s = *impl_->skeleton;
    return {s.key, s.names, s.parents, s.offsets};
}
const limits &model::active_limits() const noexcept { return impl_->active; }
bool model::has_text_encoder() const noexcept {
#ifdef KIMODO_HAVE_GGML
    return impl_->text != nullptr;
#else
    return false;
#endif
}

bool model::post_processing_available() noexcept {
#ifdef KIMODO_HAVE_POSTPROCESS
    return true;
#else
    return false;
#endif
}

namespace {
// The defaults, with what `options` raise or lower, within the ceilings.
limits limits_for(const runtime_options &options) {
    limits out = default_limits;
    if (options.max_segment_frames)
        out.max_segment_frames = std::clamp(options.max_segment_frames, limit_ceilings.min_segment_frames,
                                            limit_ceilings.max_segment_frames);
    if (options.max_segments) out.max_segments = std::min(options.max_segments, limit_ceilings.max_segments);
    return out;
}

std::string range(unsigned low, unsigned high) { return std::to_string(low) + ".." + std::to_string(high); }

std::expected<void, std::string> check_prompt(const limits &active, std::string_view prompt) {
    if (prompt.empty()) return std::unexpected("a prompt is required");
    if (prompt.size() > active.max_prompt_bytes)
        return std::unexpected("a prompt may hold at most " + std::to_string(active.max_prompt_bytes) + " UTF-8 bytes");
    return {};
}

std::expected<void, std::string> check_sampling(const limits &active, unsigned steps, float text_cfg, float constraint_cfg,
                                                size_t constraints) {
    if (steps == 0 || steps > active.max_diffusion_steps)
        return std::unexpected("diffusion_steps must be in " + range(1, active.max_diffusion_steps));
    if (!std::isfinite(text_cfg) || !std::isfinite(constraint_cfg)) return std::unexpected("CFG weights must be finite");
    if (constraints > active.max_constraints)
        return std::unexpected("at most " + std::to_string(active.max_constraints) + " constraints");
    return {};
}

// A sampler's failure, with what to do when the device ran out of memory:
// one attention buffer grows with the square of a segment's frames.
std::string sampling_error(std::string error, size_t longest_segment) {
    if (error.find("allocation failed") != std::string::npos)
        error += ": the device has too little free memory for a segment of " + std::to_string(longest_segment) +
                 " frames; generate shorter segments";
    return error;
}

bool cancelled(const generation_options &options, progress_stage stage, unsigned done, unsigned total) {
    return options.progress && !options.progress(stage, done, total);
}
} // namespace

#ifdef KIMODO_HAVE_GGML
namespace {
// What a clip of `frames` is conditioned on and post-processed against.
struct conditions {
    std::optional<detail::constraint_condition> condition;
#ifdef KIMODO_HAVE_POSTPROCESS
    detail::postprocess_targets targets;
#endif
    detail::sequence_postprocess post;

    [[nodiscard]] const detail::constraint_condition *user() const noexcept { return condition ? &*condition : nullptr; }
    [[nodiscard]] const detail::sequence_postprocess *postprocess() const noexcept { return post.targets ? &post : nullptr; }
};

std::expected<std::unique_ptr<conditions>, std::string> build_conditions(
    const detail::skeleton_spec &skeleton, const generation_options &options, size_t frames) {
    if (!std::isfinite(options.first_heading)) return std::unexpected("first_heading must be finite");
    if (!std::isfinite(options.root_margin) || options.root_margin < 0.F) return std::unexpected("root_margin must be a non-negative distance");
    // Every pose as XYZW in the skeleton's joint order.
    auto constraints = detail::canonical_constraints(skeleton, options.constraints);
    if (!constraints) return std::unexpected(constraints.error());
    auto out = std::make_unique<conditions>();
    if (!constraints->empty()) {
        auto condition = detail::build_constraint_condition(skeleton, *constraints, frames);
        if (!condition) return std::unexpected(condition.error());
        out->condition = std::move(*condition);
    }
    if (options.post_process) {
#ifdef KIMODO_HAVE_POSTPROCESS
        out->targets = detail::build_postprocess_targets(skeleton, *constraints, frames);
        out->post = {&out->targets, options.root_margin};
#else
        return std::unexpected("this build has no post-processing (it needs x86 and the eigen submodule)");
#endif
    }
    return out;
}

// A raw joined representation from the sequence sampler -> the caller's motion.
std::expected<motion_data, std::string> decode_raw_motion(
    const detail::ggml_motion_weights &weights, const detail::skeleton_spec &skeleton, std::vector<float> raw) {
    auto bm=weights.f32_values("stats.body.mean"), bs=weights.f32_values("stats.body.std");
    auto gm=weights.f32_values("stats.global_root.mean"), gs=weights.f32_values("stats.global_root.std");
    if (!gm || !gs || !bm || !bs) return std::unexpected("motion GGUF lacks normalization statistics");
    const size_t motion_dim=skeleton.motion_dim();
    const auto frames=static_cast<unsigned>(raw.size()/motion_dim);
    detail::normalize_motion_rows(raw, motion_dim, *gm, *gs, *bm, *bs);
    auto decoded=detail::decode_motion(raw,frames,skeleton,*gm,*gs,*bm,*bs);
    if (!decoded) return std::unexpected(decoded.error());
    motion_data result; result.frames=frames; result.joints=static_cast<unsigned>(skeleton.joints());
    result.local_rotations_xyzw=std::move(decoded->local_xyzw); result.root_positions=std::move(decoded->root_positions);
    return result;
}

// Reports sampling progress, `total` diffusion steps over every segment, and
// carries a cancel back into the sampler.
struct sampling_progress {
    detail::sampling_observer observer;
    mutable unsigned done = 0;

    sampling_progress(const generation_options &options, unsigned total) {
        if (!options.progress) return;
        observer = [this, &options, total] { return options.progress(progress_stage::sampling, ++done, total); };
    }
    sampling_progress(const sampling_progress &) = delete; // the observer holds `this`
    sampling_progress &operator=(const sampling_progress &) = delete;
    [[nodiscard]] const detail::sampling_observer *get() const noexcept { return observer ? &observer : nullptr; }
};
} // namespace
#endif

std::expected<std::unique_ptr<model>, std::string> model::load(
    std::string_view motion_path, std::string_view text_path, const runtime_options &options) {
    auto file = detail::read_gguf_header(motion_path);
    if (!file) return std::unexpected(file.error());
    if (auto valid = detail::validate_motion_gguf(*file); !valid) return std::unexpected(valid.error());
    auto state = std::make_unique<impl>();
    state->motion = std::move(*file);
    state->motion_path = std::string(motion_path);
    state->skeleton = detail::find_skeleton(state->motion.strings.at("kimodo.skeleton"));
    state->options = options;
    state->active = limits_for(options);
    detail::logf(log_level::info, "motion model: %s (%s)",
                 detail::utf8_string(detail::utf8_path(motion_path).filename()).c_str(), std::string(state->skeleton->key).c_str());
#ifdef KIMODO_HAVE_GGML
    if (!text_path.empty()) {
        auto text = detail::llm_text_encoder::load(text_path, options);
        if (!text) return std::unexpected(text.error());
        state->text = std::move(*text);
    }
    if (options.preload)
        if (auto loaded = state->ensure_weights({}); !loaded) return std::unexpected(loaded.error());
#else
    if (!text_path.empty()) return std::unexpected("Kimodo was built without GGML support");
    if (options.preload) return std::unexpected("Kimodo was built without GGML support");
#endif
    return std::unique_ptr<model>(new model(std::move(state)));
}

std::expected<void, std::string> model::preload() const {
#ifdef KIMODO_HAVE_GGML
    const std::lock_guard inference_lock(impl_->inference_mutex);
    return impl_->ensure_weights({});
#else
    return std::unexpected("Kimodo was built without GGML support");
#endif
}

std::expected<std::array<float, embedding_width>, std::string> model::encode_text(std::string_view utf8_prompt) const {
    if (auto checked = check_prompt(impl_->active, utf8_prompt); !checked) return std::unexpected(checked.error());
#ifdef KIMODO_HAVE_GGML
    if (!impl_->text) return std::unexpected("model was loaded without a native text bundle");
    return impl_->text->encode(utf8_prompt);
#else
    return std::unexpected("Kimodo was built without GGML support");
#endif
}

std::expected<std::vector<motion_constraint>, std::string> model::constraints_from_json(
    std::string_view json, unsigned total_frames) const {
    return detail::constraints_from_json(json, *impl_->skeleton, total_frames, impl_->active.max_constraints);
}

std::expected<motion_data, std::string> model::generate_text(
    std::string_view utf8_prompt, unsigned frames, unsigned steps, std::uint64_t seed,
    float text_cfg, float constraint_cfg, const generation_options &options) const {
#ifdef KIMODO_HAVE_GGML
    if (!impl_->text) return std::unexpected("model was loaded without a native text bundle");
    if (auto checked = check_prompt(impl_->active, utf8_prompt); !checked) return std::unexpected(checked.error());
    if (cancelled(options, progress_stage::encoding_text, 0, 1)) return std::unexpected(std::string(cancelled_error));
    auto embedding = impl_->text->encode(utf8_prompt);
    if (!embedding) return std::unexpected(embedding.error());
    if (cancelled(options, progress_stage::encoding_text, 1, 1)) return std::unexpected(std::string(cancelled_error));
    return generate_embedding(*embedding, frames, steps, seed, text_cfg, constraint_cfg, options);
#else
    (void) utf8_prompt; (void) frames; (void) steps; (void) seed; (void) text_cfg; (void) constraint_cfg; (void) options;
    return std::unexpected("Kimodo was built without GGML support");
#endif
}

std::expected<motion_data, std::string> model::generate_embedding(
    const std::array<float, embedding_width> &embedding, unsigned frames, unsigned steps,
    std::uint64_t seed, float text_cfg, float constraint_cfg, const generation_options &options) const {
    const limits &active = impl_->active;
    if (frames < active.min_segment_frames || frames > active.max_segment_frames)
        return std::unexpected("frames must be in " + range(active.min_segment_frames, active.max_segment_frames));
    if (auto checked = check_sampling(active, steps, text_cfg, constraint_cfg, options.constraints.size()); !checked)
        return std::unexpected(checked.error());
    for (float value : embedding) if (!std::isfinite(value)) return std::unexpected("embedding contains a non-finite value");
#ifdef KIMODO_HAVE_GGML
    auto conditions = build_conditions(*impl_->skeleton, options, frames);
    if (!conditions) return std::unexpected(conditions.error());
    const std::lock_guard inference_lock(impl_->inference_mutex);
    const auto generate_started = std::chrono::steady_clock::now();
    if (auto loaded = impl_->ensure_weights(options.progress); !loaded) return std::unexpected(loaded.error());
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> normal(0.f, 1.f);
    const size_t motion_dim=impl_->skeleton->motion_dim();
    std::vector<float> noise(static_cast<size_t>(frames)*motion_dim);
    for (float &value : noise) value = normal(rng);
    if (cancelled(options, progress_stage::sampling, 0, steps)) return std::unexpected(std::string(cancelled_error));
    const sampling_progress progress(options, steps);
    if (!options.plain()) {
        // A one-segment upstream `_multiprompt`: the conditioned sampler.
        const detail::sampled_sequence_segment segment{embedding, noise, frames};
        auto joined=detail::sample_motion_sequence_from_noise(*impl_->weights,{&segment,1},1,steps,text_cfg,constraint_cfg,
                                                              (*conditions)->user(),options.first_heading,(*conditions)->postprocess(),
                                                              progress.get());
        if (!joined) return std::unexpected(sampling_error(joined.error(), frames));
        return decode_raw_motion(*impl_->weights,*impl_->skeleton,std::move(*joined));
    }
    const auto sampling_started = std::chrono::steady_clock::now();
    auto sampled = detail::sample_motion_from_noise(*impl_->weights, noise, embedding, frames, steps, text_cfg, constraint_cfg,
                                                    progress.get());
    if (!sampled) return std::unexpected(sampling_error(sampled.error(), frames));
    const double sampling_ms = detail::profile_elapsed_ms(sampling_started);
    auto global_mean=impl_->weights->f32_values("stats.global_root.mean"), global_std=impl_->weights->f32_values("stats.global_root.std");
    auto body_mean=impl_->weights->f32_values("stats.body.mean"), body_std=impl_->weights->f32_values("stats.body.std");
    if (!global_mean) return std::unexpected(global_mean.error());
    if (!global_std) return std::unexpected(global_std.error());
    if (!body_mean) return std::unexpected(body_mean.error());
    if (!body_std) return std::unexpected(body_std.error());
    const auto decode_started = std::chrono::steady_clock::now();
    auto decoded=detail::decode_motion(*sampled,frames,*impl_->skeleton,*global_mean,*global_std,*body_mean,*body_std);
    if (!decoded) return std::unexpected(decoded.error());
    motion_data result;
    result.frames=frames; result.joints=static_cast<unsigned>(impl_->skeleton->joints());
    result.local_rotations_xyzw=std::move(decoded->local_xyzw);
    result.root_positions=std::move(decoded->root_positions);
    if (detail::profile_enabled())
        detail::logf(log_level::debug, "profile motion.generate sampling_ms=%.3f decode_ms=%.3f total_ms=%.3f",
                     sampling_ms, detail::profile_elapsed_ms(decode_started),
                     detail::profile_elapsed_ms(generate_started));
    return result;
#else
    return std::unexpected("Kimodo was built without GGML support");
#endif
}

std::expected<motion_data, std::string> model::generate_text_sequence(
    std::span<const prompt_segment> segments, unsigned transition_frames,
    unsigned steps, std::uint64_t seed, float text_cfg, float constraint_cfg,
    const generation_options &options) const {
#ifdef KIMODO_HAVE_GGML
    const limits &active = impl_->active;
    if (!impl_->text) return std::unexpected("model was loaded without a native text bundle");
    if (segments.empty() || segments.size() > active.max_segments)
        return std::unexpected("a sequence needs " + range(1, active.max_segments) + " prompt segments");
    if (transition_frames == 0 || transition_frames > active.max_transition_frames)
        return std::unexpected("transition_frames must be in " + range(1, active.max_transition_frames));
    if (auto checked = check_sampling(active, steps, text_cfg, constraint_cfg, options.constraints.size()); !checked)
        return std::unexpected(checked.error());
    size_t total_frames=0;
    for (size_t index=0; index<segments.size(); ++index) {
        const auto &segment=segments[index];
        if (auto checked = check_prompt(active, segment.prompt); !checked) return std::unexpected(checked.error());
        if (segment.frames < active.min_segment_frames || segment.frames > active.max_segment_frames)
            return std::unexpected("each sequence segment must have " + range(active.min_segment_frames, active.max_segment_frames) + " frames");
        if (index && transition_frames >= segment.frames) return std::unexpected("transition must be shorter than every following segment");
        total_frames+=segment.frames;
    }
    auto conditions = build_conditions(*impl_->skeleton, options, total_frames);
    if (!conditions) return std::unexpected(conditions.error());
    const auto segment_count = static_cast<unsigned>(segments.size());
    std::vector<std::array<float, embedding_width>> embeddings;
    embeddings.reserve(segments.size());
    if (cancelled(options, progress_stage::encoding_text, 0, segment_count)) return std::unexpected(std::string(cancelled_error));
    for (const auto &segment : segments) {
        auto embedding=impl_->text->encode(segment.prompt);
        if (!embedding) return std::unexpected(embedding.error());
        embeddings.push_back(*embedding);
        if (cancelled(options, progress_stage::encoding_text, static_cast<unsigned>(embeddings.size()), segment_count))
            return std::unexpected(std::string(cancelled_error));
    }
    const std::lock_guard inference_lock(impl_->inference_mutex);
    // Initialize and warm the quantized text backend before the F32 motion
    // backend applies its Vulkan parity switches. This preserves
    // cooperative-matrix text kernels in persistent sequence workers.
    if (auto loaded = impl_->ensure_weights(options.progress); !loaded) return std::unexpected(loaded.error());
    auto bm=impl_->weights->f32_values("stats.body.mean"), bs=impl_->weights->f32_values("stats.body.std");
    auto gm=impl_->weights->f32_values("stats.global_root.mean"), gs=impl_->weights->f32_values("stats.global_root.std");
    if (!gm || !gs || !bm || !bs) return std::unexpected("motion GGUF lacks normalization statistics");
    std::mt19937_64 rng(seed); std::normal_distribution<float> normal(0.f, 1.f);
    std::vector<std::vector<float>> noise;
    std::vector<detail::sampled_sequence_segment> sampled;
    noise.reserve(segments.size()); sampled.reserve(segments.size());
    for (size_t index=0; index<segments.size(); ++index) {
        const auto &segment=segments[index];
        const auto sampled_frames = static_cast<size_t>(segment.frames) +
            (index == 0 ? 0 : transition_frames);
        noise.emplace_back(sampled_frames*impl_->skeleton->motion_dim());
        for (float &value : noise.back()) value=normal(rng);
        sampled.push_back({embeddings[index], noise.back(), segment.frames});
    }
    const unsigned total_steps = steps * segment_count;
    if (cancelled(options, progress_stage::sampling, 0, total_steps)) return std::unexpected(std::string(cancelled_error));
    const sampling_progress progress(options, total_steps);
    auto joined=detail::sample_motion_sequence_from_noise(*impl_->weights,sampled,transition_frames,steps,text_cfg,constraint_cfg,
                                                          (*conditions)->user(),options.first_heading,(*conditions)->postprocess(),
                                                          progress.get());
    if (!joined) {
        size_t longest = 0;
        for (const auto &segment : segments) longest = std::max<size_t>(longest, segment.frames);
        return std::unexpected(sampling_error(joined.error(), longest));
    }
    return decode_raw_motion(*impl_->weights,*impl_->skeleton,std::move(*joined));
#else
    (void) segments; (void) transition_frames; (void) steps; (void) seed; (void) text_cfg; (void) constraint_cfg; (void) options;
    return std::unexpected("Kimodo was built without GGML support");
#endif
}
} // namespace kimodo
