#include <kimodo/kimodo.hpp>
#include "constraints.hpp"
#include "gguf.hpp"
#ifdef KIMODO_HAVE_POSTPROCESS
#include "postprocess.hpp"
#endif
#include "skeleton.hpp"
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
    std::string motion_path;
    const detail::skeleton_spec *skeleton = nullptr;
#ifdef KIMODO_HAVE_GGML
    mutable std::unique_ptr<detail::ggml_motion_weights> weights;
    std::unique_ptr<detail::llm_text_encoder> text;
    mutable std::mutex inference_mutex;
#endif
};
model::model(std::unique_ptr<impl> state) : impl_(std::move(state)) {}
model::~model() = default;

unsigned model::joints() const noexcept { return static_cast<unsigned>(impl_->skeleton->joints()); }

bool model::post_processing_available() noexcept {
#ifdef KIMODO_HAVE_POSTPROCESS
    return true;
#else
    return false;
#endif
}

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
    auto out = std::make_unique<conditions>();
    if (!options.constraints.empty()) {
        auto condition = detail::build_constraint_condition(skeleton, options.constraints, frames);
        if (!condition) return std::unexpected(condition.error());
        out->condition = std::move(*condition);
    }
    if (options.post_process) {
#ifdef KIMODO_HAVE_POSTPROCESS
        out->targets = detail::build_postprocess_targets(skeleton, options.constraints, frames);
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
} // namespace
#endif

std::expected<std::unique_ptr<model>, std::string> model::load(std::string_view motion_path, std::string_view text_path) {
    auto file = detail::read_gguf_header(motion_path);
    if (!file) return std::unexpected(file.error());
    if (auto valid = detail::validate_motion_gguf(*file); !valid) return std::unexpected(valid.error());
    auto state = std::make_unique<impl>();
    state->motion = std::move(*file);
    state->motion_path = std::string(motion_path);
    state->skeleton = detail::find_skeleton(state->motion.strings.at("kimodo.skeleton"));
#ifdef KIMODO_HAVE_GGML
    if (!text_path.empty()) {
        auto text = detail::llm_text_encoder::load(text_path);
        if (!text) return std::unexpected(text.error());
        state->text = std::move(*text);
    }
#else
    if (!text_path.empty()) return std::unexpected("Kimodo was built without GGML support");
#endif
    return std::unique_ptr<model>(new model(std::move(state)));
}

std::expected<motion_data, std::string> model::generate_text(
    std::string_view utf8_prompt, unsigned frames, unsigned steps, std::uint64_t seed,
    float text_cfg, float constraint_cfg, const generation_options &options) const {
#ifdef KIMODO_HAVE_GGML
    if (!impl_->text) return std::unexpected("model was loaded without a native text bundle");
    auto embedding = impl_->text->encode(utf8_prompt);
    if (!embedding) return std::unexpected(embedding.error());
    return generate_embedding(*embedding, frames, steps, seed, text_cfg, constraint_cfg, options);
#else
    (void) utf8_prompt; (void) frames; (void) steps; (void) seed; (void) text_cfg; (void) constraint_cfg; (void) options;
    return std::unexpected("Kimodo was built without GGML support");
#endif
}

