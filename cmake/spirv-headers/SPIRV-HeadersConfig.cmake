# SPIR-V headers for ggml's Vulkan backend, from the pinned spirv-headers
# submodule: what ggml's find_package(SPIRV-Headers CONFIG) finds on Linux
# and Android, where the system has none, or an older one, or (cross
# building) none inside the sysroot.  CMakeLists.txt points SPIRV-Headers_DIR
# here and puts the include folder on ggml's Vulkan target, which uses the
# headers without linking this target.
if(NOT TARGET SPIRV-Headers::SPIRV-Headers)
  add_library(SPIRV-Headers::SPIRV-Headers INTERFACE IMPORTED)
  set_target_properties(SPIRV-Headers::SPIRV-Headers PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../../spirv-headers/include")
endif()
set(SPIRV-Headers_FOUND TRUE)
