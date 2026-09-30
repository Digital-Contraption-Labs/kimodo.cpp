# kimodo.cpp

GGML/C++ implementation of NVIDIA's Kimodo text-to-motion model.

## Status

The five released Kimodo motion checkpoints accept either a UTF-8 prompt or a
precomputed LLM2Vec embedding and generate local rotations plus root
translations on CPU or Vulkan:

- SMPL-X RP v1: 22 joints
- SOMA RP/SEED v1.1: the predicted compact 30-joint control skeleton
- G1 RP/SEED v1: 34 Unitree G1 joints

NVIDIA's Python API expands SOMA's predicted 30 joints to a relaxed-hand
77-joint presentation skeleton. The native API currently returns the 30 joints
the model actually predicts. The text encoder stays resident on the GPU when
the GPU has room for it beside the motion model, and otherwise streams through in
eight-layer chunks; the tools take `KIMODO_TEXT_LAYER_CHUNK=1..32` to choose.

Included: checked GGUF loading, safetensors conversion, DDIM sampling, C/C++
APIs, conditioned multi-prompt transitions, kinematic constraints (full-body
keyframes, end effectors, root paths and waypoints), CPU/Vulkan parity tests,
skeleton-only GLB export, selective LLM2Vec quantisation, divergence reporting,
upstream's post-processing (foot-skate cleanup and IK onto constraints, x86
only), and local generation/comparison viewers. 77-joint SOMA expansion,
skinned-mesh GLB export, and motion-denoiser quantisation are not implemented
yet.

## Build and test on Linux

Install a C++23 compiler, CMake 3.25+, Ninja, Python 3 with the Hugging Face
CLI (`pip install huggingface_hub`), and the Vulkan loader/headers for Vulkan
support. GGML and Eigen (for post-processing) are pinned Git submodules:

```sh
git submodule update --init --recursive
scripts/download_gguf_weights.sh --output "$PWD" --model soma-rp-v1.1
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The standard test suite requires the local motion GGUF, text bundle, and
fixtures. It never downloads weights by itself. `release`, `asan-ubsan`, and
`fuzz` presets are also available.

Nix is optional and provides these dependencies reproducibly:

```sh
nix develop path:. --command cmake --preset debug
nix develop path:. --command cmake --build --preset debug
```

For sanitizer work:

```sh
nix develop path:. --command cmake --preset asan-ubsan
nix develop path:. --command cmake --build --preset asan-ubsan
nix develop path:. --command env \
  LD_LIBRARY_PATH="$PWD/build/asan-ubsan/ggml/src:$PWD/build/asan-ubsan/ggml/src/ggml-vulkan:$LD_LIBRARY_PATH" \
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 \
  ctest --preset asan-ubsan --output-on-failure
```

Leak detection is disabled because Vulkan loader/driver allocations are global
to the process. The GGUF parser fuzzer requires Clang.

## API

`include/kimodo/kimodo_capi.h` is the C API. Model loading checks the motion
GGUF and text model before inference. The text model can be a monolithic GGUF
beside `tokenizer.gguf` or the legacy component directory. Use
`kimodo_generate_embedding` for
4096 F32 values or `kimodo_generate` for text. Both return the selected model's
root translations and local XYZW rotations; query the joint count from the
result rather than assuming a fixed skeleton. `kimodo_generate_constrained`
adds kinematic constraints; see [`docs/CONSTRAINTS.md`](docs/CONSTRAINTS.md).

## Demo

After building the debug preset and downloading the native GGUF bundle:

```sh
go run ./demo -addr 0.0.0.0:8094
```

Open `http://localhost:8094`. The left sidebar contains the prompt and a
history of this session's animations plus motion-model and
text-encoder-quantization selectors;
choosing a previous animation restores its prompt and encoder choice for a new
generation. Every successful animation is also built as a standalone
`animation.glb` beside its raw streams. By default nothing is written to
disk: the gallery, the streams and the GLB live in the server's memory (the
most recent 64 animations) and are gone when it stops. With
`-output demo-output` every animation also persists there as
`<animation-id>.json` beside `<animation-id>/animation.glb` and its raw
streams, and the gallery reloads on the next start. The GLB contains the
selected animated node hierarchy (no mesh), ready to copy into a Three.js
project; it is available from `/api/animations/<animation-id>/animation.glb`
while the demo is running. Segments may be 60..360 frames (2..12 s at
30 fps).

