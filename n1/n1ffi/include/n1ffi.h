/* n1ffi: the live UE3 scene renderer (Rust, ash) behind a C ABI.
 *
 * The host owns the Vulkan instance, device, queue and command buffers. At the point where the game's 3D scene
 * would be drawn it calls n1_record with its open command buffer (outside any render pass / dynamic rendering), a
 * colour target, the frame's camera and the primitives. n1ffi records texture uploads and the scene into that
 * command buffer and returns; the host submits as usual. n1ffi never submits, waits or touches a queue.
 *
 * Device requirements: Vulkan 1.3 dynamic rendering (vkCmdBeginRendering); BC texture formats where supported
 * (checked per format; textures in unsupported formats draw flat grey).
 *
 * Conventions: UE3 world space (left-handed, Z up, cm) and UE3 FMatrix values as stored (16 floats, row-major, row
 * vectors). Reverse-Z depth in n1ffi's own depth buffer. The scene is drawn into n1ffi's own linear RGBA16F target
 * and resolved (post or copy) into the view rectangle of the colour target, which is written, never read.
 *
 * Threads: every call may come from any thread; calls serialise on one internal mutex.
 */
#ifndef N1FFI_H
#define N1FFI_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N1_NONE 0xFFFFFFFFu

typedef struct N1Renderer N1Renderer;

typedef struct N1Init {
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkFormat color_format;   /* format of the colour targets; UNORM gets sRGB encoding in the shader */
  const char* cooked_dir;  /* game/C1Game/CookedXenon */
  const char* cache_dir;   /* writable: asset index and extracted meshes/textures/light */
  uint32_t loader_threads; /* background loaders; 0 = load synchronously inside n1_record (stalls) */
} N1Init;

typedef struct N1Camera {
  float view[16]; /* FSceneView ViewMatrix */
  float proj[16]; /* FSceneView ProjectionMatrix (x/y scales used) */
  float location[3];
  float near_plane; /* 5 in this game; <= 0 means 5 */
} N1Camera;

typedef struct N1Target {
  VkImageView view;
  VkImageLayout layout; /* the image's current layout, kept (e.g. VK_IMAGE_LAYOUT_GENERAL) */
  uint32_t width, height;
  int32_t x, y; /* view rectangle, pixels */
  uint32_t w, h;
  uint32_t clear; /* non-zero: clear the rectangle to clear_color first */
  float clear_color[4];
} N1Target;

typedef struct N1Instance {
  float transform[16]; /* LocalToWorld */
  uint32_t mesh;       /* n1_mesh handle */
  uint32_t light;      /* n1_light handle or N1_NONE */
  uint32_t mat_offset; /* range in the frame's material handle array, one per mesh section */
  uint32_t mat_count;
  uint32_t env;        /* n1_env handle (owner's DynamicLightEnvironmentComponent) or N1_NONE */
} N1Instance;

typedef struct N1SkelInstance {
  float transform[16];   /* LocalToWorld */
  uint32_t mesh;         /* n1_skel_mesh handle */
  uint32_t bone_offset;  /* first SpaceBases matrix in the frame's bone array */
  uint32_t bone_count;
  uint32_t mat_offset;   /* range in the frame's material handles, per mesh material slot */
  uint32_t mat_count;
  float bounds[4];       /* world bounding sphere: centre, radius */
  uint32_t env;          /* n1_env handle (owner's DynamicLightEnvironmentComponent) or N1_NONE */
  uint32_t lod;          /* B-265: PredictedLODLevel. Bones outside that LOD's RequiredBones are stale in SpaceBases and
                            are rebuilt from the reference pose (0: all bones as given) */
} N1SkelInstance;

/* B-186 particles. One emitter instance (sprite or mesh) and its live particles. Rows are `row_floats` floats each,
 * `count` of them from float `row_offset` of the rows array: [location 3, old_location 3, size 3, rotation, colour 4,
 * relative_time, payload...] in UE3 units, as FBaseParticle stores them. Payload: for sub-UV sprites the frame data
 * [ImageH, ImageV, Interp, Image2H, Image2V] (5 floats; a host that can't locate it may pass all the module data after
 * the base particle and n1 searches it), for mesh particles their rotation (3 floats). */
