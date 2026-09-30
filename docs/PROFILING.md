# Inference profiling

Set `KIMODO_PROFILE=1` to print phase timings for text-component uploads,
embedding, transformer execution, motion-weight upload, graph construction,
graph allocation, host/device transfers, GPU compute, diffusion steps, and
decode. GGML's lower-level Vulkan counters remain available through
`GGML_VK_PERF_LOGGER=1`. These variables, like the other `KIMODO_*` ones
below, are read by the command-line tools, which pass them to the library as
options; the library reads none itself. A host of the shared library gets the
same timings at `KIMODO_LOG_DEBUG` through `kimodo_set_log_callback`.

The measurements below were taken on an NVIDIA GeForce RTX 5070 Ti (16 GiB)
with the 19-token jump exemplar, the Q8_0 text bundle, and a 60-frame SOMA
SEED sample. Times vary with clocks and shader-cache state.

| Path | Before | Current | Change |
| --- | ---: | ---: | ---: |
| Repeated Q8 text encode (isolated) | 1,253 ms | 60 ms | about 21x faster |
| 8-frame motion diffusion step | 53.7 ms | 25.5 ms | 53% faster |
| 150-frame, 100-step motion sample | 14.49 s | 4.55 s | 3.18x faster |
| Motion graph calls per step | 36 | 8 | 4.5x fewer |

The Q8 encoder's one-time upload takes about 1.1–1.2 seconds. The live worker
keeps that encoder and the selected motion checkpoint in VRAM, so requests
using the same model/quantization avoid subsequent weight uploads. On this
workstation the resulting native worker occupies about 8,894 MiB of VRAM.
Changing either selector replaces the worker and therefore incurs a new cold
load.

The BF16 bundle is 14,481 MiB before motion weights and inference buffers. It
cannot safely remain resident with the motion model on a 16 GiB GPU. The demo
therefore applies `KIMODO_TEXT_RESIDENT_LIMIT_MIB=10000`: Q8 and smaller
bundles stay resident, while BF16 streams eight-layer groups. The measured BF16
reference encode was 5.79 seconds in this safe mode.

## Operation breakdown

For a warm Q8 text encode, Vulkan timestamps reported about 49 ms of GPU work.
The main quantized FFN projections dominate. The rank-16 F32 LoRA down
projections are the next largest group and run at low utilization because of
their narrow shape. Sharing the attention normalization for Q, K, and V removes
two redundant RMS-normalization subgraphs per layer. Packing the Q/K/V LoRA A
projections into one rank-48 multiplication, and gate/up into one rank-32
multiplication, reduces a warm isolated encode from about 63.7 ms to 60.0 ms.
The resulting embedding is byte-identical. Set `KIMODO_TEXT_PACKED_LORA=0` to
select the reference graph.

Before motion-layer grouping, one 60-frame diffusion step spent approximately:

- 38.5 ms in `ggml_backend_graph_compute`
- 8.4 ms allocating graph buffers
- 3.4 ms uploading inputs
- 5.2 ms downloading intermediate outputs

Packed four-dimensional attention replaces the per-head branches, removing
most of their small launches and materializations. Executing eight transformer
layers per graph then reduces each root or body transformer to two layer
graphs. The compute allocator and its scratch buffer are retained with the
resident motion weights instead of being recreated for every graph. The latest
batch/frame shape also retains its eight graph topologies, dedicated scratch
buffers, position encoding, and expanded text condition. Warm executions do no
graph construction or allocation and avoid regenerating invariant host inputs.
Set `KIMODO_MOTION_GRAPH_CACHE=0` to disable this cache, or set
`KIMODO_MOTION_PACKED_ATTENTION=0` and `KIMODO_MOTION_LAYER_CHUNK=1` to select
the original reference layout and grouping.

The packed path was compared against that reference for an 8-frame step and a
150-frame, 100-step in-distribution sample. The sampled state, root positions,
and local rotations were byte-identical. The longer sample fell from 14.49 to
4.55 seconds. Disabling only the graph cache raises it to 4.94 seconds. The
cache adds 96.4 MiB of GPU scratch and about 8 MiB of resident host memory, and
is bounded to the most-recent shape. Cached layer groups of 16 were also tested
at 4.63 seconds, so eight remains the default.

The next optimization targets are:

1. Text LoRA kernel fusion. Packing reduces the number of narrow A projections,
   but the LoRA B result is still a separate F32 matrix multiplication and add
   for every base projection. A fused Vulkan operation would avoid those
   intermediate tensors.
2. Conversion-time packing. The LoRA A tensors are concatenated while each
   execution graph is built. Storing prepacked QKV and gate/up tensors in the
   bundle would reduce graph construction and metadata work.
3. Motion-kernel fusion. GPU compute is now about 38 ms of a 42 ms warm
   diffusion step. Fusing normalization/residual and feed-forward operations
   is the largest remaining single-request opportunity, but requires GGML or
   Vulkan kernel work and strict F32 parity validation.
4. Caller-controlled request batching. Independent prompts with matching
   shapes can share GPU launches when the caller can guarantee a full batch;
   this trades latency and activation memory for throughput.

For repeatable text measurements, `kmd-encode` accepts an optional repetition
count and reports every iteration:

```sh
KIMODO_BACKEND=vulkan KIMODO_TEXT_LAYER_CHUNK=32 KIMODO_PROFILE=1 \
  build/debug/kmd-encode generated/llm2vec-text-q8_0 \
  benchmarks/quantization/prompts/seed-train/03-jump.txt /tmp/jump.f32 3
```
