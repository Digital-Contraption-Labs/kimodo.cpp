# Kimodo as a shared library -- plan (2026-09-29)

This document records a decision and the plan that follows from it.  It is
written for whoever carries the work out in this repository, and assumes no
knowledge of the conversation it came from.

Facts under "Where the code stands" were checked against commit `8c9a9b1` on
2026-09-29.  Check them again before relying on them; the tree moves.

## 1. The decision

ContraptionFabricator (CF) is the owner's 3D character tool.  Its Clip Editor
generates animation from text by talking to this project's Go server over
HTTP on localhost, and the server drives `kmd-generate`.  That works, but it
means every user starts and keeps alive a separate server before the feature
does anything.

The owner's decision: **package the engine as one shared library per platform,
reached through the C API, and load it straight into CF.**  The reasons are
the user's experience (no server to start) and distribution to the team (a
library and a weights folder, instead of a Go toolchain, a build and a running
process).

The HTTP service is not going away.  CF's web build cannot load a native
library and will keep using it, and a remote machine with a bigger GPU stays a
valid setup.  Nothing in this plan may break `kmd-generate`, the Go server or
the scripts under `scripts/`.

**The web build is out of scope here.**  The owner will take it up later.

## 2. What is being asked of this repository

1. A build of the library for five targets: a Windows DLL, a macOS dylib, an
   iOS library, a Linux `.so` and an Android `.so`.
2. Build scripts that produce them: `.bat` for Windows hosts, `.sh` for
   macOS, Linux and iOS.  The owner will run the macOS and iOS scripts on a
   Mac himself, so those must be written to be run by someone who was not
   there when they were written.
3. The changes to the library that loading it into another program's process
   requires (section 5), and the additions to the C API that CF needs in
   place of what the server did for it (section 6).
4. A packaged result per platform, and a script that stages it into CF's
   tree (section 8).

The CF side -- loading the library, the second transport, the menu -- is
described in section 10 for context only.  It is not work for this
repository.

### Decisions (2026-09-29)

The owner's answers to the questions this plan raised:

- **Constraints** cross the C API as structs in the model's joint order.
  The library also takes over everything `demo/constraints.go` does for the
  server today -- NVIDIA's constraints JSON, axis-angle rotations, SOMA
  77-joint poses, the named end-effector types and the validation -- so a
  file saved from NVIDIA's demo loads in CF with no server.  Section 6.
- **Clip length.**  The app may raise the frame limits up to the most the
  library can handle; the defaults stay modest.  Section 6.
- **GPU memory.**  Full residency.  The app starts the engine when it enters
  generation mode and stops it when it leaves, possibly several times in a
  session.  Starting loads everything; stopping releases all of it, host and
  GPU memory.
- **Models.**  The default motion model is `kimodo-soma-seed-v1.1-f32.gguf`.
  The app may name another one that is present in the data folder.
- **Post-processing on ARM** is wanted: compile MotionCorrection's SSE code
  through `sse2neon`.  Part of the macOS step.
- **Targets.**  Windows first, as far as CF running it.  The other targets
  follow, so that CF can be tested on an Android phone and the rest.
- **Weights** are copied into the package from this repository's local
  cache.  Updating them stays manual; no fetch script.
- **Data folder.**  CF keeps the data files together in `Assets/kimodo/`, so
  the tokenizer's fixed place beside the text GGUF stays as it is.

## 3. Where the code stands

**What the Windows build produces today.**  `scripts/build/build_service.bat`
builds `kmd-generate.exe` with five DLLs beside it (release sizes):

| File | Size |
|---|---|
| `kimodo.dll` | 0.56 MB |
| `ggml.dll` | 0.07 MB |
| `ggml-base.dll` | 0.65 MB |
| `ggml-cpu.dll` | 0.80 MB |
| `ggml-vulkan.dll` | 52.7 MB |

So a DLL exists, but in five pieces, and `kimodo.dll` cannot be loaded
without the other four.

**The C API** (`include/kimodo/kimodo_capi.h`, ABI 2) is flat, catches every
exception, versions its option structs by `size`, and already carries the
export macros (`KIMODO_SHARED`, `KIMODO_BUILD`).  It covers: load and free a
model, its joint count, generate from a prompt, generate with kinematic
constraints, generate from a 4096-float embedding, and read a motion's frames,
joints, local rotations and root positions.