typedef struct N1Emitter {
  uint32_t kind;       /* 0 sprite, 1 mesh */
  uint32_t material;   /* n1_pmaterial handle */
  uint32_t mesh;       /* n1_pmesh handle (mesh emitters) or N1_NONE */
  uint32_t align;      /* EParticleScreenAlignment: 0 square, 1 rectangle, 2 velocity */
  uint32_t sub_h, sub_v;
  uint32_t sub_method; /* InterpolationMethod, 0: no sub-UV animation */
  uint32_t local_space;
  uint32_t count, row_offset, row_floats;
  uint32_t lock_axis;  /* EParticleAxisLock of the emitter's OrientationAxisLock module, 0 (EPAL_NONE) without */
  float origin[3];     /* emitter location (UE3), for far-to-near ordering */
  float pad1;          /* 0; B-236 debug: a small integer picks a sprite orientation variant (particles.rs) */
  float transform[16]; /* component LocalToWorld: transforms local-space rows; its scale sizes sprites (zero = scale 1) */
  float sort_origin[4]; /* B-247: the component's Bounds.Origin (UE3) in xyz, w = 1 when set; n1 orders components
                           back to front by its view depth, as UE3 sorts translucent primitives (w = 0: by `origin`) */
} N1Emitter;

typedef struct N1Stats {
  uint32_t instances, culled, draws;
  uint32_t skipped; /* mesh not loaded (yet, or at all) */
  uint64_t triangles;
  float record_ms;
  uint32_t uploads;
  uint32_t meshes_ready, meshes_pending, meshes_missing;
  uint32_t skel_drawn, skel_culled, skel_skipped, skel_meshes_ready; /* skinned primitives this frame */
  uint32_t loading; /* requested from the loader threads, not uploaded yet */
  uint32_t textures, textures_unsupported;
  uint32_t shaders, pipelines, pipelines_pending; /* material shaders/pipelines made; draws waiting on the budget */
  uint32_t dyn_lights, light_envs, feed_frames;   /* feed lighting in use */
  uint32_t env_bound, env_handles;                /* instances lit by an environment this frame; n1_env handles */
  uint32_t particle_emitters, particle_sprites, particle_meshes; /* drawn this frame */
  uint32_t particle_dropped;                      /* sprites + mesh instances dropped by the caps, plus bad rows */
  uint32_t particle_mats_ready, particle_mats_failed;
  uint32_t particle_skipped;                      /* emitters waiting for their material or mesh */
  uint32_t tree_placements, tree_instances, tree_draws;       /* B-187: feed placements; instances and draws this frame */
  uint32_t tree_assets_ready, tree_assets_uncaptured, tree_assets_failed;
  uint32_t tree_wanted;   /* uncaptured assets some placement in the feed needs now */
  uint32_t tree_captured; /* assets captured from the game's draws this run */
  /* W2: shaders and pipelines built since start, by who asked (the startup step, or play); late_* = asked for in play
   * after the startup step was over (what the precompile missed); failures; SPIR-V disk-cache hits and misses; time in
   * vkCreateGraphicsPipelines on all threads (worst one separate); naga time; compile queue depth; draws the render
   * thread skipped / drew with the grey stand-in for want of a pipeline; frames with any skipped. */
  uint32_t shaders_startup, shaders_live, pipelines_startup, pipelines_live;
  uint32_t late_shaders, late_pipelines, build_failures;
  uint32_t spv_hits, spv_misses;
  float pipeline_ms, pipeline_max_ms, naga_ms;
  uint32_t compile_backlog;
  uint32_t partial_frames;
  uint64_t draws_no_pipe, fallback_draws;
  /* B-199 load counters, cumulative: loads first asked for by a draw (lazy_*: what the level preload missed), draws
   * skipped because a preloaded asset wasn't in yet, preload requests, loads applied on the render thread (count, total
   * and worst ms), bytes copied to GPU memory, textures and meshes made, preloaded assets still to come, texture copies
   * waiting for a command buffer and their staging bytes, package scans running. */
  uint64_t lazy_meshes, lazy_materials, lazy_lights, lazy_skel, lazy_particles, lazy_trees;
  uint64_t late_skips, preload_requests, applied;
  float apply_ms, apply_max_ms;
  uint64_t upload_bytes, textures_added, meshes_added;
  uint32_t preload_pending, copies_pending;
  uint64_t staging_bytes;
  uint32_t preload_scanning;
  uint64_t variant_materials; /* run-time material parameter variants of a resident base (not lazy) */
  /* B-228: decals this frame (sent; drawn as receiver pieces), (decal, receiver) meshes cached, this frame's decal work
   * in ms; meshes built and their triangles since start. */
  uint32_t decals, decal_draws, decal_cached;
  float decal_build_ms;
  uint64_t decal_builds, decal_triangles;
  /* B-287: level-change eviction, cumulative: boundaries seen, sweeps run; given back (static and skinned meshes,
   * textures, materials, baked lights, GPU bytes); textures a sweep couldn't free yet; CPU copies still queued to
   * drop; the last and longest sweep (ms). */
  uint64_t evict_boundaries, evict_sweeps, meshes_freed, skin_meshes_freed, textures_freed, materials_freed,
      lights_freed, bytes_freed, textures_kept;
  uint32_t forgets_pending;
  float evict_last_ms, evict_max_ms;
} N1Stats;