`POST /api/generate` also takes kinematic constraints in NVIDIA Kimodo's
constraints JSON, so a file saved from the upstream demo posts unchanged; the
page's "Pin current pose" turns the pose on screen into a keyframe for the next
generation. Like NVIDIA's demo, the server post-processes by default (not on
G1): foot-skate cleanup and IK that land constraints exactly; send
`"post_processing": false` for the raw diffusion output. The request format and
what to expect are in [`docs/CONSTRAINTS.md`](docs/CONSTRAINTS.md).

On Windows, `scripts\build\build_service.bat` builds the service (release by
default, `--debug` for a debug build) from any shell: it loads the Visual
Studio x64 toolchain itself, initialises the submodules, configures and
builds the `windows-release` or `windows-debug` preset's `kmd-generate`, and
builds the Go server. `scripts\start-demo.bat` then runs the demo with the
persistent gallery and opens the page; `scripts\start-server.bat` runs the
service alone, in memory, for a client such as ContraptionFabricator's Clip
Editor; `scripts\stop-demo.bat` and `scripts\stop-server.bat` stop it from
anywhere.

For a host application that runs the engine in its own process instead of
talking to the server, `scripts\build\build_library_windows.bat` builds
`kimodo.dll` (the engine and ggml in one file, exporting only the C API in
`include/kimodo/kimodo_capi.h`), checks its exports and dependencies, and
packages it with the header, licences, `VERSION.json` and the weights from
the local cache into `dist\kimodo-windows`. `scripts\build\stage_to_cf.bat
--cf <folder>` copies that package into ContraptionFabricator.
`build_library_linux.sh` (or `.bat`, through WSL) and `build_library_android`
(`.bat` or `.sh`, with the NDK) build the Linux and Android packages the same
way; the Linux and Android builds compile against the pinned `vulkan-headers`
and `spirv-headers` submodules. The plan for
the library and its other platforms is in
[`docs/SHARED_LIBRARY_PLAN.md`](docs/SHARED_LIBRARY_PLAN.md).

The demo keeps all 32 layers of its default Q8 text encoder in VRAM for maximum
throughput, while executing them as bounded eight-layer GGML graphs. Its 10 GiB
residency ceiling makes the larger BF16 reference stream in bounded groups on a
16 GiB GPU. Elsewhere the library decides: the encoder is resident when the
GPU has room for it beside the motion model (the C API's `text_layer_chunk`
and `text_resident_limit_mib` override that; the tools take
`KIMODO_TEXT_LAYER_CHUNK` and `KIMODO_TEXT_RESIDENT_LIMIT_MIB`). The library
itself reads no environment variables.
The demo reuses one native worker while the selected motion model and text
quantization remain unchanged, preserving both weight sets across requests.
Profiling controls, measurements, and the next optimization targets are in
[`docs/PROFILING.md`](docs/PROFILING.md).

Quantisation comparisons produced by the workflow in
[`docs/QUANTIZATION.md`](docs/QUANTIZATION.md) can be opened at
`http://localhost:8094/compare`. Supply their parent directory with the demo's
`--comparisons` option. The comparison view overlays every variant on a shared
timeline and provides world-space and root-position-aligned modes.

## Weights

