#include <kimodo/kimodo.hpp>

#if defined(KIMODO_HAVE_GGML_VULKAN)
#include <ggml-vulkan.h>
#endif

#ifndef KIMODO_VERSION_STRING
#define KIMODO_VERSION_STRING "unknown"
#endif
#ifndef KIMODO_COMMIT_STRING
#define KIMODO_COMMIT_STRING "unknown"
#endif

namespace kimodo {

build_information build() noexcept {
    build_information out;
    out.version = KIMODO_VERSION_STRING;
    out.commit = KIMODO_COMMIT_STRING;
#if defined(KIMODO_HAVE_GGML_VULKAN)
    out.vulkan = true;
#endif
    out.post_processing = model::post_processing_available();
    return out;
}

std::vector<gpu_description> list_gpus() {
    std::vector<gpu_description> gpus;
#if defined(KIMODO_HAVE_GGML_VULKAN)
    const int count = ggml_backend_vk_get_device_count();
    for (int index = 0; index < count; ++index) {
        char name[256] = {};
        ggml_backend_vk_get_device_description(index, name, sizeof name);
        size_t free = 0, total = 0;
        ggml_backend_vk_get_device_memory(index, &free, &total);
        gpus.push_back({name, total, free});
    }
#endif
    return gpus;
}

} // namespace kimodo