/* NULL on failure; see n1_last_error. */
N1Renderer* n1_create(const N1Init* init);
/* The GPU must be done with everything recorded. */
void n1_destroy(N1Renderer* r);

/* W2: the startup step. Builds every shader module and pipeline the disc can ask for, on worker threads, so that
 * play compiles nothing: from the previous boot's manifest in <cache_dir>/n1-gpu when it is still valid (reads no
 * packages), else by enumerating the disc's materials and particle emitters. Returns at once (0, or -1 for a bad
 * handle); calling it again does nothing. Needs the same Vulkan device the renderer draws with; the host keeps
 * submitting frames while it runs (draws whose pipeline isn't ready use a grey stand-in or wait). */
int32_t n1_precompile_start(const N1Renderer* r);
/* 0 not started, 1 running, 2 done. done/total count work items (they restart when the step passes from its first
 * phase to its second); either pointer may be NULL. */
uint32_t n1_precompile_progress(const N1Renderer* r, uint32_t* done, uint32_t* total);
const char* n1_last_error(void);

/* Stable handles by path; no I/O, never fail. Cache them per component. */
uint32_t n1_mesh(N1Renderer* r, const char* static_mesh_path);
uint32_t n1_material(N1Renderer* r, const char* material_path); /* "" or NULL: N1_NONE */
uint32_t n1_light(N1Renderer* r, const char* component_path);
void n1_set_ambient(N1Renderer* r, float ambient); /* no-op since N12: ambient comes from the feed */
/* Debug A/B switches, next frame on (no new feed needed). Bit 0 (B-230): level dynamic lights in the plain
   LightColor / 255 * Brightness colour, not UE3's pow(LightColor / 255, 2.2) * Brightness. 0: none. */
void n1_set_debug_bits(N1Renderer* r, uint32_t bits);

/* N12 lighting. n1_env: handle for a DynamicLightEnvironmentComponent by full path (stable, no I/O).
 * n1_set_feed: the frame's lights/fog/post as the scene feed's JSON ({"lights":[..],"fog":[..],"post":{..}});
 * sun, sky, height fog, post curve, dynamic lights and light environments follow it until the next call.
 * Returns 0, or -1 with n1_last_error set.
 * Low-end knobs (environment, read at n1_create): BOTS_N1_DYN=0 (no level dynamic lights), BOTS_N1_ENV=0 (no
 * per-object light environments), BOTS_N1_POST=0 (no bloom/grade: plain copy), BOTS_N1_FEATURES (bits: 1 lightmaps,
 * 2 normal maps, 4 specular, 8 fog, 16 dynamic), BOTS_N1_PIPE_BUDGET (pipelines made per frame), BOTS_N1_MAXTEX
 * (largest texture edge), BOTS_N1_ENVP / BOTS_N1_POSTP (calibration overrides). */
uint32_t n1_env(N1Renderer* r, const char* light_environment_path);
int32_t n1_set_feed(N1Renderer* r, const char* json, uint32_t len);
/* The game's world time in seconds (ViewFamily CurrentWorldTime) for materials' and particles' Time: materials compare
 * it with times the game wrote (a tracer's EffectStartTime). Until it is called, n1 uses seconds since it started. */