**What the C API lacks**, all of which the C++ API or the server has:

- multi-prompt sequences (`model::generate_text_sequence` is C++ only);
- the skeleton: `kimodo_model_joints` gives a count, but joint names, parents
  and rest offsets live in `src/skeleton.hpp` and are not reachable;
- progress and cancel (`docs/IMPLEMENTATION.md` lists a progress callback as
  planned; it was never added);
- a way to ask what the build can do (`model::post_processing_available` is
  C++ only).

**The C++ class is exported too.**  `kimodo::model` in `kimodo.hpp` carries
`KIMODO_API`, and `kmd-generate` uses it across the DLL boundary.  That class
passes `std::expected`, `std::string` and `std::unique_ptr` between modules,
which only works while both sides are built by the same compiler with the
same runtime.

**Runtime options are declared and ignored.**  `kimodo_runtime_options` has
`threads`, `device` and `backend_dir`; `src/capi.cpp` checks the struct's size
and reads none of them.

**Behaviour is steered by environment variables.**  The library reads
`KIMODO_BACKEND`, `KIMODO_THREADS`, `KIMODO_TEXT_LAYER_CHUNK`,
`KIMODO_TEXT_RESIDENT_LIMIT_MIB`, `KIMODO_TEXT_PACKED_LORA`,
`KIMODO_MOTION_PACKED_ATTENTION`, `KIMODO_MOTION_GRAPH_CACHE`,
`KIMODO_MOTION_LAYER_CHUNK` and `KIMODO_PROFILE`.

**The library also writes the environment.**  `configure_vulkan_f32_parity`
in `src/ggml_weights.cpp` sets `GGML_VK_DISABLE_COOPMAT`,
`GGML_VK_DISABLE_COOPMAT2` and `GGML_VK_DISABLE_F16` (`setenv` on Unix,
`_putenv_s` on Windows), because ggml's Vulkan backend offers no other way to
switch those paths off and the reference model is F32.

**The library prints.**  With `KIMODO_PROFILE` set it writes timing lines to
`stderr` from `model.cpp`, `denoiser.cpp`, `llm_text_encoder.cpp` and
`ggml_weights.cpp`.

**Backends.**  ggml is pinned at v0.20.2 as a submodule.  Only Vulkan and the
CPU are wired in (`KIMODO_HAVE_GGML_VULKAN`).  ggml's Metal backend is not
used anywhere in this code.

**Post-processing is x86 only.**  The vendored MotionCorrection library is SSE
code built with AVX, so `CMakeLists.txt` builds it only when
`CMAKE_SYSTEM_PROCESSOR` is an x86 name.  On ARM, foot-skate cleanup and the
IK that pins constraints are absent.

**Two things in `CMakeLists.txt` to look at before building on it:**

- `add_library(kimodo ...)` names no type, and the line that forces
  `BUILD_SHARED_LIBS ON` comes after it.  The library's type therefore appears
  to depend on whether a cache exists from an earlier configure.  Confirm with
  a fresh build directory.
- The `windows-release` and `windows-debug` presets that `build_service.bat`
  uses are in `CMakeUserPresets.json`, which is tracked.  CMake treats that
  file as the user's own, so project presets belong in `CMakePresets.json`.

**Paths.**  The C API takes `const char *` paths, and
`llm_text_encoder.cpp` builds `std::filesystem::path` from them.  On Windows
that reads a narrow string in the system code page, not UTF-8, so a weights
folder under a user name with an accent will fail to open.

## 4. The targets

| Target | Result | Architecture | GPU backend | Post-processing | Built on |
|---|---|---|---|---|---|
| Windows | `kimodo.dll`, import library, PDB | x64 | Vulkan | yes | Windows, Visual Studio 2022 or later |
| Linux | `libkimodo.so` | x86_64 | Vulkan | yes | Linux, or WSL from Windows |
| macOS | `libkimodo.dylib` | arm64, and x86_64 if it costs little | Metal | yes (arm64 through `sse2neon`) | a Mac |
| iOS | `kimodo.xcframework` | arm64 device, arm64 simulator | Metal | yes, through `sse2neon` | a Mac |
| Android | `libkimodo.so` | arm64-v8a, x86_64 optional | Vulkan | yes, through `sse2neon` | Windows or Linux with the NDK |

