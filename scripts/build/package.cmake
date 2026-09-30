# Package a built Kimodo shared library for a host application
# (docs/SHARED_LIBRARY_PLAN.md, section 8).  The platform build scripts in
# this folder run it after their build:
#
#   cmake -DKIMODO_TARGET=windows|linux|android|macos|ios -DKIMODO_SOURCE_DIR=<repository>
#         -DKIMODO_BUILD_DIR=<build folder> -DKIMODO_PACKAGE_DIR=<package folder>
#         [-DKIMODO_WEIGHTS=OFF] [-DKIMODO_TEXT_WEIGHTS=OFF] [-DKIMODO_WEIGHTS_DIR=<weights cache>]
#         [-DKIMODO_ANDROID_ABI=arm64-v8a] [-DKIMODO_NM=<nm>] [-DKIMODO_READELF=<readelf>]
#         [-DKIMODO_STRIP=<strip>] [-DKIMODO_XCFRAMEWORK=<kimodo.xcframework>]
#         -P scripts/build/package.cmake
#
# The package:
#
#   include/kimodo/kimodo_capi.h
#   bin/kimodo.dll, bin/kimodo.pdb, lib/kimodo.lib     (windows)
#   lib/libkimodo.so                                   (linux)
#   lib/<abi>/libkimodo.so, symbols/<abi>/libkimodo.so (android: stripped as
#                           jniLibs lays it out, and with its symbols for
#                           reading crash reports)
#   lib/libkimodo.dylib                                (macos)
#   lib/kimodo.xcframework  device and simulator frameworks (ios)
#   tools/kimodo-capi-smoke    loads the library and generates a clip, to check
#                              an install (on Android, run it through adb)
#   LICENSE  NOTICE  THIRD_PARTY.md  licenses/
#   VERSION.json    ABI, version, commit, ggml, backends, post-processing
#   weights/        the weights in the cache, and WEIGHTS.md describing them;
#                   KIMODO_TEXT_WEIGHTS=OFF leaves the text encoder out, as the
#                   Android packages do: no phone holds it beside an app
#
# The weights cache is the repository root by default: motion models in
# models/, the text encoder and tokenizer.gguf beside it, where
# scripts/download_gguf_weights.py puts them.  An SMPL-X checkpoint is never
# packaged: its licence forbids redistributing it.  Weights are copied only
# when they changed, and their SHA-256 is remembered in weights/.sha256-cache
# for the same reason: 11 GB is slow to copy and to hash.
#
# The library is checked before it is packaged: it must export kimodo_* and
# nothing else, and import only the system's libraries, its C/C++ runtime and
# the GPU's (dumpbin on Windows, nm and readelf on ELF targets, nm and otool
# on Apple's).  A failed check stops the packaging.

cmake_minimum_required(VERSION 3.25)

foreach(required KIMODO_TARGET KIMODO_SOURCE_DIR KIMODO_BUILD_DIR KIMODO_PACKAGE_DIR)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "package.cmake needs -D${required}=...")
  endif()
endforeach()
if(NOT DEFINED KIMODO_WEIGHTS)
  set(KIMODO_WEIGHTS ON)
endif()
if(NOT DEFINED KIMODO_TEXT_WEIGHTS)
  set(KIMODO_TEXT_WEIGHTS ON)
endif()
if(NOT DEFINED KIMODO_ANDROID_ABI OR KIMODO_ANDROID_ABI STREQUAL "")
  set(KIMODO_ANDROID_ABI arm64-v8a)
endif()
if(NOT DEFINED KIMODO_WEIGHTS_DIR OR KIMODO_WEIGHTS_DIR STREQUAL "")
  set(KIMODO_WEIGHTS_DIR "${KIMODO_SOURCE_DIR}")
endif()
foreach(dir KIMODO_SOURCE_DIR KIMODO_BUILD_DIR KIMODO_PACKAGE_DIR KIMODO_WEIGHTS_DIR)
  file(TO_CMAKE_PATH "${${dir}}" ${dir})