Ready-to-run native GGML weights are published under the Hugging Face
`LocalAI-io` organisation (not GitHub's `localai-org`). The reusable
[Llama-3-Kimodo-GGML](https://huggingface.co/LocalAI-io/Llama-3-Kimodo-GGML)
text encoder is separate from the four redistributable motion repositories,
each of which preserves a one-to-one relationship to its NVIDIA upstream:

- [Kimodo-SOMA-RP-v1.1-GGML](https://huggingface.co/LocalAI-io/Kimodo-SOMA-RP-v1.1-GGML)
- [Kimodo-SOMA-SEED-v1.1-GGML](https://huggingface.co/LocalAI-io/Kimodo-SOMA-SEED-v1.1-GGML)
- [Kimodo-G1-RP-v1-GGML](https://huggingface.co/LocalAI-io/Kimodo-G1-RP-v1-GGML)
- [Kimodo-G1-SEED-v1-GGML](https://huggingface.co/LocalAI-io/Kimodo-G1-SEED-v1-GGML)

Download one or repeat `--model` to install several:

```sh
scripts/download_gguf_weights.sh --output "$PWD" \
  --model soma-rp-v1.1 --model g1-rp-v1
```

Q8_0 is the default text encoder. Pass `--text-quantization` with `bf16`,
`q8_0`, `q6_k`, `q5_k`, `q4_k`, or `q4_k_m` to install another level. Each
level is one weight GGUF and all levels share `tokenizer.gguf`; the runtime can
still selectively stream layer tensors from the monolith. The installer
verifies each published manifest and SHA-256 hashes. Use
`--motion-only` when supplying a precomputed 4096-float LLM2Vec embedding.
SMPL-X RP is deliberately absent from the published-weight installer: its
internal-R&D licence prohibits distributing derivative models, so it must be
converted locally after the user obtains the upstream checkpoint under its
gated terms.

The text bundle includes converted Meta Llama 3 material and retains its
separate terms. Review every selected model card before downloading or
redistributing.

## License

The C++ port and its original tooling are licensed under Apache-2.0; see
[LICENSE](LICENSE). GGML and the model weights retain their respective
licences.

| Motion checkpoint | Upstream terms | Commercial use |
| --- | --- | --- |
| Kimodo-SMPLX-RP-v1 | [NVIDIA Internal Scientific Research and Development Model License](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-internal-scientific-research-and-development-model-license/) | No; internal, non-production R&D only; derivative model redistribution is prohibited |
| SOMA RP/SEED v1.1 | [NVIDIA Open Model License](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-open-model-license/) | Permitted by the model licence |
| G1 RP/SEED v1 | [NVIDIA Open Model License](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-open-model-license/) | Permitted by the model licence |

The SMPL-X warning is about NVIDIA's trained Kimodo checkpoint, not the mere
fact that its output uses an SMPL-X-shaped 22-joint hierarchy. Converting that
checkpoint to GGUF is a new runtime representation of the same weights and does
not replace its licence. Skeleton names, parent links, and the Apache-2.0 port
source do not by themselves make the SOMA or G1 checkpoints non-commercial.
The SMPL-X Hugging Face metadata, model card, and access terms identify the
internal-R&D licence; treat those restrictive terms as controlling even though
an apparently inconsistent `LICENSE` file has also appeared in that upstream
repository.

### Regenerating the bundle

This is only needed to reproduce a conversion. The SMPL-X checkpoint and Llama
base model are gated. After accepting their Hugging Face licences and
authenticating, download the exact revisions and hash manifests with:

```sh
nix develop path:. --command hf auth login
scripts/download_weights.sh --output "$PWD/models" --with-text \
  --model smplx-rp-v1 --model soma-rp-v1.1 --model soma-seed-v1.1 \
  --model g1-rp-v1 --model g1-seed-v1
```

Convert the local LLM2Vec model to the native component bundle with:

```sh
nix develop path:. --command scripts/convert_llm2vec_bundle.sh \
  "$PWD/models/llama3-8b-instruct-base" "$PWD/generated/llm2vec-text-bundle"
```

Quantize and pack the release variants as described in
[`docs/QUANTIZATION.md`](docs/QUANTIZATION.md). `kmd-pack-text` streams tensor
payloads between files, so creating the BF16 monolith does not require enough
RAM to hold the complete encoder.

Validate a prospective release without network access, then explicitly upload
it from an account allowed to publish to `LocalAI-io`:

```sh
nix develop path:. --command python scripts/publish_gguf.py --component motion \
  --motion-model soma-rp-v1.1
nix develop path:. --command python scripts/publish_gguf.py --component motion \
  --motion-model soma-rp-v1.1 --upload --confirm-upstream-licences
nix develop path:. --command python scripts/publish_gguf.py --component text \
  --upload --confirm-upstream-licences
```