Every target keeps the CPU backend as the fallback.

Notes on each:

- **Windows** is the first target and the only one that can be built, run and
  measured end to end on the owner's development machine.
- **Linux.**  The machine has WSL with Ubuntu and Debian 12.  Build in Debian
  12 so the library's glibc floor is that release's.  Vulkan inside WSL is a
  software driver (llvmpipe), so WSL proves the build and the CPU path; a GPU
  run needs a real Linux machine.
- **macOS.**  CF's own renderer reaches Metal through MoltenVK, but for this
  library ggml's Metal backend is the better road: it needs no Vulkan loader
  beside the dylib.  Build ggml with `GGML_METAL_EMBED_LIBRARY` so the shader
  library is inside the binary.  This is new code in `ggml_weights.cpp` and
  the text encoder, and it needs its own parity run against the F32 fixtures.
- **iOS.**  The owner asked for a dylib.  A shipped iOS app cannot load a
  loose dylib; it must be a framework inside the app, signed with it.  Build
  a dynamic framework and wrap the device and simulator slices as an
  XCFramework.  llama.cpp's `build-xcframework.sh` builds ggml this way and
  is worth reading first.  Signing is the app's job, not this script's.
- **Android.**  Match CF exactly: NDK `27.2.12479018`, `arm64-v8a`,
  `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON` (Android 15 devices with 16 KB
  pages refuse a library aligned to 4 KB).  CF's phone build has minSdk 33
  and its Quest build minSdk 29; build for 29 so one library serves both.
  ggml's Vulkan shaders are compiled by a tool that runs on the build
  machine, so a cross build needs a host compiler as well as the NDK.  How
  well ggml's Vulkan backend behaves on Adreno and Mali drivers is unknown
  until it runs on a device; ggml also has an OpenCL backend tuned for
  Adreno, which is the alternative to evaluate if Vulkan disappoints.

**A limit to state plainly.**  The text encoder is Llama-3-8B: 4.4 GB at
Q4_K, 8.1 GB at Q8.  No phone, tablet or headset will hold it beside an app.
On iOS and Android the library is expected to run the motion model only,
from an embedding made elsewhere, through `kimodo_generate_embedding`.  The
motion model is 1.13 GB and F32 only, which a high-end device can hold.  So
on those targets the library must load and work with no text model given.

## 5. What loading into another program's process requires

These follow from one fact: the library will share a process, and a GPU, with
an application that knows nothing about it.

**One file, one boundary.**  Link ggml statically into the library.  Export
the C API and nothing else: no C++ class, no ggml symbol, no symbol from the
C++ runtime.  The reason is that a C boundary is the only one that survives
the two sides being built by different compilers and standards; CF is C++20
and this library is C++23.

The tools and tests still need the C++ API.  One way to give it to them is a
static core target that holds every source, which the shared library and the
tools both link.  Choose the structure that fits; the requirement is that
`kmd-generate` keeps working and does not depend on the shared library's
exports.

**No surprise dependencies.**  The library may depend on the platform's
system libraries, its C and C++ runtimes and its GPU loader, and on nothing
else.  In particular build ggml with `GGML_OPENMP=OFF`, or the library gains
a dependency on an OpenMP runtime that is not on every machine.

| Target | May depend on |
|---|---|
| Windows | system DLLs, the Visual C++ runtime (`/MD`, as CF uses), `vulkan-1.dll` |
| Linux | glibc, `libvulkan.so.1`; link libstdc++ and libgcc statically |
| macOS, iOS | system frameworks and the system libc++ |
| Android | `libc`, `libm`, `libdl`, `liblog`, `libvulkan.so`; link the C++ runtime statically (`c++_static`), since CF ships `c++_shared` and two copies only coexist when nothing C++ crosses the boundary |

**No environment variables as controls.**  Every `getenv` in the library
becomes a field of `kimodo_runtime_options`, which already exists for this.
The tools may keep reading the environment and pass what they read through
the options.  The owner's rule in his own projects is that levers are flags
and options, never the environment.

The Vulkan parity switches are the hard case, because ggml reads them from
the environment itself.  Three choices, in order of preference:

