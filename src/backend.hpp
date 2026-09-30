#pragma once

#include <kimodo/kimodo.hpp>

#include <expected>
#include <string>

struct ggml_backend;

namespace kimodo::detail {

// Starts the backend `options` select: the Vulkan GPU `gpu_index` unless the
// CPU is asked for or there is no GPU (an error when Vulkan was asked for),
// else the CPU with `threads` threads.  `f32_parity` switches ggml's Vulkan
// cooperative-matrix and FP16 paths off, which the F32 motion model needs
// for reference results; ggml reads those switches only while it creates a
// device, so they are set around that and the environment is restored, and
// a device the text encoder created first keeps its own.
std::expected<ggml_backend *, std::string> start_backend(const runtime_options &options, bool f32_parity);

} // namespace kimodo::detail