endforeach()
set(src "${KIMODO_SOURCE_DIR}")
set(build "${KIMODO_BUILD_DIR}")
set(out "${KIMODO_PACKAGE_DIR}")

set(build_info "${build}/kimodo-build.cmake")
if(NOT EXISTS "${build_info}")
  message(FATAL_ERROR "${build_info} not found: configure and build ${build} first")
endif()
include("${build_info}")

# A file's size in bytes.  file(SIZE) keeps it in an unsigned long, which is
# 32 bits on Windows, so there a file over 4 GB reads short by 4 GB.
function(file_size path out_var)
  if(CMAKE_HOST_WIN32)
    file(TO_NATIVE_PATH "${path}" native)
    string(REPLACE "'" "''" native "${native}")
    execute_process(COMMAND powershell -NoProfile -NonInteractive -Command "(Get-Item -LiteralPath '${native}').Length"
                    OUTPUT_VARIABLE size OUTPUT_STRIP_TRAILING_WHITESPACE)
  else()
    file(SIZE "${path}" size)
  endif()
  if(NOT size MATCHES "^[0-9]+$")
    message(FATAL_ERROR "Could not read the size of ${path}")
  endif()
  set(${out_var} "${size}" PARENT_SCOPE)
endfunction()

# Bytes as decimal GB, or MB below a gigabyte, with two places, as the
# model cards give them.
function(human_size bytes out_var)
  set(unit GB)
  set(scale 10000000)
  if(bytes LESS 1000000000)
    set(unit MB)
    set(scale 10000)
  endif()
  math(EXPR hundredths "${bytes} / ${scale}")
  math(EXPR whole "${hundredths} / 100")
  math(EXPR fraction "${hundredths} % 100")
  if(fraction LESS 10)
    set(fraction "0${fraction}")
  endif()
  set(${out_var} "${whole}.${fraction} ${unit}" PARENT_SCOPE)
endfunction()

# A JSON array of strings from a CMake list.
function(json_array out_var)
  set(items)
  foreach(item IN LISTS ARGN)
    string(REPLACE "\\" "\\\\" item "${item}")
    string(REPLACE "\"" "\\\"" item "${item}")
    list(APPEND items "\"${item}\"")
  endforeach()
  list(JOIN items ", " joined)
  set(${out_var} "[${joined}]" PARENT_SCOPE)
endfunction()

# ---- The library ---------------------------------------------------------
set(elf FALSE)
set(macho FALSE)
set(strip_library "")
if(KIMODO_TARGET STREQUAL "windows")
  set(binaries "bin/kimodo.dll" "bin/kimodo.pdb" "lib/kimodo.lib" "tools/kimodo-capi-smoke.exe")
  set(sources "${build}/kimodo.dll" "${build}/kimodo.pdb" "${build}/kimodo.lib" "${build}/kimodo-capi-smoke.exe")
elseif(KIMODO_TARGET STREQUAL "linux")
  set(elf TRUE)
  set(binaries "lib/libkimodo.so" "tools/kimodo-capi-smoke")
  set(sources "${build}/libkimodo.so" "${build}/kimodo-capi-smoke")
  # glibc, the Vulkan loader and the dynamic linker; the C++ runtime is
  # linked in statically.
  set(allowed_imports "^(libc\\.so\\.6|libm\\.so\\.6|libdl\\.so\\.2|libpthread\\.so\\.0|librt\\.so\\.1|libvulkan\\.so\\.1|ld-linux-[a-z0-9_-]+\\.so\\.[0-9])$")
elseif(KIMODO_TARGET STREQUAL "android")
  set(elf TRUE)
  set(abi "${KIMODO_ANDROID_ABI}")
  set(binaries "symbols/${abi}/libkimodo.so" "tools/${abi}/kimodo-capi-smoke")
  set(sources "${build}/libkimodo.so" "${build}/kimodo-capi-smoke")
  # lib/<abi>/libkimodo.so is the stripped copy of symbols/<abi>/libkimodo.so.
  set(strip_library "lib/${abi}/libkimodo.so")
  # Bionic and the Vulkan loader; the C++ runtime is c++_static.
  set(allowed_imports "^(libc|libm|libdl|liblog|libvulkan|libandroid)\\.so$")