1. Find out when ggml reads them.  If it is only while the device is
   created, set them immediately before `ggml_backend_vk_init` and restore
   the previous values immediately after.
2. If they are read later as well, leave them set for the library's lifetime
   and say so in the header.
3. Patch ggml to take them as parameters.  This is the clean answer and the
   most expensive to maintain against a pinned submodule; do it only if the
   first two fail.

**No printing.**  Add a log callback to the runtime options.  With none set
the library is silent.

**The GPU is shared.**  CF's renderer holds the same GPU.  How much memory
the text encoder keeps resident, and whether it stays loaded between
generations, must be the caller's choice through the runtime options, not a
default the caller cannot see.

**Load and free must balance.**  `kimodo_model_free` has to release the GPU
device and its memory.  A user will generate, close the panel and open it
again many times in one session.

**One generation at a time, from any thread.**  CF will call from a worker
thread.  State in the header what may be called concurrently and what may
not.

**Paths are UTF-8** on every platform, converted inside the library.

## 6. C API additions (ABI 3)

Proposals.  The names and shapes may change; the rules may not: append
fields, never reorder; keep accepting the ABI 1 and ABI 2 struct sizes; add
functions, never change one.

| Addition | Why CF needs it |
|---|---|
| Honour `device` and `threads`; add GPU index, text resident limit, text layer chunk, log callback | section 5 |
| `kimodo_get_capabilities`: ABI, library version and commit, which devices were compiled in, whether post-processing is present | CF hides what a build cannot do instead of failing at the click |
| Skeleton accessors: skeleton name, and per joint its name, parent and rest offset; the model's frame rate | CF builds its clip from the arrays; without these it must carry a copy of `skeleton.hpp` that will drift |
| `kimodo_generate_sequence`: segments of prompt and frame count, transition frames, options, constraints | the server's multi-prompt generation |
| Progress callback with stage, step and total, whose return value cancels; a status that tells cancelled from failed | a generation runs for seconds to minutes and CF's operations show progress and offer Cancel |
| `kimodo_encode_text`: a prompt to its 4096 floats | lets a desktop make the embeddings that the mobile targets consume |
| Open by data folder: a folder, and optional motion and text file names defaulting to `kimodo-soma-seed-v1.1-f32.gguf` and `Llama-3-Kimodo-Q8_0.gguf`; a call that lists the motion models present | CF points at `Assets/kimodo/` and offers the models it finds |
| Start and stop: opening loads every weight onto the device (motion weights load lazily at the first generation today); text residency `auto` keeps the whole encoder resident when the device has the memory, else runs it in chunks | the engine is started and stopped with the generation mode |
| `kimodo_generation_options_init(model, &options)`: the server's defaults -- 150 frames, 100 steps, text and constraint guidance 2.0, transition 5, root margin 0.04, post-processing on except for G1 | a zeroed struct means no text guidance and zero steps; the defaults then live in one place |
| Limits: `kimodo_get_limits` for the ceilings the library can handle and the frame rate; a runtime option setting the active limits, clamped to those ceilings; every entry point enforces them | CF may raise the clip length (see below) |
| Constraint input: the struct path gains axis-angle rotations and SOMA 77-joint poses; NVIDIA's constraints JSON is accepted as well; one validation for both | the port of `demo/constraints.go` (see below) |

**Limits.**  Today they disagree.  The server allows 60..360 frames per
segment, 16 segments, transitions of 1..60 frames and 1..1000 steps; the C++
sequence path allows 2..360 per segment; the single-prompt path, which the C
API's `kimodo_generate` takes, allows 1..10000.  One table in the core
replaces them.  Active limits default to the server's (60..360 frames per
segment, 16 segments).  The ceilings are what the library can handle: the
motion model computes its sinusoidal positions at run time, so no table caps
the length; memory does, since attention grows with the square of the frames
and guidance runs a batch of three.  Measure the ceiling on the development
GPU and record it here.  Quality past 12 s is unmeasured, so the header says
so.

**Constraints.**  Port `demo/constraints.go` to the library: the types
`root2d`, `fullbody`, `end-effector` and the four named limbs; axis-angle
`local_joints_rot` and `local_joints_rot_xyzw`; SOMA 77-joint poses mapped to
the 30 joints; frame and shape checks; at most 256 constraints.  The JSON
needs a parser: vendor a mature one under a permissive licence into
`third_party/` and add the entry point to the libFuzzer targets.  Port the
cases in `demo/constraints_test.go` to C++ so the two implementations are
held to the same inputs.  The server keeps its Go copy for now; handing the
JSON through to `kmd-generate` and deleting that copy is a later option.

