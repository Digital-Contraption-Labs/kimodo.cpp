cmake_minimum_required(VERSION 3.25)
project(kimodo VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Static code, ggml's included, is linked into the shared library.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# Debug information in every object, so the shared library's PDB covers ggml
# as well as this code: a fault inside a host application is then readable.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
option(KIMODO_BUILD_TESTS "Build Kimodo tests" ON)
# The GPU backend: Vulkan everywhere but Apple, which has Metal.
if(APPLE)
  set(KIMODO_VULKAN_DEFAULT OFF)
else()
  set(KIMODO_VULKAN_DEFAULT ON)
endif()
option(KIMODO_ENABLE_VULKAN "Enable GGML Vulkan backend when GGML is available" ${KIMODO_VULKAN_DEFAULT})
option(KIMODO_ENABLE_METAL "Enable GGML Metal backend on Apple platforms" ON)
# An iOS app embeds frameworks, which carry a bundle identifier.
set(KIMODO_FRAMEWORK_IDENTIFIER "org.kimodo-cpp.kimodo" CACHE STRING "Bundle identifier of the iOS kimodo.framework")
option(KIMODO_ENABLE_FUZZERS "Build libFuzzer parser targets" OFF)
set(KIMODO_GGML_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ggml" CACHE PATH "Pinned GGML submodule source directory")
option(KIMODO_ENABLE_POSTPROCESS "Build upstream motion post-processing (x86, needs Eigen)" ON)
set(KIMODO_EIGEN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/eigen" CACHE PATH "Pinned Eigen submodule source directory")
set(KIMODO_VULKAN_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vulkan-headers/include" CACHE PATH "Pinned Vulkan-Headers submodule include directory, used on Linux and Android")
set(KIMODO_ROOT_PARITY_MODEL "${CMAKE_CURRENT_SOURCE_DIR}/models/kimodo-smplx-rp-v1-f32.gguf" CACHE FILEPATH "F32 motion GGUF required by parity tests")
set(KIMODO_ROOT_PARITY_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/smplx-zero-embedding" CACHE PATH "Fixture directory required by motion parity tests")
set(KIMODO_LLM_TEXT_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/generated/llm2vec-text-bundle" CACHE PATH "Native LLM2Vec GGUF component bundle required by text parity tests")
set(KIMODO_LLM_TEXT_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/llm2vec-real-prompt" CACHE PATH "Captured LLM2Vec fixture required by text parity tests")
set(KIMODO_MULTIPROMPT_FIXTURE "" CACHE PATH "Optional extracted upstream multi-prompt fixture for raw trajectory parity")
# The engine, static.  The tools and tests use its C++ API directly; the
# shared library below wraps it behind the C API, so nothing C++ crosses a
# module boundary (docs/SHARED_LIBRARY_PLAN.md).
add_library(kimodo-core STATIC src/model.cpp src/constraints.cpp src/constraints_json.cpp src/gguf.cpp
  src/diffusion.cpp src/motion_rep.cpp src/motion_decode.cpp src/log.cpp src/runtime_info.cpp)
target_include_directories(kimodo-core PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_include_directories(kimodo-core SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set_target_properties(kimodo-core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
# The version and source commit kimodo_get_capabilities reports; the commit
# as of this configure, which every scripts/build script runs.
set(KIMODO_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=12 HEAD
                  OUTPUT_VARIABLE KIMODO_GIT_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE KIMODO_GIT_RESULT)
  if(KIMODO_GIT_RESULT EQUAL 0)
    set(KIMODO_COMMIT "${KIMODO_GIT_HEAD}")
  endif()
endif()
set_property(SOURCE src/runtime_info.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "KIMODO_VERSION_STRING=\"${PROJECT_VERSION}\"" "KIMODO_COMMIT_STRING=\"${KIMODO_COMMIT}\"")
# kimodo.dll, libkimodo.so, libkimodo.dylib: one file holding the engine and
# ggml, exporting the C API and nothing else.
add_library(kimodo SHARED src/capi.cpp)
add_library(kimodo::kimodo ALIAS kimodo)
target_include_directories(kimodo PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_include_directories(kimodo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(kimodo PRIVATE kimodo-core)
target_compile_definitions(kimodo PRIVATE KIMODO_BUILD KIMODO_SHARED)
set_target_properties(kimodo PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
add_executable(kmd-inspect src/inspect.cpp)
target_link_libraries(kmd-inspect PRIVATE kimodo-core)
if(MSVC)
  target_compile_options(kimodo-core PRIVATE /W4 /permissive-)
  target_compile_options(kimodo PRIVATE /W4 /permissive-)
  # A PDB for every configuration, with the release optimisations kept.
  target_link_options(kimodo PRIVATE /DEBUG "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF>")
else()
  target_compile_options(kimodo-core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  target_compile_options(kimodo PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang's -Wconversion also covers signedness, which GCC's leaves out;
    # the same warnings on every compiler.
    target_compile_options(kimodo-core PRIVATE -Wno-sign-conversion)
    target_compile_options(kimodo PRIVATE -Wno-sign-conversion)
  endif()
endif()
if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android)$")
  # Only the C API leaves the .so: the symbols of the static archives it
  # links (kimodo-core, ggml, the C++ runtime) stay inside, and nothing may
  # be left for the loader to find elsewhere.
  target_link_options(kimodo PRIVATE -Wl,--exclude-libs,ALL -Wl,--no-undefined
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/kimodo.map")
  set_property(TARGET kimodo APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/kimodo.map")
  if(NOT ANDROID)
    # A host built with another libstdc++ loads it all the same; Android
    # takes c++_static from its toolchain instead (ANDROID_STL).
    target_link_options(kimodo PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()
if(APPLE)
  # Only the C API leaves the dylib: ggml's static archives would otherwise
  # export theirs.  The system's libc++ is the one runtime it links.
  target_link_options(kimodo PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/kimodo.exports")
  set_property(TARGET kimodo APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/kimodo.exports")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  # An iOS app embeds a framework, not a loose dylib, and signs it itself.
  # scripts/build/build_library_ios.sh joins the device and simulator builds
  # into kimodo.xcframework.
  set_target_properties(kimodo PROPERTIES
    FRAMEWORK TRUE
    MACOSX_FRAMEWORK_IDENTIFIER "${KIMODO_FRAMEWORK_IDENTIFIER}"
    MACOSX_FRAMEWORK_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_FRAMEWORK_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_FRAMEWORK_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ios/Info.plist.in"
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/kimodo/kimodo_capi.h")
endif()
# A C program that loads the shared library at run time, as a host
# application does, using kimodo_capi.h alone.
add_executable(kimodo-capi-smoke tests/capi_smoke.c)
target_include_directories(kimodo-capi-smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(kimodo-capi-smoke PRIVATE ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(kimodo-capi-smoke PRIVATE shell32) # UTF-8 arguments
elseif(NOT APPLE)
  target_link_libraries(kimodo-capi-smoke PRIVATE m)
endif()
add_dependencies(kimodo-capi-smoke kimodo)
if(EXISTS "${KIMODO_GGML_SOURCE_DIR}/CMakeLists.txt")
  # GGML is a pinned submodule: portable outside Nix, patchable, and kept
  # current independently of a distribution package.
  set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  set(GGML_VULKAN ${KIMODO_ENABLE_VULKAN} CACHE BOOL "" FORCE)
  set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
  # ggml is linked statically into the shared library and the tools, and
  # without OpenMP, whose runtime is not on every machine.
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
  if(APPLE)
    # Metal, its shader source embedded in the library and compiled by the
    # device at run time (no Metal toolchain at build time, and one build
    # for macOS and iOS).  No BLAS backend: the engine runs on Metal or on
    # ggml's own CPU backend, which uses Accelerate by itself.
    set(GGML_METAL ${KIMODO_ENABLE_METAL} CACHE BOOL "" FORCE)
    set(GGML_METAL_EMBED_LIBRARY ${KIMODO_ENABLE_METAL} CACHE BOOL "" FORCE)
    set(GGML_BLAS OFF CACHE BOOL "" FORCE)
  endif()
  # ggml's Vulkan backend needs newer headers than Debian 12 or the Android
  # NDK ship, so Linux and Android compile against the pinned vulkan-headers
  # and spirv-headers submodules (the version of the Windows Vulkan SDK, whose
  # own headers Windows uses).  Only headers: the program still loads the
  # system's Vulkan loader.  A cross build's sysroot has neither, and the
  # host's must not leak into it.
  set(KIMODO_PINNED_VULKAN_HEADERS FALSE)
  if(KIMODO_ENABLE_VULKAN AND NOT WIN32 AND NOT APPLE AND EXISTS "${KIMODO_VULKAN_HEADERS_DIR}/vulkan/vulkan.hpp"
     AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/spirv-headers/include/spirv/unified1/spirv.hpp")
    set(KIMODO_PINNED_VULKAN_HEADERS TRUE)
    set(Vulkan_INCLUDE_DIR "${KIMODO_VULKAN_HEADERS_DIR}" CACHE PATH "Vulkan headers" FORCE)
    set(SPIRV-Headers_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spirv-headers" CACHE PATH "SPIR-V headers' CMake package" FORCE)
  endif()
  add_subdirectory("${KIMODO_GGML_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/ggml" EXCLUDE_FROM_ALL)
  get_directory_property(KIMODO_GGML_VERSION DIRECTORY "${KIMODO_GGML_SOURCE_DIR}" DEFINITION GGML_VERSION)
  get_directory_property(KIMODO_GGML_COMMIT DIRECTORY "${KIMODO_GGML_SOURCE_DIR}" DEFINITION GGML_BUILD_COMMIT)
  set(KIMODO_HAVE_GGML TRUE)
  set(KIMODO_GPU_LIBS)
  set(KIMODO_GPU_DEFS)
  if(KIMODO_ENABLE_VULKAN AND TARGET ggml-vulkan)
    set(KIMODO_GPU_LIBS ggml-vulkan)
    set(KIMODO_GPU_DEFS KIMODO_HAVE_GGML_VULKAN=1)
    if(KIMODO_PINNED_VULKAN_HEADERS)
      # ggml includes spirv.hpp without linking the SPIR-V headers' target.
      target_include_directories(ggml-vulkan SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/spirv-headers/include")
    endif()
  endif()
  if(APPLE AND KIMODO_ENABLE_METAL AND TARGET ggml-metal)
    list(APPEND KIMODO_GPU_LIBS ggml-metal)
    list(APPEND KIMODO_GPU_DEFS KIMODO_HAVE_GGML_METAL=1)
  endif()
  target_link_libraries(kimodo-core PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_compile_definitions(kimodo-core PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
  target_link_libraries(kmd-inspect PRIVATE ggml)
  target_compile_definitions(kmd-inspect PRIVATE KIMODO_HAVE_GGML=1)
  target_sources(kimodo-core PRIVATE src/backend.cpp src/ggml_weights.cpp src/denoiser.cpp src/sequence.cpp)
  target_sources(kimodo-core PRIVATE src/llm_tokenizer.cpp src/llm_text_encoder.cpp)
  # NVIDIA's MotionCorrection (vendored, unmodified) behind post-processing.
  # Its math library is SSE code, built for AVX on x86 as upstream builds it
  # and through sse2neon on ARM64; its solvers are Eigen's.  Every file that
  # includes Eigen lives in this one target: Eigen's allocation alignment
  # follows the instruction set, so mixing flags across translation units
  # corrupts the heap.
  set(KIMODO_POSTPROCESS_SIMD "")
  if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
     (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$"))
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sse2neon/sse2neon.h")
      set(KIMODO_POSTPROCESS_SIMD neon)
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64|i[3-6]86|x86)$")
    set(KIMODO_POSTPROCESS_SIMD sse)
  endif()
  if(KIMODO_ENABLE_POSTPROCESS AND EXISTS "${KIMODO_EIGEN_SOURCE_DIR}/Eigen/Sparse" AND KIMODO_POSTPROCESS_SIMD)
    set(KIMODO_HAVE_POSTPROCESS TRUE)
    file(GLOB KIMODO_MOTION_CORRECTION_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/motion_correction/Math/*.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/motion_correction/AnimProcessing/*.cpp")
    add_library(kimodo-postprocess STATIC ${KIMODO_MOTION_CORRECTION_SOURCES} src/postprocess.cpp src/motion_encode.cpp)
    target_include_directories(kimodo-postprocess PRIVATE src include)
    target_include_directories(kimodo-postprocess SYSTEM PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/motion_correction" "${KIMODO_EIGEN_SOURCE_DIR}")
    target_compile_definitions(kimodo-postprocess PRIVATE EIGEN_MPL2_ONLY)
    set_target_properties(kimodo-postprocess PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      # MotionCorrection's Compiler.h defines FORCE_INLINE for MSVC and GCC
      # only; Clang (the Android NDK, Apple) takes GCC's.
      target_compile_definitions(kimodo-postprocess PRIVATE "FORCE_INLINE=inline __attribute__((always_inline))")
    endif()
    if(KIMODO_POSTPROCESS_SIMD STREQUAL "neon")
      # MotionCorrection's <immintrin.h> is sse2neon's (see its README).
      target_include_directories(kimodo-postprocess BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sse2neon/include")
    elseif(MSVC)
      target_compile_options(kimodo-postprocess PRIVATE /arch:AVX)
    else()
      target_compile_options(kimodo-postprocess PRIVATE -msse4.1 -mavx)
    endif()
    target_link_libraries(kimodo-core PRIVATE kimodo-postprocess)
    target_compile_definitions(kimodo-core PRIVATE KIMODO_HAVE_POSTPROCESS=1)
  else()
    message(STATUS "kimodo: post-processing disabled (needs x86 or ARM64, and ${KIMODO_EIGEN_SOURCE_DIR})")
  endif()
  # Tools and fixture tests that compile engine sources themselves rather
  # than linking kimodo-core: the runtime pieces those sources share.
  function(kimodo_engine_sources target)
    target_sources(${target} PRIVATE src/backend.cpp src/log.cpp)
    target_include_directories(${target} PRIVATE src include)
  endfunction()
  add_executable(kmd-sample-fixture src/sample_fixture.cpp)
  kimodo_engine_sources(kmd-sample-fixture)
  target_sources(kmd-sample-fixture PRIVATE
    src/gguf.cpp src/ggml_weights.cpp src/denoiser.cpp src/diffusion.cpp
    src/motion_rep.cpp src/motion_decode.cpp)
  target_include_directories(kmd-sample-fixture PRIVATE src)
  target_link_libraries(kmd-sample-fixture PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_compile_definitions(kmd-sample-fixture PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
  add_executable(kmd-sample-embedding src/sample_embedding.cpp)
  kimodo_engine_sources(kmd-sample-embedding)
  target_sources(kmd-sample-embedding PRIVATE
    src/gguf.cpp src/ggml_weights.cpp src/denoiser.cpp src/diffusion.cpp
    src/motion_rep.cpp src/motion_decode.cpp)
  target_include_directories(kmd-sample-embedding PRIVATE src)
  target_link_libraries(kmd-sample-embedding PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_compile_definitions(kmd-sample-embedding PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
  add_executable(kmd-generate src/generate.cpp)
  target_link_libraries(kmd-generate PRIVATE kimodo-core)
  add_executable(kmd-encode src/encode.cpp)
  kimodo_engine_sources(kmd-encode)
  target_sources(kmd-encode PRIVATE src/llm_text_encoder.cpp src/llm_tokenizer.cpp)
  target_include_directories(kmd-encode PRIVATE src)
  target_link_libraries(kmd-encode PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_compile_definitions(kmd-encode PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
  add_executable(kmd-quantize-text src/quantize_text.cpp)
  target_link_libraries(kmd-quantize-text PRIVATE ggml)
  add_executable(kmd-pack-text src/pack_text.cpp)
  target_link_libraries(kmd-pack-text PRIVATE ggml)
  add_executable(kimodo-llm-layer-parity tests/llm_layer_parity.cpp)
  target_link_libraries(kimodo-llm-layer-parity PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_include_directories(kimodo-llm-layer-parity PRIVATE src)
  target_compile_definitions(kimodo-llm-layer-parity PRIVATE ${KIMODO_GPU_DEFS})
  add_executable(kimodo-llm-final-norm-parity tests/llm_final_norm_parity.cpp)
  target_link_libraries(kimodo-llm-final-norm-parity PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_compile_definitions(kimodo-llm-final-norm-parity PRIVATE ${KIMODO_GPU_DEFS})
  add_executable(kimodo-llm-tokenizer-test tests/llm_tokenizer_test.cpp src/llm_tokenizer.cpp)
  target_include_directories(kimodo-llm-tokenizer-test PRIVATE src)
  target_link_libraries(kimodo-llm-tokenizer-test PRIVATE ggml)
  add_executable(kimodo-llm-embedding-parity tests/llm_embedding_parity.cpp)
  target_link_libraries(kimodo-llm-embedding-parity PRIVATE ggml ${KIMODO_GPU_LIBS})
  add_executable(kimodo-multiprompt-fixture-parity tests/multiprompt_fixture_parity.cpp)
  kimodo_engine_sources(kimodo-multiprompt-fixture-parity)
  target_sources(kimodo-multiprompt-fixture-parity PRIVATE src/gguf.cpp src/ggml_weights.cpp src/denoiser.cpp src/sequence.cpp src/constraints.cpp src/motion_rep.cpp src/diffusion.cpp)
  target_include_directories(kimodo-multiprompt-fixture-parity PRIVATE src include)
  target_link_libraries(kimodo-multiprompt-fixture-parity PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_compile_definitions(kimodo-multiprompt-fixture-parity PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
  add_executable(kimodo-constraints-test tests/constraints_test.cpp)
  kimodo_engine_sources(kimodo-constraints-test)
  target_sources(kimodo-constraints-test PRIVATE src/gguf.cpp src/ggml_weights.cpp src/denoiser.cpp src/sequence.cpp src/constraints.cpp src/motion_rep.cpp src/diffusion.cpp)
  target_include_directories(kimodo-constraints-test PRIVATE src include)
  target_link_libraries(kimodo-constraints-test PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_compile_definitions(kimodo-constraints-test PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
  add_executable(kimodo-stage-fixture-parity tests/stage_fixture_parity.cpp)
  kimodo_engine_sources(kimodo-stage-fixture-parity)
  target_sources(kimodo-stage-fixture-parity PRIVATE src/gguf.cpp src/ggml_weights.cpp src/denoiser.cpp src/motion_rep.cpp src/diffusion.cpp)
  target_include_directories(kimodo-stage-fixture-parity PRIVATE src)
  target_link_libraries(kimodo-stage-fixture-parity PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_compile_definitions(kimodo-stage-fixture-parity PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
  add_executable(kimodo-llm-text-session-parity
    tests/llm_text_session_parity.cpp src/llm_text_encoder.cpp src/llm_tokenizer.cpp)
  kimodo_engine_sources(kimodo-llm-text-session-parity)
  target_include_directories(kimodo-llm-text-session-parity PRIVATE src)
  target_link_libraries(kimodo-llm-text-session-parity PRIVATE ggml ${KIMODO_GPU_LIBS})
  target_compile_definitions(kimodo-llm-text-session-parity PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
endif()
include(CTest)
if(BUILD_TESTING AND KIMODO_BUILD_TESTS)
  add_executable(kimodo-capi-test tests/capi_test.cpp)
  target_link_libraries(kimodo-capi-test PRIVATE kimodo)
  add_test(NAME kimodo-capi-test COMMAND kimodo-capi-test "${KIMODO_ROOT_PARITY_MODEL}" "${KIMODO_LLM_TEXT_BUNDLE}")
  add_test(NAME kimodo-capi-smoke COMMAND kimodo-capi-smoke --library "$<TARGET_FILE:kimodo>")
  # NVIDIA's constraints JSON, against the demo server's cases.
  add_executable(kimodo-constraints-json-test tests/constraints_json_test.cpp src/constraints.cpp src/constraints_json.cpp)
  target_include_directories(kimodo-constraints-json-test PRIVATE src include)
  target_include_directories(kimodo-constraints-json-test SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  add_test(NAME kimodo-constraints-json-test COMMAND kimodo-constraints-json-test)
  add_executable(kimodo-diffusion-test tests/diffusion_test.cpp)
  target_sources(kimodo-diffusion-test PRIVATE src/diffusion.cpp)
  target_include_directories(kimodo-diffusion-test PRIVATE src)
  target_link_libraries(kimodo-diffusion-test PRIVATE kimodo)
  add_test(NAME kimodo-diffusion-test COMMAND kimodo-diffusion-test)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME kimodo-converter-test
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/converter_test.py")
    add_test(NAME kimodo-quantization-metrics-test
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/quantization_metrics_test.py")
  endif()
  if(KIMODO_HAVE_GGML)
    add_test(NAME kimodo-constraints-test COMMAND kimodo-constraints-test)
    if(KIMODO_HAVE_POSTPROCESS)
      add_executable(kimodo-postprocess-test tests/postprocess_test.cpp src/constraints.cpp)
      target_include_directories(kimodo-postprocess-test PRIVATE src include)
      target_link_libraries(kimodo-postprocess-test PRIVATE kimodo-postprocess)
      add_test(NAME kimodo-postprocess-test COMMAND kimodo-postprocess-test)
    endif()
    add_test(NAME kimodo-llm-text-session-cpu
      COMMAND "${CMAKE_COMMAND}" -E env KIMODO_BACKEND=cpu "$<TARGET_FILE:kimodo-llm-text-session-parity>" "${KIMODO_LLM_TEXT_BUNDLE}" "${KIMODO_LLM_TEXT_FIXTURE}")
    if(KIMODO_ENABLE_VULKAN AND TARGET ggml-vulkan)
      add_test(NAME kimodo-llm-text-session-vulkan
        COMMAND kimodo-llm-text-session-parity "${KIMODO_LLM_TEXT_BUNDLE}" "${KIMODO_LLM_TEXT_FIXTURE}")
    endif()
    add_executable(kimodo-root-parity tests/root_parity.cpp src/motion_rep.cpp)
    target_include_directories(kimodo-root-parity PRIVATE src)
    target_link_libraries(kimodo-root-parity PRIVATE kimodo ggml ${KIMODO_GPU_LIBS})
    add_test(NAME kimodo-root-parity
        COMMAND kimodo-root-parity "${KIMODO_ROOT_PARITY_MODEL}" "${KIMODO_ROOT_PARITY_FIXTURE}" root)
      add_test(NAME kimodo-body-parity
        COMMAND kimodo-root-parity "${KIMODO_ROOT_PARITY_MODEL}" "${KIMODO_ROOT_PARITY_FIXTURE}" body)
      add_executable(kimodo-fixture-sampler-parity tests/fixture_sampler_parity.cpp)
      target_include_directories(kimodo-fixture-sampler-parity PRIVATE src)
      target_link_libraries(kimodo-fixture-sampler-parity PRIVATE kimodo)
      add_test(NAME kimodo-fixture-sampler-parity
        COMMAND kimodo-fixture-sampler-parity "${KIMODO_ROOT_PARITY_FIXTURE}")
      add_executable(kimodo-ggml-weights-test tests/ggml_weights_test.cpp)
      kimodo_engine_sources(kimodo-ggml-weights-test)
      target_sources(kimodo-ggml-weights-test PRIVATE src/gguf.cpp src/ggml_weights.cpp)
      target_include_directories(kimodo-ggml-weights-test PRIVATE src)
      target_link_libraries(kimodo-ggml-weights-test PRIVATE kimodo ggml ${KIMODO_GPU_LIBS})
      target_compile_definitions(kimodo-ggml-weights-test PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
      add_test(NAME kimodo-ggml-weights-test
        COMMAND kimodo-ggml-weights-test "${KIMODO_ROOT_PARITY_MODEL}")
      add_executable(kimodo-denoiser-runtime-test tests/denoiser_runtime_test.cpp)
      kimodo_engine_sources(kimodo-denoiser-runtime-test)
      target_sources(kimodo-denoiser-runtime-test PRIVATE src/gguf.cpp src/ggml_weights.cpp src/denoiser.cpp src/motion_rep.cpp src/diffusion.cpp)
      target_include_directories(kimodo-denoiser-runtime-test PRIVATE src)
      target_link_libraries(kimodo-denoiser-runtime-test PRIVATE kimodo ggml ${KIMODO_GPU_LIBS})
      target_compile_definitions(kimodo-denoiser-runtime-test PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
      add_test(NAME kimodo-denoiser-runtime-root
        COMMAND kimodo-denoiser-runtime-test "${KIMODO_ROOT_PARITY_MODEL}" "${KIMODO_ROOT_PARITY_FIXTURE}" root)
      add_test(NAME kimodo-denoiser-runtime-body
        COMMAND kimodo-denoiser-runtime-test "${KIMODO_ROOT_PARITY_MODEL}" "${KIMODO_ROOT_PARITY_FIXTURE}" body)
      add_test(NAME kimodo-denoiser-runtime-full
        COMMAND kimodo-denoiser-runtime-test "${KIMODO_ROOT_PARITY_MODEL}" "${KIMODO_ROOT_PARITY_FIXTURE}" full)
      add_executable(kimodo-decode-test tests/decode_test.cpp)
      kimodo_engine_sources(kimodo-decode-test)
      target_sources(kimodo-decode-test PRIVATE src/gguf.cpp src/ggml_weights.cpp src/motion_decode.cpp)
      target_include_directories(kimodo-decode-test PRIVATE src)
      target_link_libraries(kimodo-decode-test PRIVATE kimodo ggml ${KIMODO_GPU_LIBS})
      target_compile_definitions(kimodo-decode-test PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
      add_test(NAME kimodo-decode-test COMMAND kimodo-decode-test "${KIMODO_ROOT_PARITY_MODEL}" "${KIMODO_ROOT_PARITY_FIXTURE}")
      add_executable(kimodo-generate-smoke tests/generate_smoke.cpp)
      target_link_libraries(kimodo-generate-smoke PRIVATE kimodo-core)
      add_test(NAME kimodo-generate-smoke COMMAND kimodo-generate-smoke "${KIMODO_ROOT_PARITY_MODEL}")
      add_executable(kimodo-sampler2-test tests/sampler2_test.cpp)
      kimodo_engine_sources(kimodo-sampler2-test)
      target_sources(kimodo-sampler2-test PRIVATE src/gguf.cpp src/ggml_weights.cpp src/denoiser.cpp src/motion_rep.cpp src/diffusion.cpp)
      target_include_directories(kimodo-sampler2-test PRIVATE src)
      target_link_libraries(kimodo-sampler2-test PRIVATE kimodo ggml ${KIMODO_GPU_LIBS})
      target_compile_definitions(kimodo-sampler2-test PRIVATE KIMODO_HAVE_GGML=1 ${KIMODO_GPU_DEFS})
      if(IS_DIRECTORY "${KIMODO_MULTIPROMPT_FIXTURE}")
        add_test(NAME kimodo-multiprompt-fixture-parity
          COMMAND kimodo-multiprompt-fixture-parity "${KIMODO_ROOT_PARITY_MODEL}" "${KIMODO_MULTIPROMPT_FIXTURE}")
      endif()
  endif()
endif()
# What this build holds, for the package's VERSION.json
# (scripts/build/package.cmake includes this file).
set(KIMODO_BACKENDS cpu)
if(KIMODO_ENABLE_VULKAN AND TARGET ggml-vulkan)
  list(APPEND KIMODO_BACKENDS vulkan)
endif()
if(APPLE AND KIMODO_ENABLE_METAL AND TARGET ggml-metal)
  list(APPEND KIMODO_BACKENDS metal)
endif()
if(NOT KIMODO_HAVE_POSTPROCESS)
  set(KIMODO_HAVE_POSTPROCESS FALSE)
endif()
# The instruction sets ggml's CPU code was built for: the CPU the library
# needs.  Post-processing is built for AVX besides (see kimodo-postprocess).
set(KIMODO_CPU_FEATURES)
foreach(feature SSE42 AVX AVX2 FMA F16C BMI2 AVX_VNNI AVX512)
  if(GGML_${feature})
    list(APPEND KIMODO_CPU_FEATURES ${feature})
  endif()
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/kimodo-build.cmake" CONTENT [[
set(KIMODO_BUILD_VERSION "@PROJECT_VERSION@")
set(KIMODO_BUILD_TYPE "@CMAKE_BUILD_TYPE@")
set(KIMODO_BUILD_SYSTEM "@CMAKE_SYSTEM_NAME@")
set(KIMODO_BUILD_PROCESSOR "@CMAKE_SYSTEM_PROCESSOR@")
set(KIMODO_BUILD_COMPILER "@CMAKE_CXX_COMPILER_ID@ @CMAKE_CXX_COMPILER_VERSION@")
set(KIMODO_BUILD_GGML_VERSION "@KIMODO_GGML_VERSION@")
set(KIMODO_BUILD_GGML_COMMIT "@KIMODO_GGML_COMMIT@")
set(KIMODO_BUILD_BACKENDS "@KIMODO_BACKENDS@")
set(KIMODO_BUILD_POSTPROCESS "@KIMODO_HAVE_POSTPROCESS@")
set(KIMODO_BUILD_POSTPROCESS_SIMD "@KIMODO_POSTPROCESS_SIMD@")
set(KIMODO_BUILD_CPU_FEATURES "@KIMODO_CPU_FEATURES@")
set(KIMODO_BUILD_MINIMUM_OS "@CMAKE_OSX_DEPLOYMENT_TARGET@")
]] @ONLY)
if(KIMODO_ENABLE_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "KIMODO_ENABLE_FUZZERS requires Clang/libFuzzer")
  endif()
  add_executable(kimodo-gguf-fuzz fuzz/gguf_fuzz.cpp src/gguf.cpp)
  target_include_directories(kimodo-gguf-fuzz PRIVATE include src)
  target_compile_options(kimodo-gguf-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(kimodo-gguf-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  add_executable(kimodo-constraints-json-fuzz fuzz/constraints_json_fuzz.cpp src/constraints.cpp src/constraints_json.cpp)
  target_include_directories(kimodo-constraints-json-fuzz PRIVATE include src)
  target_include_directories(kimodo-constraints-json-fuzz SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
  target_compile_options(kimodo-constraints-json-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(kimodo-constraints-json-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