elseif(KIMODO_TARGET STREQUAL "macos")
  set(macho TRUE)
  set(binaries "lib/libkimodo.dylib" "tools/kimodo-capi-smoke")
  set(sources "${build}/libkimodo.dylib" "${build}/kimodo-capi-smoke")
  list(GET sources 0 macho_binary)
  set(own_install_name "@rpath/libkimodo.dylib")
elseif(KIMODO_TARGET STREQUAL "ios")
  # The XCFramework build_library_ios.sh made from the device and simulator
  # frameworks; its device slice is what gets checked.
  if(NOT KIMODO_XCFRAMEWORK)
    message(FATAL_ERROR "package.cmake needs -DKIMODO_XCFRAMEWORK=<kimodo.xcframework> for ios")
  endif()
  file(TO_CMAKE_PATH "${KIMODO_XCFRAMEWORK}" KIMODO_XCFRAMEWORK)
  set(macho TRUE)
  set(binaries "lib/kimodo.xcframework")
  set(sources "${KIMODO_XCFRAMEWORK}")
  set(macho_binary "${KIMODO_XCFRAMEWORK}/ios-arm64/kimodo.framework/kimodo")
  set(own_install_name "@rpath/kimodo.framework/kimodo")
else()
  message(FATAL_ERROR "package.cmake does not package '${KIMODO_TARGET}' yet")
endif()
foreach(file IN LISTS sources)
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "${file} not found: build the library first")
  endif()
endforeach()

set(imports)
if(KIMODO_TARGET STREQUAL "windows")
  find_program(DUMPBIN dumpbin)
  if(NOT DUMPBIN)
    message(FATAL_ERROR "dumpbin not found: run this from the Visual Studio x64 toolchain, "
                        "as scripts/build/build_library_windows.bat does")
  endif()
  execute_process(COMMAND "${DUMPBIN}" /nologo /exports "${build}/kimodo.dll"
                  OUTPUT_VARIABLE dump RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "dumpbin /exports failed:\n${dump}")
  endif()
  string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-F]+ +[0-9A-F]+ +[^ \r\n]+" rows "${dump}")
  set(exports)
  set(foreign)
  foreach(row IN LISTS rows)
    string(REGEX REPLACE ".* ([^ ]+)$" "\\1" name "${row}")
    list(APPEND exports "${name}")
    if(NOT name MATCHES "^kimodo_")
      list(APPEND foreign "${name}")
    endif()
  endforeach()
  list(LENGTH exports export_count)
  if(export_count EQUAL 0)
    message(FATAL_ERROR "kimodo.dll exports nothing:\n${dump}")
  endif()
  if(foreign)
    list(JOIN foreign ", " foreign)
    message(FATAL_ERROR "kimodo.dll exports symbols outside the C API: ${foreign}")
  endif()
  execute_process(COMMAND "${DUMPBIN}" /nologo /dependents "${build}/kimodo.dll"
                  OUTPUT_VARIABLE dump RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "dumpbin /dependents failed:\n${dump}")
  endif()
  string(REGEX MATCHALL "\n +[^ \r\n]+\\.[dD][lL][lL]" rows "${dump}")
  set(unexpected)
  foreach(row IN LISTS rows)
    string(STRIP "${row}" name)
    list(APPEND imports "${name}")
    string(TOLOWER "${name}" lower)
    # Windows itself, the Visual C++ runtime (/MD; the D forms are the debug
    # runtime), and the Vulkan loader that comes with every GPU driver.
    if(NOT lower MATCHES "^(kernel32|advapi32|user32|shell32|ole32|oleaut32|vulkan-1|ucrtbased?|msvcp140(_1|_2|_atomic_wait|_codecvt_ids)?d?|vcruntime140(_1)?d?|api-ms-win-.+)\\.dll$")
      list(APPEND unexpected "${name}")
    endif()
  endforeach()
  if(unexpected)
    list(JOIN unexpected ", " unexpected)
    message(FATAL_ERROR "kimodo.dll depends on DLLs a host would have to ship as well: ${unexpected}")
  endif()
  message(STATUS "kimodo.dll exports ${export_count} kimodo_* functions and nothing else")
  list(JOIN imports ", " import_text)
  message(STATUS "kimodo.dll imports ${import_text}")
