/*
 * kimodo-capi-smoke: loads the shared library at run time, as a host
 * application does, and drives it through kimodo_capi.h alone.  C, so the
 * header is proven to be C.
 *
 *   kimodo-capi-smoke --library <lib>
 *       ABI, capabilities, GPUs and limit ceilings; no model.
 *   kimodo-capi-smoke --library <lib> --motion <gguf> --text <gguf> [--frames N --steps N --seed N]
 *       kimodo_model_load and kimodo_generate: the ABI 1 route.
 *   kimodo-capi-smoke --library <lib> --data <folder> [checks...]
 *       kimodo_open on a data folder (default models), then a generation
 *       through kimodo_generate_sequence with progress, and the checks asked
 *       for:
 *         --keyframe       a pose pinned as a keyframe, given as structs and as
 *                          NVIDIA's JSON: the two must agree, and the pose
 *                          must land with post-processing
 *         --constraints F  NVIDIA constraints JSON from file F
 *         --embedding      kimodo_encode_text + kimodo_generate_embedding
 *                          must equal kimodo_generate
 *         --cancel-after N cancel at the Nth diffusion step
 *         --cycles N       open, generate and free N times, printing memory
 *         --dump PREFIX    write the clip to PREFIX.root.f32, PREFIX.rotations.f32
 *   Common: --prompt P (repeat --prompt/--frames for segments), --frames N,
 *   --steps N, --seed N, --motion-name F, --gpu N, --log, --max-frames N.
 *
 * Each stage prints a line; clips end with their shape and a checksum.
 * Exits 0 when every check passed.
 */
#include <kimodo/kimodo_capi.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
typedef HMODULE library_handle;
static library_handle open_library(const char *utf8_path) {
    wchar_t wide[4096];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wide, 4096)) return NULL;
    /* The library's own folder first, as a host loading it by path gets. */
    return LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