void n1_set_time(N1Renderer* r, float seconds);
/* B-240: the view's screen fade (UE3 camera and cinematic fades), as the port's composite applies it:
 * out = pow(saturate(c * scale), 1 / 2.2) + overlay, where scale = ColorScale * (1 - OverlayColor.a) (the composite's
 * c5.rgb) and overlay = OverlayColor.rgb * OverlayColor.a. Kept until the next call; null pointers reset to no fade. */
void n1_set_fade(N1Renderer* r, const float* scale, const float* overlay);

/* B-188: the same lighting feed as fixed-layout binary records (no JSON text to build or parse). Layouts match
 * lightfeed::bin (Rust) and HybridLight/HybridFog/HybridPost in the host's hybrid_state.h; sizes are asserted on each
 * side. Light colours are LightColor / 255 * Brightness; the post chain is raw numbers (NaN = property absent).
 * A feed without N1_FEED_FOG_POST keeps the last fog and post. n1_set_feed_bin: returns 0 or -1 (n1_last_error).
 * n1_check_feed (debugging): compares a JSON feed with the binary one built from the same state; returns the number of
 * differences (the first few in n1_last_error) or -1. */
#define N1_FEED_VERSION 3u /* 3 (B-266): param_count + params, the post materials' scalar parameters */
#define N1_FEED_FOG_POST 1u
typedef struct N1FeedLight {
  uint32_t kind;  /* 0 point, 1 spot, 2 directional, 3 spherical harmonic, 4 sky */
  uint32_t flags; /* 1 enabled, 2 baked, 4 casts dynamic shadows, 8 lights static, 16 lights dynamic,
                   32 CastShadows, 64 LightShadowMode == LightShadow_Modulate, 128 sky light: raw LightColor / 255 in pos
                   and raw LowerColor / 255 in axis (B-249) */
  uint32_t env;   /* index in the feed's path table (DynamicLightEnvironmentComponent) or N1_NONE */
  float radius, falloff, inner_deg, outer_deg;
  float pos[3];   /* LightToWorld translation, UE3 axes */
  float axis[3];  /* LightToWorld Z row */
  float color[3];
  float lower[3];
  int32_t sh;     /* index of 27 floats (R, G, B x 9) in the feed's sh array, or -1 */
  float mod_shadow[3]; /* ModShadowColor R, G, B (linear; 1 when absent) */
  float mod_fade_exp, shadow_falloff_exp, min_shadow_falloff_radius; /* defaults 3, 2, 0 */
} N1FeedLight; /* 104 bytes */
typedef struct N1FeedFog {
  uint32_t present;
  float color[3];
  float light_color[3]; /* raw LightColor / 255, no brightness */
  float density, height, start, extinction;
} N1FeedFog;
typedef struct N1FeedPost {
  uint32_t present, flags, material_effect; /* material_effect: path table index or N1_NONE; flags bits 24..27: how
                                              many MaterialEffect paths start there (0 = 1) */
  float uber_bloom_scale, uber_bloom_threshold, ls_bloom_scale, ls_bloom_threshold;
  float balance[9];
  float desaturation, shadows[3], midtones[3], highlights[3];
  float tone_min, tone_max, tone_scale;
  float god[9];
  float uber_dof[7]; /* BloomKernelSize, BlurKernelSize, FalloffExponent, MaxNear/FarBlurAmount, FocusInnerRadius, FocusDistance */
} N1FeedPost;
/* B-266: one scalar parameter of a post material (Gangsta Fire's PARAMAmount, ...), live from the game. */
typedef struct N1FeedParam {
  uint32_t material; /* index among the post's MaterialEffect paths (0 = the one at post.material_effect) */
  uint32_t name;     /* path table index of the parameter's name */
  float value;
} N1FeedParam; /* 12 bytes */
typedef struct N1FeedBin {
  uint32_t version, flags, light_count, path_count, sh_count;
  uint32_t param_count; /* version 3: entries at `params`, sent with N1_FEED_FOG_POST (was `reserved`) */
  const N1FeedLight* lights;
  const char* const* paths;
  const float* sh;
  N1FeedFog fog;
  N1FeedPost post;
  const N1FeedParam* params; /* version 3 */
} N1FeedBin;
int32_t n1_set_feed_bin(N1Renderer* r, const N1FeedBin* feed);
/* B-248: decodes the colour grades of these MaterialEffect paths on a background thread (the same paths a feed with
 * N1_FEED_FOG_POST names in `paths`), so the feed's first n1_set_feed_bin finds them cached instead of opening the
 * package on the render thread (about 47 ms on the Mac). Cheap for paths already known. Returns 1 if a thread was
 * started, 0 if nothing was left to do, -1 on a bad renderer. */