## 7. The scripts

Follow the conventions this repository already has in
`scripts/build/build_service.bat`, and these rules of the owner's:

- every script answers `-h` and `--help` with its usage and exits 0 before
  doing any work;
- choices are flags, not environment variables;
- a missing prerequisite is reported by name with the command that installs
  it, as `build_service.bat` does;
- no prompts; a script either runs or stops with a reason and a non-zero
  exit.

Suggested set, under `scripts/build/`:

| Script | Host | Produces |
|---|---|---|
| `build_library_windows.bat` | Windows | the Windows package |
| `build_library_linux.sh` | Linux, WSL | the Linux package |
| `build_library_linux.bat` | Windows | runs the `.sh` inside WSL, as CF's `BuildLinux.bat` does |
| `build_library_macos.sh` | Mac | the macOS package |
| `build_library_ios.sh` | Mac | the iOS package |
| `build_library_android.bat`, `.sh` | Windows, Linux or Mac | the Android package |
| `stage_to_cf.bat`, `.sh` | any | copies a package into CF's tree |

Keep them thin.  Put the settings in `CMakePresets.json`, one preset per
target, so a script is a prerequisite check, a preset and a packaging step.
Each takes `--release` (default) or `--debug`, and `--out <folder>`.

The Windows, Linux and Android scripts can be run and checked on the
development machine.  The macOS and iOS scripts cannot.  Write those with
more care for that reason, and hand the owner the exact commands to run and
what each should print.

## 8. The package and its place in CF

One folder per target under `dist/`, which `.gitignore` should cover:

```
dist/kimodo-<target>/
  include/kimodo/kimodo_capi.h
  bin/    kimodo.dll, kimodo.pdb                 (Windows)
  lib/    kimodo.lib | libkimodo.so | libkimodo.dylib | kimodo.xcframework
  LICENSE  NOTICE  THIRD_PARTY.md
  VERSION.json     ABI, version, commit, ggml version, backends, post-processing
  weights/         the motion GGUFs in the cache, Llama-3-Kimodo-Q8_0.gguf,
                   tokenizer.gguf -- copied from this repository's cache
  WEIGHTS.md       each weight file's source, size, SHA-256 and licence
```

The cache today holds `kimodo-soma-seed-v1.1-f32.gguf`,
`kimodo-soma-rp-v1.1-f32.gguf`, `kimodo-g1-rp-v1-f32.gguf` (1.13 GB each),
`Llama-3-Kimodo-Q8_0.gguf` (8.14 GB) and `tokenizer.gguf`: 11.5 GB.  An
SMPL-X checkpoint is never copied, even when one is in the cache (section 13).
The scripts take `--no-weights` for a rebuild that only changes the library.

`THIRD_PARTY.md` names what is compiled in and under which terms: ggml
(MIT), MotionCorrection (Apache-2.0), Eigen (MPL-2.0, built with
`EIGEN_MPL2_ONLY`).

`stage_to_cf` copies a package's library to `<CF>/external/kimodo/<target>/`
and its weights to `<CF>/Assets/kimodo/`, where `<CF>` is given by
`--cf <path>`.

Two facts about CF's tree that the staging must respect:

- CF's `.gitignore` ignores `*.dll` and `*.dylib`.  Whether the binaries are
  committed to CF (behind an exception for that folder) or installed by CF's
  setup scripts is the owner's call.  For a team the first is simpler, since
  nobody else then needs this repository's toolchain.
- The weights never go into git, in either repository: GitHub refuses any
  file over 100 MB.  CF's `Assets/kimodo/` is ignored by its git and
  excluded from its web and device packaging.

## 9. Proving it

For each target, as far as the machine allows:

1. **Dependencies.**  List what the library imports (`dumpbin /dependents`,
   `readelf -d`, `otool -L`) and compare with the table in section 5.
2. **Exports.**  List what it exports (`dumpbin /exports`, `nm -D
   --defined-only`, `nm -gU`).  Every name begins with `kimodo_`.
