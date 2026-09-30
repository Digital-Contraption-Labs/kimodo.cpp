#include "ggml_weights.hpp"
#include "backend.hpp"
#include "gguf.hpp"
#include "motion_graph_cache.hpp"
#include "profile.hpp"
#include "utf8_path.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

#include <ggml.h>
#include <ggml-alloc.h>
#include <ggml-backend.h>
#include <ggml-cpu.h>
#include <gguf.h>
#if defined(KIMODO_HAVE_GGML_VULKAN)
#include <ggml-vulkan.h>
#endif

namespace kimodo::detail {

std::expected<std::unique_ptr<ggml_motion_weights>, std::string> ggml_motion_weights::load(
    std::string_view path, const runtime_options &options) {
    const auto load_started = std::chrono::steady_clock::now();
    auto checked = read_gguf_header(path);
    if (!checked) return std::unexpected(checked.error());
    if (auto valid = validate_motion_gguf(*checked); !valid) return std::unexpected(valid.error());
    auto result = std::unique_ptr<ggml_motion_weights>(new ggml_motion_weights);
    result->options_ = options;
    result->skeleton_ = checked->strings.at("kimodo.skeleton");
    result->motion_dim_ = static_cast<size_t>(checked->uints.at("kimodo.motion_dim"));
    result->body_dim_ = static_cast<size_t>(checked->uints.at("kimodo.body_dim"));
    gguf_init_params params{true, &result->context_};
    // ggml opens paths as UTF-8.
    result->gguf_ = gguf_init_from_file(std::string(path).c_str(), params);
    if (!result->gguf_ || !result->context_) return std::unexpected("GGML could not load checked motion GGUF");
    auto backend = start_backend(options, true);
    if (!backend) return std::unexpected(backend.error());
    result->backend_ = *backend;
    result->buffer_ = ggml_backend_alloc_ctx_tensors(result->context_, result->backend_);
    if (!result->buffer_) return std::unexpected("GGML motion weight allocation failed");
    result->allocator_ = ggml_gallocr_new(ggml_backend_get_default_buffer_type(result->backend_));
    if (!result->allocator_) return std::unexpected("GGML motion compute allocator creation failed");
    const double allocation_ms = profile_elapsed_ms(load_started);
    std::ifstream input(utf8_path(path), std::ios::binary);
    if (!input) return std::unexpected("cannot reopen motion GGUF");
    const size_t data_start = gguf_get_data_offset(result->gguf_);
    std::vector<char> scratch(8U*1024U*1024U);
    for (int64_t i=0;i<gguf_get_n_tensors(result->gguf_);++i) {
        auto *tensor = ggml_get_tensor(result->context_, gguf_get_tensor_name(result->gguf_, i));
        if (!tensor || tensor->type != GGML_TYPE_F32) return std::unexpected("motion GGUF contains an invalid non-F32 tensor");
        const size_t bytes=ggml_nbytes(tensor), offset=gguf_get_tensor_offset(result->gguf_, i);
        input.seekg(static_cast<std::streamoff>(data_start+offset));
        for(size_t done=0;done<bytes;) {
            const size_t chunk=std::min(scratch.size(), bytes-done);
            input.read(scratch.data(), static_cast<std::streamsize>(chunk));
            if (!input) return std::unexpected("short tensor data in motion GGUF");
            ggml_backend_tensor_set(tensor, scratch.data(), done, chunk); done+=chunk;
        }
    }
    if (profile_enabled()) {
        logf(log_level::debug, "profile motion.weights allocation_ms=%.3f upload_ms=%.3f total_ms=%.3f",
             allocation_ms, profile_elapsed_ms(load_started) - allocation_ms,
             profile_elapsed_ms(load_started));
    }
    return result;
}
ggml_motion_weights::~ggml_motion_weights() {
    graph_cache_.reset();
    if (allocator_) ggml_gallocr_free(allocator_);
    if (buffer_) ggml_backend_buffer_free(buffer_);
    if (gguf_) gguf_free(gguf_);
    if (context_) ggml_free(context_);
    if (backend_) ggml_backend_free(backend_);
}
motion_graph_cache *ggml_motion_weights::graph_cache() const noexcept {
    return graph_cache_.get();
}
void ggml_motion_weights::graph_cache(std::unique_ptr<motion_graph_cache> cache) const noexcept {
    graph_cache_ = std::move(cache);
}
ggml_tensor *ggml_motion_weights::tensor(std::string_view name) const {
    return context_ ? ggml_get_tensor(context_, std::string(name).c_str()) : nullptr;
}
std::expected<std::vector<float>, std::string> ggml_motion_weights::f32_values(std::string_view name) const {
    const bool cache_on_host = name.starts_with("stats.");
    if (cache_on_host) {
        const auto found = host_f32_cache_.find(std::string(name));
        if (found != host_f32_cache_.end()) return found->second;
    }
    auto *value = tensor(name);
    if (!value || value->type != GGML_TYPE_F32) return std::unexpected("missing F32 GGML tensor: " + std::string(name));
    std::vector<float> result(static_cast<size_t>(ggml_nelements(value)));
    ggml_backend_tensor_get(value, result.data(), 0, result.size()*sizeof(float));
    if (cache_on_host) host_f32_cache_.emplace(std::string(name), result);
    return result;
}
} // namespace kimodo::detail