int32_t n1_prewarm_grade(N1Renderer* r, const char* const* paths, uint32_t count);
int32_t n1_check_feed(const char* json, uint32_t len, const N1FeedBin* feed);

/* Skinned meshes (N7): handle by SkeletalMesh path, and the next n1_record's skinned primitives (copied, used once).
 * bones: SpaceBases, 16 floats per bone (UE3 row-major, component space; NaN = not set by the game, rebuilt from
 * the reference pose). */
uint32_t n1_skel_mesh(N1Renderer* r, const char* skeletal_mesh_path);
void n1_set_skeletal(N1Renderer* r, const N1SkelInstance* instances, uint32_t count, const float* bones,
                     uint32_t bone_floats, const uint32_t* materials, uint32_t material_count);

/* B-186 particles. Handles by path (no I/O; the material shader is built from the package graph on a loader thread
 * and an emitter is skipped until its material, and mesh, are ready). n1_set_particles: the next n1_record's emitters
 * (copied, used once) and the float rows they index. Sprites and mesh particles draw after the scene geometry, depth
 * tested (soft against the scene depth), before post.
 * Low-end knobs (environment, read at n1_create): BOTS_N1_PARTICLES=0 (no particles), BOTS_N1_PARTICLE_CAP (most
 * sprites per frame, default 2048; the farthest emitters drop first), BOTS_N1_PARTICLE_LOG (loader diagnostics). */
uint32_t n1_pmaterial(N1Renderer* r, const char* material_path);
uint32_t n1_pmesh(N1Renderer* r, const char* static_mesh_path);
void n1_set_particles(N1Renderer* r, const N1Emitter* emitters, uint32_t count, const float* rows,
                      uint32_t row_floats_total);

/* B-187 SpeedTree trees. Handles by path (no I/O). n1_set_trees: the next n1_record's placements (copied, used once).
 * An asset's geometry is not in the disc's packages (SpeedTreeRT builds it at load), so it is captured once per
 * machine: while the game draws a tree, the host hands that draw's vertex shader constants and streams over
 * (n1_tree_probe / n1_tree_ucode / n1_tree_capture) and n1ffi runs the game's own vertex shader on the CPU. The result
 * is cached in <cache_dir>/trees/ (generated on the player's machine, never shipped). n1_tree_end_frame, once per
 * rendered frame, returns how many frames the host should let the game draw its own scene for when a capture starts.
 * Knobs (environment, read at n1_create): BOTS_N1_TREES=off|low|medium|high[,dist=N,leaves=F,nocull] (default high),
 * BOTS_N1_TREE_CAPTURE=0 (never capture), BOTS_N1_TREE_VS=hash:branch|leafcard|leafmesh,... (more tree vertex
 * shaders), BOTS_N1_TREE_LOG (diagnostics). */
typedef struct N1Tree {
  float transform[16]; /* component LocalToWorld (UE3, row-major, scale + translation, no rotation) */
  uint32_t component;  /* n1_tree_component handle */
  uint32_t asset;      /* n1_tree_asset handle */
  int32_t rotation[3]; /* actor rotation (pitch, yaw, roll), UE3 units */
  uint32_t pad;
} N1Tree;

typedef struct N1TreeDraw {
  uint64_t vs_hash;
  const float* consts;        /* 256 vec4 */
  const uint8_t* stream[4];   /* vertex streams in the vertex shader's binding order, guest byte order */
  uint32_t stream_bytes[4], stream_stride[4], stream_addr[4];
  uint32_t stream_count, pad;
  const uint32_t* indices;    /* as fetched, host order */
  uint32_t index_count, index_offset;
  uint32_t tex_base[4];       /* pixel shader texture fetch constant word 1 */
  uint32_t tex_count;
  uint32_t fetch_consts;      /* vertex fetch constant of stream 0 (low byte) and stream 1 (next byte): the shader's vfN */
} N1TreeDraw;