3. **A smoke test written in C**, not C++, compiled against the header
   alone, that loads the library at run time (`LoadLibrary`, `dlopen`),
   checks the ABI, and, given a model path by flag, generates a short clip
   and prints a checksum of it.  Being C proves the header is; loading at
   run time is how CF will load it.
4. **Parity.**  The existing fixture tests pass against the shared build on
   the CPU and on the GPU.  On Metal this is the first time they run.
5. **Balance.**  Load, generate and free in a loop; memory and GPU memory
   return to where they started.
6. **A path with a character outside ASCII** opens.
7. **The service still builds and runs**: `build_service.bat`, then a
   generation through the server.
8. **Constraints through the C API.**  The same keyframes given as structs
   and as NVIDIA's JSON produce the same clip; with post-processing the
   constrained joints land on their targets; the ported Go test cases pass.
9. **Start and stop.**  Opening loads every weight; after stopping, the
   process's GPU memory is back where it was before the start.

State in the final report, per target, which of these ran and which were
only compiled.  A library that built but never ran is reported as that.

## 10. How CF will use it

For context.  None of this is work for this repository.

- **Found at run time, not linked.**  CF includes only `kimodo_capi.h` and
  loads the library when the Clip Editor first needs it.  The standard CF
  build gains no dependency, and the local entry appears only when the
  library and the weights are both present.
- **A second transport behind the same job.**  CF's `KimodoJob`
  (`App/src/kimodo_client.h`) already runs a generation as an operation with
  progress and Cancel.  It gains a transport that calls the library from a
  worker thread.  The panel does not change.
- **Clips from arrays.**  Today CF imports the GLB the Go server writes.
  With the library it builds the clip from the rotation and root arrays and
  the skeleton accessors.  CF's project rule is that behaviour lives once in
  shared code and each platform file only translates; loading a dynamic
  library is the platform's part, and everything after the arrays is shared.
- **Weights in `Assets/kimodo/`**, ignored by git, with a tracked manifest of
  names, sizes and SHA-256, filled by a setup script that ports
  `scripts/download_gguf_weights.py`.  The files CF uses today:
  `kimodo-soma-rp-v1.1-f32.gguf` (1.13 GB), `Llama-3-Kimodo-Q8_0.gguf`
  (8.14 GB) and `tokenizer.gguf` (7 MB).  CF's packaging for the web and for
  devices reads from `Assets/`, so that folder has to be excluded from it.
- **The cost.**  The server was a separate process, so a GPU fault inside
  ggml ended the server and CF reported an error.  Inside CF's process the
  same fault ends CF.  This was raised when the decision was made, and the
  owner chose the simpler setup.

## 11. Order of work

1. **Windows, one file.**  Restructure the build so `kimodo.dll` stands
   alone with ggml inside it, exporting the C API only.  Checks 1, 2, 3 and
   7 of section 9.
2. **In-process fitness.**  Runtime options honoured, no environment reads,
   the Vulkan switches handled, log callback, UTF-8 paths.  Checks 4, 5
   and 6.
3. **ABI 3.**  Capabilities, skeleton, sequence, progress and cancel,
   `kimodo_encode_text`, opening by data folder, start and stop, the
   options initialiser, limits, and the constraints port with its JSON.
   Extend `tests/capi_test.cpp`; checks 8 and 9.
4. **Linux**, in WSL's Debian 12.
5. **Android**, compiled against the NDK.
6. **macOS**, including the Metal backend and its parity run, and
   MotionCorrection on arm64 through `sse2neon` (MIT, one header; it covers
   every SSE intrinsic the library uses except AVX's `_mm_permutevar_ps`,
   which needs a small shim), checked against the x86 output.  The owner
   runs it.
7. **iOS.**  The owner runs it.
8. **Packaging and `stage_to_cf`.**

Windows through step 3 is the part CF is waiting on.  Steps 4 to 7 can
follow in any order.

### Progress

**Step 1, done 2026-09-29.**  `kimodo.dll` stands alone.

- `CMakeLists.txt`: the engine is a static `kimodo-core`, which the tools
  and tests link; `kimodo` is a `SHARED` library of `src/capi.cpp` over it.
  ggml is static and built without OpenMP.  Every object carries debug
  information, so `kimodo.pdb` covers ggml too.  `kimodo::model` no longer
  carries `KIMODO_API`.  Both issues in section 3's "Two things" are fixed:
  the library's type is explicit, and the Windows presets moved to
  `CMakePresets.json` (`CMakeUserPresets.json` is ignored now).