endif()

set(glibc "")
if(elf)
  list(GET sources 0 library)
  get_filename_component(library_name "${library}" NAME)
  # The binutils tools, or LLVM's under a cross toolchain (-DKIMODO_NM=...).
  if(NOT KIMODO_NM)
    find_program(KIMODO_NM NAMES nm llvm-nm)
  endif()
  if(NOT KIMODO_READELF)
    find_program(KIMODO_READELF NAMES readelf llvm-readelf)
  endif()
  if(NOT KIMODO_NM OR NOT KIMODO_READELF)
    message(FATAL_ERROR "nm and readelf are needed to check ${library_name}: install binutils")
  endif()
  execute_process(COMMAND "${KIMODO_NM}" -D --defined-only "${library}" OUTPUT_VARIABLE dump RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "nm -D failed on ${library}")
  endif()
  string(REGEX MATCHALL "[0-9a-fA-F]+ [A-Za-z] [^ \r\n]+" rows "${dump}")
  set(exports)
  set(foreign)
  foreach(row IN LISTS rows)
    string(REGEX REPLACE "^[0-9a-fA-F]+ [A-Za-z] ([^@]+).*$" "\\1" name "${row}")
    list(APPEND exports "${name}")
    if(NOT name MATCHES "^kimodo_")
      list(APPEND foreign "${name}")
    endif()
  endforeach()
  list(LENGTH exports export_count)
  if(export_count EQUAL 0)
    message(FATAL_ERROR "${library_name} exports nothing")
  endif()
  if(foreign)
    list(JOIN foreign ", " foreign)
    message(FATAL_ERROR "${library_name} exports symbols outside the C API: ${foreign}")
  endif()
  execute_process(COMMAND "${KIMODO_READELF}" -d "${library}" OUTPUT_VARIABLE dump RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "readelf -d failed on ${library}")
  endif()
  string(REGEX MATCHALL "\\(NEEDED\\)[^[\n]*\\[[^]\n]+\\]" rows "${dump}")
  set(unexpected)
  foreach(row IN LISTS rows)
    string(REGEX REPLACE ".*\\[([^]]+)\\]$" "\\1" name "${row}")
    list(APPEND imports "${name}")
    if(NOT name MATCHES "${allowed_imports}")
      list(APPEND unexpected "${name}")
    endif()
  endforeach()
  if(unexpected)
    list(JOIN unexpected ", " unexpected)
    message(FATAL_ERROR "${library_name} depends on libraries a host would have to ship as well: ${unexpected}")
  endif()
  if(KIMODO_TARGET STREQUAL "android")
    # Android 15 devices with 16 KB pages refuse a library whose segments are
    # aligned to less (ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES).
    execute_process(COMMAND "${KIMODO_READELF}" -lW "${library}" OUTPUT_VARIABLE dump)
    string(REGEX MATCHALL "\n *LOAD[^\n]*" rows "${dump}")
    if(NOT rows)
      message(FATAL_ERROR "readelf -l found no LOAD segments in ${library_name}")
    endif()
    foreach(row IN LISTS rows)
      string(REGEX REPLACE ".* 0x([0-9a-fA-F]+)[ \r]*$" "\\1" align "${row}")
      math(EXPR align "0x${align}")
      if(align LESS 16384)
        message(FATAL_ERROR "${library_name} has a segment aligned to ${align} bytes; 16 KB pages need 16384")
      endif()
    endforeach()
    message(STATUS "${library_name} is aligned for 16 KB pages")
  endif()
  # The newest glibc symbol version it needs: the oldest glibc it runs on.
  execute_process(COMMAND "${KIMODO_READELF}" -V "${library}" OUTPUT_VARIABLE dump)
  string(REGEX MATCHALL "GLIBC_[0-9]+\\.[0-9]+" versions "${dump}")
  foreach(version IN LISTS versions)
    string(REPLACE "GLIBC_" "" version "${version}")
    if(glibc STREQUAL "" OR version VERSION_GREATER glibc)
      set(glibc "${version}")
    endif()
  endforeach()
  message(STATUS "${library_name} exports ${export_count} kimodo_* functions and nothing else")
  list(JOIN imports ", " import_text)
  message(STATUS "${library_name} needs ${import_text}")
  if(glibc)
    message(STATUS "${library_name} needs glibc ${glibc} or later")
  endif()
