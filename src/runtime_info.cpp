#include <kimodo/kimodo.hpp>

#if defined(KIMODO_HAVE_GGML_VULKAN)
#include <ggml-vulkan.h>
#endif
#if defined(KIMODO_HAVE_GGML_METAL)
#include <ggml-backend.h>
#include <ggml-metal.h>
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
#if defined(KIMODO_HAVE_GGML_METAL)
    out.metal = true;
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
#if defined(KIMODO_HAVE_GGML_METAL)
    // Metal's one device, through ggml's backend registry.
    if (ggml_backend_reg_t registry = ggml_backend_metal_reg()) {
        for (size_t index = 0; index < ggml_backend_reg_dev_count(registry); ++index) {
            ggml_backend_dev_t device = ggml_backend_reg_dev_get(registry, index);
            size_t free = 0, total = 0;
            ggml_backend_dev_memory(device, &free, &total);
            const char *name = ggml_backend_dev_description(device);
            gpus.push_back({name ? name : "Metal", total, free});
        }
    }
#endif
    return gpus;
}

} // namespace kimodo