- `kimodo_capi.h` gained a function pointer type per entry point
  (`kimodo_generate_fn` and so on), checked against the declarations at
  compile time, for a host that loads the library at run time.  No ABI
  change.
- Scripts: `build_library_windows.bat`; `windows_prepare.bat`, the toolchain
  and prerequisite part it shares with `build_service.bat`; `package.cmake`,
  the packaging and checks, written in CMake so the other targets' scripts
  reuse it; `stage_to_cf.bat`.
- Checks run on the development machine: 1 (imports `vulkan-1.dll`,
  `KERNEL32`, `ADVAPI32` and the Visual C++ runtime; `package.cmake` fails
  on anything else), 2 (13 exports, all `kimodo_*`; likewise enforced),
  3 (`tests/capi_smoke.c` loads the packaged DLL from `dist/` and generates
  90 frames with SOMA SEED and the Q8 encoder on the RTX 5080), and 7
  (`build_service.bat`, then a generation through the server).  The packaged
  weights' SHA-256 match the published manifests.
- The library needs an x86-64 CPU with AVX2 (`VERSION.json`,
  `cpu_features`).
- Found for step 2: ggml prints its Vulkan device list to `stderr`, and the
  machine has two Vulkan devices (the RTX 5080 and a Radeon 890M), so the
  GPU index option matters.

**Steps 2 and 3, done 2026-09-29.**  ABI 3; everything in section 6 plus the
decisions of section 2.

- No environment reads in the library.  `kimodo::runtime_options` (C++)
  carries every lever; the C struct carries the ones a host needs (device,
  GPU index, threads, text residency, limits).  The execution-path switches
  (packed LoRA and attention, graph cache, motion layer chunk) are C++ only,
  with the fast paths as defaults.  The tools read the old variables in
  `src/environment_options.hpp` and pass them in; the Go server and
  `run_quantization_comparison.py` are unchanged.
- The Vulkan parity switches took choice 1: `src/backend.cpp` sets them
  around device creation only and restores the environment.  ggml creates a
  device once per process and caches it, so whichever model creates it fixes
  its switches; the text encoder loads first, as it always did, so results
  are unchanged.
- Logging is library-wide (`kimodo_set_log_callback`), not a runtime option,
  because ggml's own log is.  Silent until a host sets it.  One gap: ggml's
  Vulkan backend writes some error paths straight to `std::cerr` (a failed
  device allocation, for one), which only patching ggml would capture.