endif()

if(macho)
  get_filename_component(library_name "${macho_binary}" NAME)
  if(NOT EXISTS "${macho_binary}")
    message(FATAL_ERROR "${macho_binary} not found")
  endif()
  find_program(KIMODO_NM nm)
  find_program(KIMODO_OTOOL otool)
  if(NOT KIMODO_NM OR NOT KIMODO_OTOOL)
    message(FATAL_ERROR "nm and otool are needed to check ${library_name}: install Xcode's command line tools (xcode-select --install)")
  endif()
  execute_process(COMMAND "${KIMODO_NM}" -gU "${macho_binary}" OUTPUT_VARIABLE dump RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "nm -gU failed on ${macho_binary}")
  endif()
  string(REGEX MATCHALL "[0-9a-fA-F]+ [A-Za-z] [^ \r\n]+" rows "${dump}")
  set(exports)
  set(foreign)
  foreach(row IN LISTS rows)
    string(REGEX REPLACE "^[0-9a-fA-F]+ [A-Za-z] " "" name "${row}")
    list(APPEND exports "${name}")
    if(NOT name MATCHES "^_kimodo_")
      list(APPEND foreign "${name}")
    endif()
  endforeach()
  list(LENGTH exports export_count)
  if(export_count EQUAL 0)
    message(FATAL_ERROR "${library_name} exports nothing")
  endif()
  if(foreign)
    list(JOIN foreign ", " foreign)
    message(FATAL_ERROR "${library_name} exports symbols outside the C API: ${foreign}")
  endif()
  # Its own install name comes first, then what it loads: only the system's
  # libc++, libSystem, the Objective-C runtime and system frameworks (Metal,
  # Foundation, Accelerate) may be among them.
  execute_process(COMMAND "${KIMODO_OTOOL}" -L "${macho_binary}" OUTPUT_VARIABLE dump RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "otool -L failed on ${macho_binary}")
  endif()
  string(REGEX MATCHALL "\n[ \t]+[^ \t\r\n]+ \\(compatibility" rows "${dump}")
  set(unexpected)
  foreach(row IN LISTS rows)
    string(REGEX REPLACE "^\n[ \t]+([^ \t]+) \\(compatibility$" "\\1" name "${row}")
    if(name STREQUAL own_install_name)
      continue()
    endif()
    list(APPEND imports "${name}")
    if(NOT name MATCHES "^(/usr/lib/libc\\+\\+\\.1\\.dylib|/usr/lib/libSystem\\.B\\.dylib|/usr/lib/libobjc\\.A\\.dylib|/System/Library/Frameworks/[A-Za-z]+\\.framework/.+)$")
      list(APPEND unexpected "${name}")
    endif()
  endforeach()
  if(unexpected)
    list(JOIN unexpected ", " unexpected)
    message(FATAL_ERROR "${library_name} depends on libraries a host would have to ship as well: ${unexpected}")
  endif()
  message(STATUS "${library_name} exports ${export_count} kimodo_* functions and nothing else")
  list(JOIN imports ", " import_text)
  message(STATUS "${library_name} loads ${import_text}")
endif()

# Start clean, except for the weights, which are copied only when they
# change, and WEIGHTS.md, which describes them.
file(MAKE_DIRECTORY "${out}")
file(GLOB previous LIST_DIRECTORIES true "${out}/*")
foreach(entry IN LISTS previous)
  if(NOT entry STREQUAL "${out}/weights" AND NOT entry STREQUAL "${out}/WEIGHTS.md")
    file(REMOVE_RECURSE "${entry}")
  endif()