std::expected<motion_data, std::string> model::generate_embedding(
    const std::array<float, embedding_width> &embedding, unsigned frames, unsigned steps,
    std::uint64_t seed, float text_cfg, float constraint_cfg, const generation_options &options) const {
    if (frames == 0 || frames > 10000) return std::unexpected("frames must be in 1..10000");
    if (steps == 0 || steps > 1000) return std::unexpected("diffusion_steps must be in 1..1000");
    if (!std::isfinite(text_cfg) || !std::isfinite(constraint_cfg)) return std::unexpected("CFG weights must be finite");
    for (float value : embedding) if (!std::isfinite(value)) return std::unexpected("embedding contains a non-finite value");
#ifdef KIMODO_HAVE_GGML
    auto conditions = build_conditions(*impl_->skeleton, options, frames);
    if (!conditions) return std::unexpected(conditions.error());
    const std::lock_guard inference_lock(impl_->inference_mutex);
    const auto generate_started = std::chrono::steady_clock::now();
    // Weight residency is deferred until inference so model-load stays a
    // bounded metadata operation.  The graph integration consumes this exact
    // session; no separate unchecked tensor loader exists in the runtime.
    if (!impl_->weights) {
        const auto weights_started = std::chrono::steady_clock::now();
        auto loaded = detail::ggml_motion_weights::load(impl_->motion_path);
        if (!loaded) return std::unexpected(loaded.error());
        impl_->weights = std::move(*loaded);
        if (detail::profile_enabled())
            std::fprintf(stderr, "profile motion.weights_ready_ms=%.3f\n", detail::profile_elapsed_ms(weights_started));
    }
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> normal(0.f, 1.f);
    const size_t motion_dim=impl_->skeleton->motion_dim();
    std::vector<float> noise(static_cast<size_t>(frames)*motion_dim);
    for (float &value : noise) value = normal(rng);
    if (!options.plain()) {
        // A one-segment upstream `_multiprompt`: the conditioned sampler.
        const detail::sampled_sequence_segment segment{embedding, noise, frames};
        auto joined=detail::sample_motion_sequence_from_noise(*impl_->weights,{&segment,1},1,steps,text_cfg,constraint_cfg,
                                                              (*conditions)->user(),options.first_heading,(*conditions)->postprocess());
        if (!joined) return std::unexpected(joined.error());
        return decode_raw_motion(*impl_->weights,*impl_->skeleton,std::move(*joined));
    }
    const auto sampling_started = std::chrono::steady_clock::now();
    auto sampled = detail::sample_motion_from_noise(*impl_->weights, noise, embedding, frames, steps, text_cfg, constraint_cfg);
    if (!sampled) return std::unexpected(sampled.error());
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
        std::fprintf(stderr, "profile motion.generate sampling_ms=%.3f decode_ms=%.3f total_ms=%.3f\n",
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
    if (!impl_->text) return std::unexpected("model was loaded without a native text bundle");
    if (segments.empty() || segments.size() > 16) return std::unexpected("sequence requires 1..16 prompt segments");
    if (steps == 0 || steps > 1000 || transition_frames == 0 || transition_frames > 60)
        return std::unexpected("invalid sequence sampling parameters");
    size_t total_frames=0;
    for (const auto &segment : segments) total_frames+=segment.frames;
    auto conditions = build_conditions(*impl_->skeleton, options, total_frames);
    if (!conditions) return std::unexpected(conditions.error());
    std::vector<std::array<float, embedding_width>> embeddings;
    embeddings.reserve(segments.size());
    for (size_t index=0; index<segments.size(); ++index) {
        const auto &segment=segments[index];
        if (segment.prompt.empty() || segment.frames < 2 || segment.frames > 360)
            return std::unexpected("each sequence segment must contain a prompt and have 2..360 frames");
        if (index && transition_frames >= segment.frames) return std::unexpected("transition must be shorter than every following segment");
        auto embedding=impl_->text->encode(segment.prompt);
        if (!embedding) return std::unexpected(embedding.error());
        embeddings.push_back(*embedding);
    }
    const std::lock_guard inference_lock(impl_->inference_mutex);
    // Initialize and warm the quantized text backend before the F32 motion
    // backend applies its process-wide Vulkan parity flags. This preserves
    // cooperative-matrix text kernels in persistent sequence workers.
    if (!impl_->weights) {
        auto loaded = detail::ggml_motion_weights::load(impl_->motion_path);
        if (!loaded) return std::unexpected(loaded.error());
        impl_->weights = std::move(*loaded);
    }
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
    auto joined=detail::sample_motion_sequence_from_noise(*impl_->weights,sampled,transition_frames,steps,text_cfg,constraint_cfg,
                                                          (*conditions)->user(),options.first_heading,(*conditions)->postprocess());
    if (!joined) return std::unexpected(joined.error());
    return decode_raw_motion(*impl_->weights,*impl_->skeleton,std::move(*joined));
#else
    (void) segments; (void) transition_frames; (void) steps; (void) seed; (void) text_cfg; (void) constraint_cfg; (void) options;
    return std::unexpected("Kimodo was built without GGML support");
#endif
}
} // namespace kimodo
