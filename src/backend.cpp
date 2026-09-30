#include "backend.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include <ggml-backend.h>
#include <ggml-cpu.h>
#if defined(KIMODO_HAVE_GGML_VULKAN)
#include <ggml-vulkan.h>
#endif

namespace kimodo::detail {
namespace {

#if defined(KIMODO_HAVE_GGML_VULKAN)
// Kimodo's reference model is F32.  ggml's Vulkan cooperative-matrix paths
// convert F32 inputs to FP16 on current GPUs, which breaks parity, and ggml
// offers no switch but these variables, read while it creates a device.  They
// are set for that moment only, and a value the process already has is kept:
// the library leaves its host's environment as it found it.
class vulkan_f32_parity {
public:
    vulkan_f32_parity() {
        for (size_t i = 0; i < names.size(); ++i) {
            if (present(names[i])) continue;
            set(names[i], "1");
            added[i] = true;
        }
    }
    ~vulkan_f32_parity() {
        for (size_t i = 0; i < names.size(); ++i)
            if (added[i]) set(names[i], nullptr);
    }
    vulkan_f32_parity(const vulkan_f32_parity &) = delete;
    vulkan_f32_parity &operator=(const vulkan_f32_parity &) = delete;
private:
    static bool present(const char *name) {
#if defined(_MSC_VER)
        size_t length = 0;
        return getenv_s(&length, nullptr, 0, name) == 0 && length > 0;
#else
        return std::getenv(name) != nullptr;
#endif
    }
    static void set(const char *name, const char *value) {
#if defined(_WIN32)
        _putenv_s(name, value ? value : ""); // an empty value removes it
#else
        if (value) setenv(name, value, 1);
        else unsetenv(name);
#endif
    }
    static constexpr std::array<const char *, 3> names{
        "GGML_VK_DISABLE_COOPMAT", "GGML_VK_DISABLE_COOPMAT2", "GGML_VK_DISABLE_F16"};
    std::array<bool, 3> added{};
};
// Models starting on several threads would race on the environment.
std::mutex backend_start_mutex;
#endif

} // namespace

std::expected<ggml_backend *, std::string> start_backend(const runtime_options &options, bool f32_parity) {
    ggml_backend_t backend = nullptr;
#if defined(KIMODO_HAVE_GGML_VULKAN)
    if (options.backend != device::cpu) {
        const std::scoped_lock lock(backend_start_mutex);
        const int count = ggml_backend_vk_get_device_count();
        if (count > 0) {
            if (options.gpu_index >= static_cast<unsigned>(count))
                return std::unexpected("gpu_index " + std::to_string(options.gpu_index) + " is out of range: there " +
                                       (count == 1 ? std::string("is 1 GPU") : "are " + std::to_string(count) + " GPUs"));
            std::optional<vulkan_f32_parity> parity;
            if (f32_parity) parity.emplace();
            backend = ggml_backend_vk_init(options.gpu_index);
            if (!backend) return std::unexpected("cannot start Vulkan on GPU " + std::to_string(options.gpu_index));
        } else if (options.backend == device::vulkan) {
            return std::unexpected("no Vulkan GPU was found");
        }
    }
#else
    (void) f32_parity;
    if (options.backend == device::vulkan) return std::unexpected("this build has no Vulkan backend");
#endif
    if (!backend) {
        backend = ggml_backend_cpu_init();
        if (!backend) return std::unexpected("GGML CPU backend initialization failed");
        // ggml's CPU backend defaults to four threads, a small fraction of a
        // typical workstation.
        unsigned threads = options.threads ? options.threads : std::thread::hardware_concurrency();
        if (threads == 0) threads = 4;
        ggml_backend_cpu_set_n_threads(backend, static_cast<int>(std::min<unsigned>(threads, std::numeric_limits<int>::max())));
    }
    return backend;
}

} // namespace kimodo::detail