uint32_t n1_tree_component(N1Renderer* r, const char* component_path);
uint32_t n1_tree_asset(N1Renderer* r, const char* asset_path);
void n1_set_trees(N1Renderer* r, const N1Tree* trees, uint32_t count);
uint32_t n1_tree_probe(N1Renderer* r, uint64_t vs_hash, const float* consts, uint32_t index_count); /* bit0 capture, bit1 ucode */
void n1_tree_ucode(N1Renderer* r, uint64_t vs_hash, const char* text);
void n1_tree_capture(N1Renderer* r, const N1TreeDraw* draw);
uint32_t n1_tree_end_frame(N1Renderer* r);

/* B-228 decals: the next n1_record's attached DecalComponents (copied, used once). n1 clips each receiver's triangles
 * against the decal's box once (cached until the decal or the receiver changes) and draws the pieces with the decal's
 * material after the receivers. `receivers`: indices into that n1_record's instances (DecalReceivers that are drawn).
 * Knobs (environment, read at n1_create): BOTS_N1_DECALS=0 (none), BOTS_N1_DECAL_MS (build time per frame, default 1),
 * BOTS_N1_DECAL_BIAS (units off the surface, default 0.5), BOTS_N1_DECAL_UV (+u / +v: texture axis signs). */
typedef struct N1Decal {
  uint32_t material;        /* n1_material handle (DecalMaterial, run-time parameters as for instances) */
  uint32_t id;              /* the host's identity for the component; with the fields below, the cache key */
  uint32_t receiver_offset; /* range in the receivers array */
  uint32_t receiver_count;
  float planes[6][4];       /* DecalComponent.Planes (X, Y, Z, W): inside when dot(xyz, p) <= w */
  float location[3];        /* HitLocation */
  float width;
  float tangent[3];         /* HitTangent (DecalRotation applied) */
  float height;
  float binormal[3];        /* HitBinormal */
  float backface_angle;     /* BackfaceAngle: faces with dot(normal, HitNormal) below it are skipped */
  float normal[3];          /* HitNormal */
  uint32_t flags;           /* 1: bProjectOnBackfaces */
  float tile[2], offset[2]; /* TileX, TileY, OffsetX, OffsetY */
} N1Decal; /* 192 bytes */
void n1_set_decals(N1Renderer* r, const N1Decal* decals, uint32_t count, const uint32_t* receivers,
                   uint32_t receiver_count);

/* serial: the host's number for the submission cmd will be part of (increasing); completed: the newest serial known
 * finished on the GPU. Returns 0, or -1 with n1_last_error set. */
/* B-199 level preload. n1_preload_package: start loading everything cooked package <name> (a file name or path; ".xxx"
 * optional) holds, plus a level's baked lights; once per package; 1 when a scan started. n1_upload: on a frame without a
 * scene, apply finished loads for up to budget_us and record their texture copies into cmd (same serial rules as
 * n1_record, which may share the serial); fills only the load counters of stats; returns loads applied or -1.
 * n1_load_stats: the load counters only. */
int32_t n1_preload_package(const N1Renderer* r, const char* name);
int32_t n1_upload(const N1Renderer* r, VkCommandBuffer cmd, uint64_t serial, uint64_t completed, uint32_t budget_us,
                  N1Stats* stats);
void n1_load_stats(const N1Renderer* r, N1Stats* stats);

/* B-287: the game's level just changed. Releases the meshes, textures, materials and baked lights the last few levels
 * didn't use (BOTS_N1_EVICT=0 turns it off; BOTS_N1_EVICT_KEEP is how many levels stay). Returns how many meshes were
 * given back. */
uint32_t n1_level_boundary(const N1Renderer* r);

int32_t n1_record(N1Renderer* r, VkCommandBuffer cmd, const N1Target* target, const N1Camera* camera,
                  const N1Instance* instances, uint32_t count, const uint32_t* materials, uint32_t material_count,
                  uint64_t serial, uint64_t completed, uint32_t upload_budget, N1Stats* stats);

#ifdef __cplusplus
}
#endif
#endif