endforeach()

foreach(binary source IN ZIP_LISTS binaries sources)
  get_filename_component(folder "${out}/${binary}" DIRECTORY)
  file(COPY "${source}" DESTINATION "${folder}")
endforeach()
if(strip_library)
  # The copy an app ships, without the symbols (the NDK builds with debug
  # information even for release); the symbols/ copy reads crash reports.
  if(NOT KIMODO_STRIP)
    find_program(KIMODO_STRIP NAMES llvm-strip strip)
  endif()
  if(NOT KIMODO_STRIP)
    message(FATAL_ERROR "llvm-strip is needed to package ${KIMODO_TARGET}: pass -DKIMODO_STRIP=<the NDK's llvm-strip>")
  endif()
  get_filename_component(folder "${out}/${strip_library}" DIRECTORY)
  file(MAKE_DIRECTORY "${folder}")
  list(GET sources 0 unstripped)
  execute_process(COMMAND "${KIMODO_STRIP}" --strip-unneeded -o "${out}/${strip_library}" "${unstripped}" RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${KIMODO_STRIP} failed on ${unstripped}")
  endif()
endif()
file(COPY "${src}/include/kimodo/kimodo_capi.h" DESTINATION "${out}/include/kimodo")

# ---- Licences --------------------------------------------------------------
file(COPY "${src}/LICENSE" "${src}/NOTICE" "${src}/scripts/build/package/THIRD_PARTY.md" DESTINATION "${out}")
file(MAKE_DIRECTORY "${out}/licenses")
file(COPY_FILE "${src}/ggml/LICENSE" "${out}/licenses/ggml-LICENSE")
file(COPY_FILE "${src}/third_party/nlohmann/LICENSE.MIT" "${out}/licenses/nlohmann-json-LICENSE.MIT")
if("vulkan" IN_LIST KIMODO_BUILD_BACKENDS)
  file(COPY_FILE "${src}/scripts/build/package/Vulkan-Headers-LICENSE" "${out}/licenses/Vulkan-Headers-LICENSE")
endif()
if(KIMODO_BUILD_POSTPROCESS)
  file(COPY_FILE "${src}/third_party/motion_correction/LICENSE" "${out}/licenses/MotionCorrection-LICENSE")
  file(COPY_FILE "${src}/eigen/COPYING.MPL2" "${out}/licenses/Eigen-COPYING.MPL2")
  if(KIMODO_BUILD_POSTPROCESS_SIMD STREQUAL "neon")
    file(COPY_FILE "${src}/third_party/sse2neon/LICENSE" "${out}/licenses/sse2neon-LICENSE")
  endif()
endif()

# ---- VERSION.json ----------------------------------------------------------
file(STRINGS "${src}/include/kimodo/kimodo_capi.h" abi_line REGEX "^#define KIMODO_CAPI_ABI_VERSION [0-9]+")
string(REGEX REPLACE "^#define KIMODO_CAPI_ABI_VERSION ([0-9]+).*" "\\1" abi "${abi_line}")
set(commit "unknown")
set(dirty "false")
find_program(GIT git)
if(GIT)
  execute_process(COMMAND "${GIT}" -C "${src}" rev-parse HEAD
                  OUTPUT_VARIABLE head RESULT_VARIABLE result OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(result EQUAL 0)
    set(commit "${head}")
    execute_process(COMMAND "${GIT}" -C "${src}" status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE changes ERROR_QUIET)
    if(NOT changes STREQUAL "")
      set(dirty "true")
    endif()
  endif()
endif()
if(KIMODO_BUILD_POSTPROCESS)
  set(postprocess "true")
else()
  set(postprocess "false")
endif()
string(TIMESTAMP built "%Y-%m-%dT%H:%M:%SZ" UTC)
json_array(backends_json ${KIMODO_BUILD_BACKENDS})
json_array(cpu_json ${KIMODO_BUILD_CPU_FEATURES})
json_array(imports_json ${imports})
set(glibc_json "")
if(glibc)
  set(glibc_json ",\n  \"glibc\": \"${glibc}\"")
endif()
if(KIMODO_BUILD_MINIMUM_OS)
  string(APPEND glibc_json ",\n  \"minimum_os\": \"${KIMODO_BUILD_MINIMUM_OS}\"")
endif()
file(WRITE "${out}/VERSION.json" "{
  \"target\": \"${KIMODO_TARGET}\",
  \"abi\": ${abi},
  \"version\": \"${KIMODO_BUILD_VERSION}\",
  \"commit\": \"${commit}\",
  \"modified\": ${dirty},
  \"build_type\": \"${KIMODO_BUILD_TYPE}\",
  \"built\": \"${built}\",
  \"system\": \"${KIMODO_BUILD_SYSTEM}\",
  \"processor\": \"${KIMODO_BUILD_PROCESSOR}\",
  \"cpu_features\": ${cpu_json},
  \"compiler\": \"${KIMODO_BUILD_COMPILER}\",
  \"ggml\": {\"version\": \"${KIMODO_BUILD_GGML_VERSION}\", \"commit\": \"${KIMODO_BUILD_GGML_COMMIT}\"},
  \"backends\": ${backends_json},
  \"post_processing\": ${postprocess},
  \"imports\": ${imports_json}${glibc_json}
}
")

# ---- Weights ---------------------------------------------------------------
if(NOT KIMODO_WEIGHTS)
  message(STATUS "Weights left out (KIMODO_WEIGHTS=OFF); ${out}/weights and WEIGHTS.md are left as they were")
  message(STATUS "Packaged ${out}")
  return()
endif()

file(GLOB motion_models "${KIMODO_WEIGHTS_DIR}/models/kimodo-*.gguf")
set(text_models)
if(KIMODO_TEXT_WEIGHTS)
  file(GLOB text_models "${KIMODO_WEIGHTS_DIR}/Llama-3-Kimodo-*.gguf")
else()
  message(STATUS "Text encoder left out (KIMODO_TEXT_WEIGHTS=OFF): generation takes embeddings made elsewhere")
endif()
set(weights)
foreach(file IN LISTS motion_models text_models)
  get_filename_component(name "${file}" NAME)
  string(TOLOWER "${name}" lower)
  if(lower MATCHES "smplx")
    message(STATUS "Not packaging ${name}: the SMPL-X checkpoint's licence forbids redistributing it")
  else()
    list(APPEND weights "${file}")
  endif()
endforeach()
set(required models/kimodo-soma-seed-v1.1-f32.gguf)
if(KIMODO_TEXT_WEIGHTS)
  list(APPEND weights "${KIMODO_WEIGHTS_DIR}/tokenizer.gguf")
  list(APPEND required tokenizer.gguf)
endif()

set(missing)
foreach(name IN LISTS required)
  if(NOT EXISTS "${KIMODO_WEIGHTS_DIR}/${name}")
    list(APPEND missing "${name}")
  endif()
endforeach()
if(KIMODO_TEXT_WEIGHTS AND NOT text_models)
  list(APPEND missing "Llama-3-Kimodo-<quantization>.gguf")
endif()
if(missing)
  list(JOIN missing ", " missing)
  message(FATAL_ERROR "The weights cache ${KIMODO_WEIGHTS_DIR} lacks ${missing}.  Download them with\n"
                      "  python scripts/download_gguf_weights.py --output . --model soma-seed-v1.1\n"
                      "or package without weights.")
endif()

# Where each file came from and its terms.  The GGUFs are LocalAI-io's
# conversions of NVIDIA's checkpoints and of the LLM2Vec Llama 3 encoder.
set(nvidia_license "[NVIDIA Open Model License](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-open-model-license/)")
function(weight_origin name out_source out_license)
  set(source "unknown: check its source and terms before sharing it")
  set(license "unknown")
  if(name MATCHES "^kimodo-(soma|g1)-(rp|seed)-(v[0-9.]+)-f32\\.gguf$")
    string(TOUPPER "${CMAKE_MATCH_1}" skeleton)
    string(TOUPPER "${CMAKE_MATCH_2}" kind)
    set(repo "Kimodo-${skeleton}-${kind}-${CMAKE_MATCH_3}")
    set(source "[LocalAI-io/${repo}-GGML](https://huggingface.co/LocalAI-io/${repo}-GGML), converted from [nvidia/${repo}](https://huggingface.co/nvidia/${repo})")
    set(license "${nvidia_license}")
  elseif(name MATCHES "^Llama-3-Kimodo-.*\\.gguf$" OR name STREQUAL "tokenizer.gguf")
    set(source "[LocalAI-io/Llama-3-Kimodo-GGML](https://huggingface.co/LocalAI-io/Llama-3-Kimodo-GGML)")
    # No semicolons in these strings: CMake would split the table row there.
    set(license "converted Meta Llama 3 material under Meta's Llama 3 licence (see the model card)")
  endif()
  set(${out_source} "${source}" PARENT_SCOPE)
  set(${out_license} "${license}" PARENT_SCOPE)
endfunction()

set(cache_file "${out}/weights/.sha256-cache")
set(cache)
if(EXISTS "${cache_file}")
  file(STRINGS "${cache_file}" cache)
endif()
set(new_cache)
set(names)
set(rows)
set(total 0)
file(MAKE_DIRECTORY "${out}/weights")
foreach(file IN LISTS weights)
  get_filename_component(name "${file}" NAME)
  list(APPEND names "${name}")
  file_size("${file}" size)
  file(TIMESTAMP "${file}" stamp "%Y-%m-%dT%H:%M:%S" UTC)
  # file(COPY) keeps the timestamp and skips a file already copied.
  if(NOT EXISTS "${out}/weights/${name}")
    message(STATUS "Copying ${name} (${size} bytes)...")
  endif()
  file(COPY "${file}" DESTINATION "${out}/weights")
  set(key "${name}|${size}|${stamp}")
  set(hash)
  foreach(line IN LISTS cache)
    if(line MATCHES "^([^|]+\\|[^|]+\\|[^|]+)\\|([0-9a-f]+)$" AND CMAKE_MATCH_1 STREQUAL key)
      set(hash "${CMAKE_MATCH_2}")
    endif()
  endforeach()
  if(NOT hash)
    message(STATUS "Hashing ${name}...")
    file(SHA256 "${out}/weights/${name}" hash)
  endif()
  list(APPEND new_cache "${key}|${hash}")
  math(EXPR total "${total} + ${size}")
  human_size("${size}" shown)
  weight_origin("${name}" source license)
  list(APPEND rows "| `${name}` | ${shown} | `${hash}` | ${source} | ${license} |")
endforeach()
list(JOIN new_cache "\n" new_cache)
file(WRITE "${cache_file}" "${new_cache}\n")

# Weights that are no longer in the cache leave the package too.
file(GLOB packaged "${out}/weights/*")
foreach(file IN LISTS packaged)
  get_filename_component(name "${file}" NAME)
  if(NOT name IN_LIST names AND NOT name STREQUAL ".sha256-cache")
    message(STATUS "Removing ${name}, which the cache no longer holds")
    file(REMOVE "${file}")
  endif()
endforeach()

list(JOIN rows "\n" rows)
human_size("${total}" total_shown)
if(KIMODO_TEXT_WEIGHTS)
  set(use "The library loads a motion model and a text encoder, and reads
`tokenizer.gguf` from the text encoder's folder.  Keep them together.")
else()
  set(use "Motion models only: the text encoder (8 GB) does not fit beside an app on
this target.  Generation takes 4096-value prompt embeddings made where the
text encoder runs (`kimodo_encode_text`), through `kimodo_generate_embedding`.")
endif()
file(WRITE "${out}/WEIGHTS.md" "# Weights

The files in `weights/`, ${total_shown} in all, copied from a local cache
on ${built}.  They are data, not code, and each keeps its own terms: read
the model cards before sharing them outside the team.

${use}

| File | Size | SHA-256 | Source | Terms |
|---|---|---|---|---|
${rows}
")
message(STATUS "Packaged ${out} with ${total_shown} of weights")