- `kimodo_open` preloads; `kimodo_model_free` releases.  Residency `auto`
  keeps the 7.8 GB Q8 encoder resident when the device has it plus 3 GB free
  (the motion model, compute, and the host's own GPU use).
- Limits: defaults 2..360 frames per segment, 16 segments.  Ceiling measured
  on the RTX 5080 (16 GB) with the encoder resident: 1800 frames generate
  (9.5 s at 10 steps); 3600 fail allocating one 1.28 GB attention buffer, and
  the error now says to use shorter segments.  Ceiling set at 1800; 64
  segments.
- Progress stages are loading, encoding text and sampling (the diffusion
  steps over every segment); post-processing is quick and not reported.
- The constraints port reads NVIDIA's JSON with nlohmann/json 3.11.3
  (vendored, MIT), fuzzed by `fuzz/constraints_json_fuzz.cpp` in the Clang
  `fuzz` preset (written, not run: this machine has no libFuzzer).
- Checks run on the development machine, through `tests/capi_smoke.c`
  against the packaged DLL unless noted:
  - 1, 2: 29 exports, all `kimodo_*`; imports unchanged.
  - 3: C loads the DLL at run time; every ABI 3 entry point resolved.
  - 4: the fixture tests cannot run here (the SMPL-X model and `fixtures/`
    are absent).  In their place: the new DLL reproduces the step-1 DLL
    bit for bit (60 frames/10 steps and 90/20, seed 1), and NVIDIA's example
    `kimodo-soma-rp/07_mixed_constraints` gives byte-identical motion through
    the library's JSON port and through the Go server.
  - 5, 9: open, generate and free four times: after the first stop the
    process keeps 137 MiB more host memory and 19 MiB less GPU memory free
    than before the first start (ggml's Vulkan instance and device, which it
    never releases, and the driver), and later cycles add +2 MiB and 0 MiB.
  - 6: opened from a folder named `wéights-ünïcode`.
  - 7: `build_service.bat`, then one- and two-segment generations through
    the server.
  - 8: a pose pinned as struct and as JSON gives the same clip; with
    post-processing the hips land 0.0 mm and every joint 0.0° from the
    keyframe (179 mm and 13.8° off without it).  ABI 2 constraint arrays and
    options give the ABI 3 clip.  `tests/constraints_json_test.cpp` passes the
    Go server's cases.
  - Also: cancel returns `KIMODO_CANCELLED`; `kimodo_encode_text` +
    `kimodo_generate_embedding` equals `kimodo_generate`.

**Steps 4 and 5, Linux and Android, done 2026-09-29.**

- Headers: ggml's Vulkan backend needs newer Vulkan headers than Debian 12
  (1.3.239) or the NDK (1.3.275, and no `vulkan.hpp`) ship, and it includes
  `spirv.hpp` without linking the SPIR-V headers it looks up.  Linux and
  Android therefore compile against two pinned submodules at the Windows
  SDK's version, `vulkan-headers` (v1.4.350) and `spirv-headers`
  (vulkan-sdk-1.4.350.0), with `cmake/spirv-headers/` answering ggml's
  `find_package`; a cross build uses nothing of the host's but glslc.
- ELF exports: `--exclude-libs,ALL`, and a version script (`src/kimodo.map`)
  for the unique globals libstdc++'s templates emit whatever the visibility.
  Linux links libstdc++ and libgcc statically; Android uses `c++_static`.
- Post-processing on ARM64 through sse2neon 1.9.1 (vendored, MIT) and a shim
  `third_party/sse2neon/include/immintrin.h` that adds AVX's
  `_mm_permutevar_ps`; MotionCorrection stays unmodified.  Its `Compiler.h`
  defines `FORCE_INLINE` for MSVC and GCC only, so Clang builds (the NDK,
  Apple) get GCC's definition from CMake.
- Linux (`build_library_linux.sh`, or `.bat` through WSL's Debian12):
  `libkimodo.so`, 29 exports, needs `libvulkan.so.1`, `libm`, `libc` and the
  loader, glibc 2.36 or later (recorded in `VERSION.json`).  Run in WSL on
  the CPU: loads, and generates a post-processed clip (60 frames, 10 steps,
  34 s).  A GPU run needs a Linux machine.
- ARM64 checked without ARM hardware: an aarch64 Linux cross build under
  QEMU passes `postprocess_test` (smoother 4.8e-7 m from upstream's values;
  keyframe and hand pinned to 0.0000 m; foot skate 0.095 m to 0), and the
  aarch64 library loads with post-processing present.
- Android (`build_library_android.bat` or `.sh`): NDK 27.2.12479018,
  arm64-v8a, API 29, `c++_static`, 16 KB-aligned segments (checked).  The
  package holds `lib/arm64-v8a/libkimodo.so` stripped (39 MB), its symbols in
  `symbols/` (82 MB), the motion models only, and an ARM build of the smoke
  test with the `adb` commands to run it; `--save-embedding` on a desktop and
  `--no-text --embedding-file` on the device carry a prompt across.  Built,
  checked and packaged; not yet run on a device.
- Every package now carries `tools/kimodo-capi-smoke`.

## 12. Questions that are the owner's to answer

Answered on 2026-09-29; see "Decisions" in section 2.  Still open, none of
them blocking the Windows step:

- Are the binaries committed to CF's tree, or installed by its setup
  scripts?
- Is a macOS x86_64 slice wanted, or Apple Silicon only?
- Is an Android x86_64 slice wanted for the emulator?

## 13. Licences

The port is Apache-2.0.  The weights keep their own terms: the SOMA and G1
checkpoints are under the NVIDIA Open Model License, which permits
commercial use; the text encoder contains converted Meta Llama 3 material
under its own licence; the SMPL-X checkpoint is for internal research only
and must not be packaged or fetched by anything built here.