static void *find_symbol(library_handle library, const char *name) {
    return (void *)GetProcAddress(library, name);
}
static void close_library(library_handle library) { FreeLibrary(library); }
static const char *default_library = "kimodo.dll";
static double private_mib(void) {
    PROCESS_MEMORY_COUNTERS_EX counters;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof counters)) return -1;
    return (double)counters.PrivateUsage / (1024.0 * 1024.0);
}
#else
#include <dlfcn.h>
typedef void *library_handle;
static library_handle open_library(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *find_symbol(library_handle library, const char *name) { return dlsym(library, name); }
static void close_library(library_handle library) { dlclose(library); }
#if defined(__APPLE__)
static const char *default_library = "libkimodo.dylib";
#else
static const char *default_library = "libkimodo.so";
#endif
static double private_mib(void) { return -1; }
#endif

/* ---- The API, resolved by name ------------------------------------------ */
static struct {
    kimodo_abi_version_fn abi_version;
    kimodo_model_load_fn model_load;
    kimodo_model_free_fn model_free;
    kimodo_model_joints_fn model_joints;
    kimodo_generate_fn generate;
    kimodo_generate_embedding_fn generate_embedding;
    kimodo_motion_free_fn motion_free;
    kimodo_motion_frames_fn motion_frames;
    kimodo_motion_joints_fn motion_joints;
    kimodo_motion_local_rotations_xyzw_fn motion_rotations;
    kimodo_motion_root_positions_fn motion_roots;
    kimodo_get_capabilities_fn get_capabilities;
    kimodo_set_log_callback_fn set_log_callback;
    kimodo_gpu_count_fn gpu_count;
    kimodo_gpu_info_get_fn gpu_info_get;
    kimodo_open_fn open;
    kimodo_list_motion_models_fn list_motion_models;
    kimodo_model_skeleton_fn model_skeleton;
    kimodo_model_joint_name_fn joint_name;
    kimodo_model_joint_parent_fn joint_parent;
    kimodo_model_joint_offset_fn joint_offset;
    kimodo_model_limits_fn model_limits;
    kimodo_get_limit_ceilings_fn get_limit_ceilings;
    kimodo_generation_options_init_fn generation_options_init;
    kimodo_runtime_options_init_fn runtime_options_init;
    kimodo_generate_sequence_fn generate_sequence;
    kimodo_encode_text_fn encode_text;
} api;

static library_handle library;
static int failures = 0;

static void fail(const char *what) {
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

static int resolve(void) {
#define RESOLVE(field, name)                                                  \
    api.field = (name##_fn)find_symbol(library, #name);                       \
    if (!api.field) { fprintf(stderr, "missing export: %s\n", #name); return 0; }
    RESOLVE(abi_version, kimodo_abi_version)
    RESOLVE(model_load, kimodo_model_load)
    RESOLVE(model_free, kimodo_model_free)
    RESOLVE(model_joints, kimodo_model_joints)
    RESOLVE(generate, kimodo_generate)
    RESOLVE(generate_embedding, kimodo_generate_embedding)
    RESOLVE(motion_free, kimodo_motion_free)
    RESOLVE(motion_frames, kimodo_motion_frames)
    RESOLVE(motion_joints, kimodo_motion_joints)
    RESOLVE(motion_rotations, kimodo_motion_local_rotations_xyzw)
    RESOLVE(motion_roots, kimodo_motion_root_positions)
    RESOLVE(get_capabilities, kimodo_get_capabilities)
    RESOLVE(set_log_callback, kimodo_set_log_callback)
    RESOLVE(gpu_count, kimodo_gpu_count)
    RESOLVE(gpu_info_get, kimodo_gpu_info_get)
    RESOLVE(open, kimodo_open)
    RESOLVE(list_motion_models, kimodo_list_motion_models)
    RESOLVE(model_skeleton, kimodo_model_skeleton)
    RESOLVE(joint_name, kimodo_model_joint_name)
    RESOLVE(joint_parent, kimodo_model_joint_parent)
    RESOLVE(joint_offset, kimodo_model_joint_offset)
    RESOLVE(model_limits, kimodo_model_limits)
    RESOLVE(get_limit_ceilings, kimodo_get_limit_ceilings)
    RESOLVE(generation_options_init, kimodo_generation_options_init)
    RESOLVE(runtime_options_init, kimodo_runtime_options_init)
    RESOLVE(generate_sequence, kimodo_generate_sequence)
    RESOLVE(encode_text, kimodo_encode_text)
#undef RESOLVE
    return 1;
}

/* ---- Helpers --------------------------------------------------------------- */
static double now_seconds(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* FNV-1a over the floats' bytes: equal output gives an equal checksum. */
static uint64_t fnv1a(const float *values, size_t count, uint64_t hash) {
    const unsigned char *bytes = (const unsigned char *)values;
    for (size_t i = 0; i < count * sizeof(float); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t checksum(const kimodo_motion *clip) {
    const int frames = api.motion_frames(clip), joints = api.motion_joints(clip);
    uint64_t hash = fnv1a(api.motion_rotations(clip), (size_t)frames * (size_t)joints * 4, 14695981039346656037ULL);
    return fnv1a(api.motion_roots(clip), (size_t)frames * 3, hash);
}

static void describe(const char *label, const kimodo_motion *clip, double seconds) {
    const int frames = api.motion_frames(clip);
    const float *roots = api.motion_roots(clip);
    printf("%s: %d frames x %d joints in %.1f s, root at the end (%.3f, %.3f, %.3f), checksum %016llx\n", label, frames,
           api.motion_joints(clip), seconds, roots[(frames - 1) * 3], roots[(frames - 1) * 3 + 1], roots[(frames - 1) * 3 + 2],
           (unsigned long long)checksum(clip));
}

static void on_log(void *user, kimodo_log_level level, const char *message) {
    static const char *names[] = {"?", "debug", "info", "warning", "error"};
    (void)user;
    printf("  [kimodo %s] %s\n", names[level >= 1 && level <= 4 ? level : 0], message);
}

struct progress_state {
    int cancel_after; /* diffusion steps; 0 never */
    int reports;
    int last_stage;
    uint32_t last_done, last_total;
};

static int on_progress(void *user, kimodo_stage stage, uint32_t done, uint32_t total) {
    struct progress_state *state = (struct progress_state *)user;
    static const char *names[] = {"?", "loading", "encoding text", "sampling"};
    ++state->reports;
    if ((int)stage != state->last_stage || done == total || done % 25 == 0)
        printf("  progress: %s %u/%u\n", names[stage >= 1 && stage <= 3 ? stage : 0], done, total);
    state->last_stage = (int)stage;
    state->last_done = done;
    state->last_total = total;
    return state->cancel_after && stage == KIMODO_STAGE_SAMPLING && (int)done >= state->cancel_after;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, file) != (size_t)size) { free(text); text = NULL; }
    if (text) text[size] = '\0';
    fclose(file);
    return text;
}

static int write_floats(const char *path, const float *values, size_t count) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    const int ok = fwrite(values, sizeof(float), count, file) == count;
    fclose(file);
    return ok;
}

/* ---- Settings -------------------------------------------------------------- */
#define MAX_SEGMENTS 16
static struct {
    const char *library, *motion, *text, *data, *motion_name, *constraints, *dump;
    kimodo_segment segments[MAX_SEGMENTS];
    uint32_t segment_count;
    unsigned long frames, steps, cycles, gpu, max_frames;
    unsigned long long seed;
    int keyframe, embedding, log, cancel_after;
} settings;

static void usage(FILE *out) {
    fprintf(out,
        "Usage: kimodo-capi-smoke [--library <path>]\n"
        "         [--motion <gguf> --text <gguf> | --data <folder> [--motion-name <file>]]\n"
        "         [--prompt <text> [--frames N]]... [--steps N] [--seed N] [--gpu N] [--max-frames N]\n"
        "         [--keyframe] [--embedding] [--constraints <json>] [--cancel-after N] [--cycles N]\n"
        "         [--dump <prefix>] [--log]\n"
        "\n"
        "Loads the Kimodo shared library at run time and checks it through the C\n"
        "API.  See the comment at the top of tests/capi_smoke.c for what each\n"
        "option runs.  The library defaults to %s beside the working folder.\n",
        default_library);
}

/* ---- The generation every data-folder run starts with ----------------------- */
static kimodo_motion *generate_sequence(kimodo_model *model, const kimodo_constraints *constraints,
                                        const char *json, int post_process, int cancel_after, kimodo_status *status_out) {
    kimodo_generation_options options;
    api.generation_options_init(model, &options);
    options.seed = settings.seed;
    options.diffusion_steps = (uint32_t)settings.steps;
    if (post_process >= 0) options.post_process = (uint32_t)post_process;
    kimodo_request request;
    memset(&request, 0, sizeof request);
    request.size = sizeof request;
    request.segments = settings.segments;
    request.segment_count = settings.segment_count;
    request.constraints = constraints;
    request.constraints_json = json;
    struct progress_state progress = {cancel_after, 0, 0, 0, 0};
    request.progress = on_progress;
    request.progress_user_data = &progress;
    kimodo_motion *clip = NULL;
    char error[1024] = {0};
    const kimodo_status status = api.generate_sequence(model, &request, &options, &clip, error, (int)sizeof error);
    if (status_out) *status_out = status;
    if (status != KIMODO_OK) printf("  kimodo_generate_sequence: status %d: %s\n", (int)status, error);
    if (status == KIMODO_OK && progress.last_stage != KIMODO_STAGE_SAMPLING)
        fail("the last progress report was not sampling");
    if (status == KIMODO_OK && progress.last_done != progress.last_total) fail("sampling progress did not reach its total");
    return clip;
}

/* A keyframe: frame `at` of `source` pinned into a new clip, as structs and
 * as NVIDIA's JSON.  Both must give the same clip, and post-processing must
 * land the pose. */
static void keyframe_check(kimodo_model *model, const kimodo_motion *source) {
    const int joints = api.motion_joints(source), frames = api.motion_frames(source);
    const int at = frames * 2 / 3;
    const float *root = api.motion_roots(source) + at * 3;
    const float *pose = api.motion_rotations(source) + (size_t)at * (size_t)joints * 4;
    uint32_t frame = (uint32_t)at;
    kimodo_constraint constraint;
    memset(&constraint, 0, sizeof constraint);
    constraint.size = sizeof constraint;
    constraint.type = KIMODO_CONSTRAINT_FULLBODY;
    constraint.frame_count = 1;
    constraint.frames = &frame;
    constraint.root_positions = root;
    constraint.local_rotations_xyzw = pose;
    kimodo_constraints set = {sizeof set, &constraint, 1};

    /* The same pose in NVIDIA's JSON, digits enough that every float parses
     * back to itself. */
    size_t capacity = 256 + (size_t)joints * 4 * 32, used = 0;
    char *json = (char *)malloc(capacity);
    used += (size_t)snprintf(json + used, capacity - used,
        "[{\"type\": \"fullbody\", \"frame_indices\": [%d], \"root_positions\": [[%.17g, %.17g, %.17g]], \"local_joints_rot_xyzw\": [[",
        at, (double)root[0], (double)root[1], (double)root[2]);
    for (int j = 0; j < joints; ++j)
        used += (size_t)snprintf(json + used, capacity - used, "%s[%.17g, %.17g, %.17g, %.17g]", j ? ", " : "",
                                 (double)pose[j * 4], (double)pose[j * 4 + 1], (double)pose[j * 4 + 2], (double)pose[j * 4 + 3]);
    snprintf(json + used, capacity - used, "]]}]");

    double started = now_seconds();
    kimodo_motion *raw = generate_sequence(model, &set, NULL, 0, 0, NULL);
    if (!raw) { fail("keyframe without post-processing"); free(json); return; }
    describe("keyframe, diffusion only", raw, now_seconds() - started);
    started = now_seconds();
    kimodo_motion *by_struct = generate_sequence(model, &set, NULL, 1, 0, NULL);
    const double struct_seconds = now_seconds() - started;
    started = now_seconds();
    kimodo_motion *by_json = by_struct ? generate_sequence(model, NULL, json, 1, 0, NULL) : NULL;
    const double json_seconds = now_seconds() - started;
    free(json);
    if (!by_struct || !by_json) { fail("keyframe with post-processing"); api.motion_free(raw); api.motion_free(by_struct); return; }
    describe("keyframe as structs, post-processed", by_struct, struct_seconds);
    describe("keyframe as JSON, post-processed", by_json, json_seconds);
    if (checksum(by_struct) != checksum(by_json)) fail("the struct and JSON keyframes gave different clips");
    else printf("keyframe: struct and JSON constraints give the same clip\n");
    /* Where the hips landed at the keyframe. */
    const kimodo_motion *clips[2] = {raw, by_struct};
    const char *labels[2] = {"diffusion only", "post-processed"};
    double landed = 0;
    for (int i = 0; i < 2; ++i) {
        const float *got = api.motion_roots(clips[i]) + at * 3;
        const double dx = got[0] - root[0], dy = got[1] - root[1], dz = got[2] - root[2];
        const double distance = sqrt(dx * dx + dy * dy + dz * dz);
        double worst = 0;
        const float *q = api.motion_rotations(clips[i]) + (size_t)at * (size_t)joints * 4;
        for (int j = 0; j < joints; ++j) {
            /* the angle between the pose's and the clip's joint rotation */
            double dot = fabs((double)q[j * 4] * pose[j * 4] + (double)q[j * 4 + 1] * pose[j * 4 + 1] +
                              (double)q[j * 4 + 2] * pose[j * 4 + 2] + (double)q[j * 4 + 3] * pose[j * 4 + 3]);
            if (dot > 1) dot = 1;
            const double angle = 2 * acos(dot) * 180 / 3.14159265358979;
            if (angle > worst) worst = angle;
        }
        printf("keyframe %s: hips %.1f mm from the target at frame %d, worst joint %.1f degrees off\n", labels[i],
               distance * 1000, at, worst);
        if (i == 1) landed = distance;
    }
    if (landed > 0.05) fail("post-processing left the hips more than 5 cm from the keyframe");
    api.motion_free(raw);
    api.motion_free(by_struct);
    api.motion_free(by_json);
}

/* Callers built against the ABI 2 header: their constraint arrays step by
 * the smaller struct, and their options end before the ABI 3 fields.  Both
 * must give what the ABI 3 structs give. */
struct constraint_v2 {
    uint32_t size; kimodo_constraint_type type; uint32_t end_effectors; uint32_t frame_count; const uint32_t *frames;
    const float *root_positions; const float *local_rotations_xyzw; const float *smooth_root_2d; const float *root_heading;
};
struct generation_options_v2 {
    uint32_t size; uint64_t seed; uint32_t frames; uint32_t diffusion_steps; float text_cfg_weight;
    float constraint_cfg_weight; float first_heading; uint32_t post_process; float root_margin;
};

static void compatibility_check(kimodo_model *model, const kimodo_motion *source) {
    const int joints = api.motion_joints(source), frames = api.motion_frames(source);
    const int at = frames / 2;
    uint32_t pose_frame = (uint32_t)at, waypoint_frame = (uint32_t)(frames - 1);
    const float waypoint[2] = {0.25f, 1.5f};
    kimodo_constraint current[2];
    struct constraint_v2 older[2];
    memset(current, 0, sizeof current);
    memset(older, 0, sizeof older);
    for (int i = 0; i < 2; ++i) {
        current[i].size = sizeof current[i];
        older[i].size = sizeof older[i];
        current[i].frame_count = older[i].frame_count = 1;
    }
    current[0].type = older[0].type = KIMODO_CONSTRAINT_FULLBODY;
    current[0].frames = older[0].frames = &pose_frame;
    current[0].root_positions = older[0].root_positions = api.motion_roots(source) + at * 3;
    current[0].local_rotations_xyzw = older[0].local_rotations_xyzw = api.motion_rotations(source) + (size_t)at * (size_t)joints * 4;
    current[1].type = older[1].type = KIMODO_CONSTRAINT_ROOT2D;
    current[1].frames = older[1].frames = &waypoint_frame;
    current[1].smooth_root_2d = older[1].smooth_root_2d = waypoint;
    kimodo_constraints set_current = {sizeof set_current, current, 2};
    kimodo_constraints set_older = {sizeof set_older, (const kimodo_constraint *)older, 2};

    kimodo_generation_options options;
    api.generation_options_init(model, &options);
    options.seed = settings.seed;
    options.diffusion_steps = (uint32_t)settings.steps;
    options.frames = (uint32_t)frames;
    struct generation_options_v2 options_v2;
    memset(&options_v2, 0, sizeof options_v2);
    options_v2.size = sizeof options_v2;
    options_v2.seed = options.seed;
    options_v2.frames = options.frames;
    options_v2.diffusion_steps = options.diffusion_steps;
    options_v2.text_cfg_weight = options.text_cfg_weight;
    options_v2.constraint_cfg_weight = options.constraint_cfg_weight;
    options_v2.first_heading = options.first_heading;
    options_v2.post_process = options.post_process;
    options_v2.root_margin = options.root_margin;

    char error[1024] = {0};
    kimodo_generate_constrained_fn constrained = (kimodo_generate_constrained_fn)find_symbol(library, "kimodo_generate_constrained");
    const char *prompt = settings.segments[0].prompt;
    kimodo_motion *a = constrained(model, prompt, &options, &set_current, error, (int)sizeof error);
    kimodo_motion *b = a ? constrained(model, prompt, (const kimodo_generation_options *)&options_v2, &set_older, error, (int)sizeof error) : NULL;
    if (!a || !b) { printf("  %s\n", error); fail("generation with ABI 2 structs"); }
    else if (checksum(a) != checksum(b)) fail("ABI 2 constraint arrays or options give a different clip");
    else printf("compatibility: ABI 2 constraint arrays and options give the ABI 3 clip (%016llx)\n", (unsigned long long)checksum(a));
    api.motion_free(a);
    api.motion_free(b);
}

static void embedding_check(kimodo_model *model) {
    float embedding[4096];
    char error[1024] = {0};
    const kimodo_status status = api.encode_text(model, settings.segments[0].prompt, embedding, error, (int)sizeof error);
    if (status != KIMODO_OK) { printf("  kimodo_encode_text: %s\n", error); fail("kimodo_encode_text"); return; }
    kimodo_generation_options options;
    api.generation_options_init(model, &options);
    options.seed = settings.seed;
    options.diffusion_steps = (uint32_t)settings.steps;
    options.frames = settings.segments[0].frames;
    options.post_process = 0;
    kimodo_embedding input = {embedding, 4096};
    kimodo_motion *from_embedding = api.generate_embedding(model, &input, &options, error, (int)sizeof error);
    kimodo_motion *from_text = api.generate(model, settings.segments[0].prompt, &options, error, (int)sizeof error);
    if (!from_embedding || !from_text) { printf("  %s\n", error); fail("embedding or text generation"); }
    else if (checksum(from_embedding) != checksum(from_text)) fail("kimodo_encode_text + generate_embedding differs from generate");
    else printf("embedding: kimodo_encode_text + kimodo_generate_embedding equals kimodo_generate (%016llx)\n",
                (unsigned long long)checksum(from_text));
    api.motion_free(from_embedding);
    api.motion_free(from_text);
}

static kimodo_model *open_model(void) {
    kimodo_runtime_options runtime;
    api.runtime_options_init(&runtime);
    runtime.gpu_index = (uint32_t)settings.gpu;
    runtime.max_segment_frames = (uint32_t)settings.max_frames;
    char error[1024] = {0};
    const double started = now_seconds();
    kimodo_model *model = api.open(settings.data, settings.motion_name, NULL, &runtime, error, (int)sizeof error);
    if (!model) { printf("  kimodo_open: %s\n", error); return NULL; }
    printf("opened %s in %.1f s: skeleton %s, %d joints\n", settings.data, now_seconds() - started,
           api.model_skeleton(model), api.model_joints(model));
    return model;
}

static int data_folder_run(void) {
    char models[1024];
    const int listed = api.list_motion_models(settings.data, models, (int)sizeof models);
    if (listed < 0) { fail("kimodo_list_motion_models"); return 1; }
    for (char *c = models; *c; ++c) if (*c == '\n') *c = ' ';
    printf("motion models in the data folder: %s\n", listed ? models : "(none)");

    if (settings.cycles) {
        /* Start and stop: memory must return to where it was. */
        double first_private = 0, first_gpu = 0;
        for (unsigned long cycle = 0; cycle <= settings.cycles; ++cycle) {
            kimodo_gpu_info gpu;
            memset(&gpu, 0, sizeof gpu);
            gpu.size = sizeof gpu;
            const int have_gpu = api.gpu_info_get((int)settings.gpu, &gpu) == 0;
            const double private_now = private_mib(), gpu_free = have_gpu ? (double)gpu.memory_free / (1024.0 * 1024.0) : -1;
            printf("cycle %lu: process private %.0f MiB, GPU free %.0f MiB\n", cycle, private_now, gpu_free);
            if (cycle == 1) { first_private = private_now; first_gpu = gpu_free; }
            if (cycle == settings.cycles) {
                if (settings.cycles >= 2)
                    printf("after %lu stops: private %+.0f MiB, GPU free %+.0f MiB against the first stop\n",
                           settings.cycles - 1, private_now - first_private, gpu_free - first_gpu);
                break;
            }
            kimodo_model *model = open_model();
            if (!model) { fail("kimodo_open"); return 1; }
            double started = now_seconds();
            kimodo_motion *clip = generate_sequence(model, NULL, NULL, -1, 0, NULL);
            if (!clip) fail("generation");
            else describe("  clip", clip, now_seconds() - started);
            api.motion_free(clip);
            api.model_free(model);
        }
        return failures != 0;
    }

    kimodo_model *model = open_model();
    if (!model) { fail("kimodo_open"); return 1; }
    kimodo_limits limits;
    memset(&limits, 0, sizeof limits);
    limits.size = sizeof limits;
    if (api.model_limits(model, &limits) != 0) fail("kimodo_model_limits");
    printf("limits: segments of %u..%u frames at %u fps, up to %u segments, %u steps, %u constraints\n",
           limits.min_segment_frames, limits.max_segment_frames, limits.frame_rate, limits.max_segments,
           limits.max_diffusion_steps, limits.max_constraints);
    float offset[3] = {0, 0, 0};
    const int joints = api.model_joints(model), last = joints - 1;
    api.joint_offset(model, last, offset);
    printf("skeleton: joint 0 %s (parent %d), joint %d %s (parent %d, offset %.3f %.3f %.3f)\n", api.joint_name(model, 0),
           api.joint_parent(model, 0), last, api.joint_name(model, last), api.joint_parent(model, last),
           offset[0], offset[1], offset[2]);
    kimodo_generation_options defaults;
    api.generation_options_init(model, &defaults);
    printf("defaults: %u frames, %u steps, guidance %.1f/%.1f, transition %u, post-processing %s\n", defaults.frames,
           defaults.diffusion_steps, defaults.text_cfg_weight, defaults.constraint_cfg_weight, defaults.transition_frames,
           defaults.post_process ? "on" : "off");

    char *json = NULL;
    if (settings.constraints) {
        json = read_file(settings.constraints);
        if (!json) { fprintf(stderr, "cannot read %s\n", settings.constraints); api.model_free(model); return 1; }
    }
    double started = now_seconds();
    kimodo_motion *clip = generate_sequence(model, NULL, json, -1, 0, NULL);
    free(json);
    if (!clip) fail("generation");
    else {
        describe(settings.constraints ? "clip with constraints" : "clip", clip, now_seconds() - started);
        if (settings.dump) {
            char path[1024];
            snprintf(path, sizeof path, "%s.root.f32", settings.dump);
            const int frames = api.motion_frames(clip);
            int ok = write_floats(path, api.motion_roots(clip), (size_t)frames * 3);
            snprintf(path, sizeof path, "%s.rotations.f32", settings.dump);
            ok = ok && write_floats(path, api.motion_rotations(clip), (size_t)frames * (size_t)api.motion_joints(clip) * 4);
            if (!ok) fail("writing the clip");
        }
    }
    if (clip && settings.keyframe) {
        keyframe_check(model, clip);
        compatibility_check(model, clip);
    }
    api.motion_free(clip);
    if (settings.embedding) embedding_check(model);
    if (settings.cancel_after) {
        kimodo_status status = KIMODO_OK;
        started = now_seconds();
        kimodo_motion *cancelled = generate_sequence(model, NULL, NULL, -1, settings.cancel_after, &status);
        if (cancelled || status != KIMODO_CANCELLED) fail("a cancel was not reported as KIMODO_CANCELLED");
        else printf("cancel: stopped at step %d in %.1f s, KIMODO_CANCELLED\n", settings.cancel_after, now_seconds() - started);
        api.motion_free(cancelled);
    }
    api.model_free(model);
    return failures != 0;
}

/* ---- main ------------------------------------------------------------------ */
static int run(int argc, char **argv) {
    settings.library = default_library;
    settings.steps = 10;
    settings.seed = 1;
    unsigned long frames = 60;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) { usage(stdout); return 0; }
        if (!strcmp(arg, "--keyframe")) { settings.keyframe = 1; continue; }
        if (!strcmp(arg, "--embedding")) { settings.embedding = 1; continue; }
        if (!strcmp(arg, "--log")) { settings.log = 1; continue; }
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (!value) { fprintf(stderr, "%s needs a value\n\n", arg); usage(stderr); return 2; }
        if (!strcmp(arg, "--library")) settings.library = value;
        else if (!strcmp(arg, "--motion")) settings.motion = value;
        else if (!strcmp(arg, "--text")) settings.text = value;
        else if (!strcmp(arg, "--data")) settings.data = value;
        else if (!strcmp(arg, "--motion-name")) settings.motion_name = value;
        else if (!strcmp(arg, "--constraints")) settings.constraints = value;
        else if (!strcmp(arg, "--dump")) settings.dump = value;
        else if (!strcmp(arg, "--prompt")) {
            if (settings.segment_count == MAX_SEGMENTS) { fprintf(stderr, "at most %d prompts\n", MAX_SEGMENTS); return 2; }
            settings.segments[settings.segment_count].prompt = value;
            settings.segments[settings.segment_count++].frames = (uint32_t)frames;
        } else if (!strcmp(arg, "--frames")) {
            frames = strtoul(value, NULL, 10);
            /* After a --prompt, the frames are that segment's. */
            if (settings.segment_count) settings.segments[settings.segment_count - 1].frames = (uint32_t)frames;
        }
        else if (!strcmp(arg, "--steps")) settings.steps = strtoul(value, NULL, 10);
        else if (!strcmp(arg, "--seed")) settings.seed = strtoull(value, NULL, 10);
        else if (!strcmp(arg, "--cycles")) settings.cycles = strtoul(value, NULL, 10);
        else if (!strcmp(arg, "--gpu")) settings.gpu = strtoul(value, NULL, 10);
        else if (!strcmp(arg, "--max-frames")) settings.max_frames = strtoul(value, NULL, 10);
        else if (!strcmp(arg, "--cancel-after")) settings.cancel_after = atoi(value);
        else { fprintf(stderr, "unknown argument: %s\n\n", arg); usage(stderr); return 2; }
        ++i;
    }
    if (!settings.segment_count) {
        settings.segments[0].prompt = "a person walks forward";
        settings.segments[0].frames = (uint32_t)frames;
        settings.segment_count = 1;
    }
    settings.frames = frames;
    if (!settings.motion != !settings.text) { fprintf(stderr, "--motion and --text go together\n"); return 2; }

    library = open_library(settings.library);
    if (!library) { fprintf(stderr, "cannot load %s\n", settings.library); return 1; }
    printf("loaded %s\n", settings.library);
    if (!resolve()) { close_library(library); return 1; }
    const int abi = api.abi_version();
    printf("abi %d (header %d)\n", abi, KIMODO_CAPI_ABI_VERSION);
    if (abi != KIMODO_CAPI_ABI_VERSION) {
        fprintf(stderr, "ABI mismatch: the library is %d, this header %d\n", abi, KIMODO_CAPI_ABI_VERSION);
        close_library(library);
        return 1;
    }
    if (settings.log) api.set_log_callback(on_log, NULL, KIMODO_LOG_INFO);

    kimodo_capabilities capabilities;
    memset(&capabilities, 0, sizeof capabilities);
    capabilities.size = sizeof capabilities;
    if (api.get_capabilities(&capabilities) != 0) fail("kimodo_get_capabilities");
    printf("library %s (commit %s): CPU%s, post-processing %s\n", capabilities.version, capabilities.commit,
           capabilities.devices & (1u << KIMODO_DEVICE_VULKAN) ? " + Vulkan" : "", capabilities.post_processing ? "yes" : "no");
    const int gpus = api.gpu_count();
    for (int index = 0; index < gpus; ++index) {
        kimodo_gpu_info gpu;
        memset(&gpu, 0, sizeof gpu);
        gpu.size = sizeof gpu;
        if (api.gpu_info_get(index, &gpu) != 0) { fail("kimodo_gpu_info_get"); continue; }
        printf("GPU %d: %s, %.1f of %.1f GiB free\n", index, gpu.name, (double)gpu.memory_free / 1073741824.0,
               (double)gpu.memory_total / 1073741824.0);
    }
    kimodo_limits ceilings;
    memset(&ceilings, 0, sizeof ceilings);
    ceilings.size = sizeof ceilings;
    if (api.get_limit_ceilings(&ceilings) != 0) fail("kimodo_get_limit_ceilings");
    printf("ceilings: %u frames per segment, %u segments\n", ceilings.max_segment_frames, ceilings.max_segments);

    int result = 0;
    if (settings.data) {
        result = data_folder_run();
    } else if (settings.motion) {
        /* The ABI 1 route. */
        char error[1024] = {0};
        kimodo_runtime_options runtime;
        api.runtime_options_init(&runtime);
        runtime.gpu_index = (uint32_t)settings.gpu;
        double started = now_seconds();
        kimodo_model *model = api.model_load(settings.motion, settings.text, NULL, &runtime, error, (int)sizeof error);
        if (!model) {
            fprintf(stderr, "kimodo_model_load failed: %s\n", error);
            close_library(library);
            return 1;
        }
        printf("model loaded in %.1f s, %d joints\n", now_seconds() - started, api.model_joints(model));
        kimodo_generation_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.seed = settings.seed;
        options.frames = (uint32_t)settings.frames;
        options.diffusion_steps = (uint32_t)settings.steps;
        options.text_cfg_weight = 2.0f;
        options.constraint_cfg_weight = 2.0f;
        options.root_margin = 0.04f;
        started = now_seconds();
        kimodo_motion *clip = api.generate(model, settings.segments[0].prompt, &options, error, (int)sizeof error);
        if (!clip) {
            fprintf(stderr, "kimodo_generate failed: %s\n", error);
            fail("kimodo_generate");
        } else {
            if (api.motion_frames(clip) != (int)settings.frames) fail("the clip's frame count");
            describe("generated", clip, now_seconds() - started);
            api.motion_free(clip);
        }
        api.model_free(model);
        result = failures != 0;
    }
    if (settings.log) api.set_log_callback(NULL, NULL, KIMODO_LOG_INFO);
    close_library(library);
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    if (!result) printf("ok\n");
    return result;
}

int main(int argc, char **argv) {
#if defined(_WIN32)
    /* The arguments as UTF-8, whatever the code page: the API takes UTF-8
     * paths, and a folder name with an accent must survive the trip. */
    int count = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide) {
        char **utf8 = (char **)calloc((size_t)count + 1, sizeof(char *));
        for (int i = 0; i < count; ++i) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
            utf8[i] = (char *)malloc((size_t)bytes);
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8[i], bytes, NULL, NULL);
        }
        LocalFree(wide);
        SetConsoleOutputCP(CP_UTF8);
        return run(count, utf8);
    }
#endif
    return run(argc, argv);
}
