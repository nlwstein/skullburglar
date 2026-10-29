//! The ash renderer, recorded straight into a command buffer the host owns. No queue, no submission, no semaphores:
//! the host submits its own command buffer and tells us, by serial number, which of its submissions have completed
//! so we can reuse per-frame memory and free garbage.
//!
//! N12: N8's compiled materials (one shader per material graph, `matdef`), directional vertex light and lightmaps
//! (three coefficients), the terrain vertex-colour blend, N10's sun/sky/fog/dynamic lights/light environments, all
//! rendered into a linear RGBA16F target (so blending is linear) and resolved onto the host's target by the game's
//! own post chain (W4: grade, DOF + bloom, gamma 2.2; n1ffi/src/post.wgsl) or a gamma-only copy.
//!
//! Coordinates: the matdef shader works in N8's Y-up world. Meshes stay in UE3 local space; the model matrix is
//! uploaded as M*S and the view-projection as S*VP (S swaps Y and Z), so clip positions are unchanged and shading
//! happens in Y-up. S is a reflection, so the tangent's bitangent sign is negated at upload.
//!
//! Memory: gpu-allocator sub-allocates. Vertex/index buffers, the light word buffer and the per-frame slot buffers
//! are host-visible; textures and render targets are device-local (textures uploaded through staging buffers
//! recorded at the start of the next `record`).

use crate::light::{Globals, Knobs, Lighting};
use crate::shaders;
use crate::store::{NO_SHADER, SceneKey, Store};
use anyhow::{Result, anyhow, bail};
use ash::vk;
use assetdb::{ComponentLight, SkinVertex};
use gpu_allocator::MemoryLocation;
use gpu_allocator::vulkan::{
    Allocation, AllocationCreateDesc, AllocationScheme, Allocator, AllocatorCreateDesc,
};
use matdef::shader::ShaderFlags;
use matdef::{MatDef, Node, TexRef};
use scenekit::{
    Backend, Blend, FrameStats, Instance, MaterialDesc, MeshDesc, NONE, SectionDesc, TexFormat,
    TextureDesc,
};
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

mod gprof;
mod pgpu;
mod shadow;
mod tgpu;

pub const DEPTH_FORMAT: vk::Format = vk::Format::D32_SFLOAT;
pub const HDR_FORMAT: vk::Format = vk::Format::R16G16B16A16_SFLOAT;
const ALIGN: u64 = 256;
const GLOBALS_AT: u64 = 0;
const POST_AT: u64 = 256; // post uniform blocks, 256 bytes apart
// grade, down, blur A h, blur A v, blur B, composite, copy, then (B-266) the linear composite and up to
// `POST_MATS` post-material passes
const POST_MATS: usize = 3;
const POST_PASSES: usize = 8 + POST_MATS;
const DYN_AT: u64 = POST_AT + 256 * POST_PASSES as u64;

/// Texture ids of the built-in 1x1 textures.
pub const TEX_GREY: u32 = 0;
pub const TEX_NORMAL: u32 = 1;
pub const TEX_BLACK: u32 = 2;

struct Buf {
    buffer: vk::Buffer,
    alloc: Option<Allocation>,
    cap: u64,
}

impl Buf {
    fn null() -> Buf {
        Buf {
            buffer: vk::Buffer::null(),
            alloc: None,
            cap: 0,
        }
    }
    fn bytes(&mut self) -> &mut [u8] {
        self.alloc
            .as_mut()
            .and_then(|a| a.mapped_slice_mut())
            .expect("host-visible buffer")
    }
}

enum Garbage {
    Buffer(Buf),
    Image(vk::Image, vk::ImageView, Allocation),
}

struct Tex {
    image: vk::Image,
    view: vk::ImageView,
    alloc: Option<Allocation>,
    /// Two-channel (BC5/DXN) normal map: blue is not stored, the shader rebuilds it.
    bc5: bool,
    /// B-235: uploaded raw for Xenos's gamma curve; materials sampling it wrap the sample in `xe_pwl`.
    gamma: bool,
}

struct GpuMesh {
    vbuf: Buf,
    ibuf: Buf,
}

struct Upload {
    staging: Buf,
    image: vk::Image,
    regions: Vec<vk::BufferImageCopy>,
    mips: u32,
}

/// One render target we own (HDR colour, bloom, depth).
struct Rt {
    image: vk::Image,
    view: vk::ImageView,
    alloc: Allocation,
    width: u32,
    height: u32,
}

struct Slot {
    buf: Buf,
    set: vk::DescriptorSet,
    post_sets: [vk::DescriptorSet; POST_PASSES],
    /// Particle globals + instances, and their descriptor set (B-186).
    pbuf: Buf,
    pset: vk::DescriptorSet,
    serial: u64,
}

/// The host's Vulkan objects (it keeps ownership of all of them).
pub struct VkContext {
    pub get_instance_proc_addr: vk::PFN_vkGetInstanceProcAddr,
    pub instance: vk::Instance,
    pub physical_device: vk::PhysicalDevice,
    pub device: vk::Device,
}

/// Where to draw: the host's colour image view (already in `layout`, with its barriers done by the host) and the
/// view rectangle in it.
#[derive(Clone, Copy)]
pub struct Target {
    pub view: vk::ImageView,
    pub layout: vk::ImageLayout,
    pub width: u32,
    pub height: u32,
    pub rect: vk::Rect2D,
    pub clear: Option<[f32; 4]>,
}

/// Static vertex as the matdef shader reads it (60 bytes).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct Vertex {
    pub pos: [f32; 3],
    pub nrm: [f32; 3],
    pub tan: [f32; 4],
    pub uv0: [f32; 2],
    pub uv1: [f32; 2],
    pub col: [u8; 4],
}

/// matdef `Instance` (144 bytes).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct GpuInstance {
    pub model: [f32; 16],
    pub light_base: i32,
    pub flags: u32,
    pub p1: u32,
    pub p2: u32,
    pub sc: [[f32; 4]; 3],
    pub lm_xf: [f32; 4],
}

/// One skinned primitive's per-frame data, built by `skin.rs`.
#[derive(Clone, Copy, Debug)]
pub struct SkinInst {
    pub bone_base: u32,
    /// Light environment index (N1 env handle already mapped), or NONE.
    pub env: u32,
    /// World bounds, UE3 axes.
    pub bounds: [f32; 4],
}

/// One skinned draw: a section of a skinned mesh (`add_skin_mesh` id) for `SkinFrame::insts[instance]`.
#[derive(Clone, Copy, Debug)]
pub struct SkinDraw {
    pub material: u32,
    pub mesh: u32,
    pub first_index: u32,
    pub index_count: u32,
    pub instance: u32,
}

#[derive(Default)]
pub struct SkinFrame {
    pub insts: Vec<SkinInst>,
    pub bones: Vec<[f32; 16]>,
    pub draws: Vec<SkinDraw>,
}

struct MeshInfo {
    vertex_count: u32,
    sections: Vec<SectionDesc>,
    center: [f32; 3],
    radius: f32,
}

pub struct MatInfo {
    /// Store module id; `NO_SHADER` until the shader is built (it is drawn with the grey stand-in meanwhile).
    shader: u32,
    /// The shader's identity (flags + structure), to find it once built.
    skey: u64,
    set: vk::DescriptorSet,
    _params: Buf,
    /// Texture count of the set's layout (`mat_layout(ntex)`), so a retired entry is only reused with the same one.
    ntex: usize,
    pub blend: Blend,
    pub two_sided: bool,
}

/// B-228: a receiver's baked light as a decal on it sees it (`Renderer::decal_light_source`).
pub enum DecalLightSource<'a> {
    None,
    Words(&'a [u32], [[f32; 4]; 3]),
    Map,
}

enum LightInfo {
    /// Light words for `count` vertices from vertex `base` (three words each), per-coefficient RGB scale.
    Words {
        base: u32,
        count: u32,
        sc: [[f32; 4]; 3],
    },
    Map {
        set: vk::DescriptorSet,
        sc: [[f32; 4]; 3],
        xf: [f32; 4],
    },
}

#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Debug)]
struct Key {
    shader: u32,
    blend: u8,
    two_sided: bool,
    mirrored: bool,
    skin: bool,
}

#[derive(Clone, Copy)]
struct Draw {
    key: Key,
    mat: u32,
    lm: vk::DescriptorSet,
    mesh: u32,
    first_index: u32,
    index_count: u32,
    instance: u32,
    depth: f32,
}

pub(crate) fn blend_of(b: Blend) -> u8 {
    match b {
        Blend::Opaque => 0,
        Blend::Masked => 1,
        Blend::Translucent => 2,
        Blend::Additive => 3,
        Blend::Modulate => 4,
    }
}

fn blended(b: u8) -> bool {
    b >= 2
}

/// W2 measurements: draws that couldn't be made as they should be, cumulative since the renderer was made. (Shaders and
/// pipelines are counted by the store: nothing is compiled on the render thread any more.)
#[derive(Default, Clone, Copy)]
pub struct Ctr {
    /// Draws skipped because neither their pipeline nor the stand-in's was ready; frames with at least one.
    pub draws_no_pipe: u64,
    pub partial_frames: u32,
    /// Draws made with the grey stand-in because their shader or pipeline wasn't ready.
    pub fallback_draws: u64,
    /// B-199: bytes copied into GPU buffers / staging by texture and mesh uploads, and their counts.
    pub upload_bytes: u64,
    pub textures_added: u64,
    pub meshes_added: u64,
}

/// B-264: per-step timers for a slow frame. `BOTS_N1_RECORD_LOG=<ms>` (`1`: 12 ms) logs a one-line breakdown of
/// any `n1_record` / `Renderer::record` call over that many ms; unset, `lap` is one predictable branch and no clock read.
pub struct Laps {
    on: bool,
    last: std::time::Instant,
    steps: Vec<(&'static str, f32)>,
}

/// The `BOTS_N1_RECORD_LOG` threshold in ms, if set (not `0`).
pub fn record_log_ms() -> Option<f32> {
    static MS: std::sync::OnceLock<Option<f32>> = std::sync::OnceLock::new();
    *MS.get_or_init(|| {
        let v = std::env::var("BOTS_N1_RECORD_LOG").ok()?;
        match v.parse::<f32>() {
            Ok(x) if x == 1.0 => Some(12.0),
            Ok(x) if x > 0.0 => Some(x),
            _ => None,
        }
    })
}

impl Laps {
    pub fn new() -> Self {
        Laps { on: record_log_ms().is_some(), last: std::time::Instant::now(), steps: Vec::new() }
    }
    /// Starts a new frame's breakdown.
    #[inline]
    pub fn begin(&mut self) {
        if self.on {
            self.steps.clear();
            self.last = std::time::Instant::now();
        }
    }
    /// Charges the time since the previous lap (or `begin`) to `name`; repeated names add up.
    #[inline]
    pub fn lap(&mut self, name: &'static str) {
        if self.on {
            let now = std::time::Instant::now();
            let ms = now.duration_since(self.last).as_secs_f32() * 1000.0;
            self.last = now;
            match self.steps.iter_mut().find(|s| s.0 == name) {
                Some(s) => s.1 += ms,
                None => self.steps.push((name, ms)),
            }
        }
    }
    /// One line, steps of at least 0.3 ms only, when `total_ms` is over the threshold.
    pub fn report(&self, who: &str, total_ms: f32, extra: &str) {
        if let Some(th) = record_log_ms().filter(|&t| self.on && total_ms > t) {
            let _ = th;
            let mut line = format!("n1ffi: record-log {who} {total_ms:.1} ms {extra}:");
            for (n, ms) in &self.steps {
                if *ms >= 0.3 {
                    line += &format!(" {n}={ms:.1}");
                }
            }
            eprintln!("{line}");
        }
    }
}

impl Default for Laps {
    fn default() -> Self {
        Self::new()
    }
}

pub struct Renderer {
    pub laps: Laps,
    pub ctr: Ctr,
    _instance: ash::Instance,
    device: ash::Device,
    allocator: Option<Allocator>,
    color_format: vk::Format,
    encode_srgb: bool,
    pub knobs: Knobs,
    set0_layout: vk::DescriptorSetLayout,
    lm_layout: vk::DescriptorSetLayout,
    pub store: Arc<Store>,
    /// Scene pipelines the store has finished (a lock-free copy of its table).
    ready: HashMap<SceneKey, vk::Pipeline>,
    unresolved: Vec<u32>,
    /// Materials the resolver released (`Backend::remove_material`): (serial of their last possible use, id). An entry
    /// is reused, set and parameter buffer included, once that serial has completed.
    free_mats: Vec<(u64, u32)>,
    /// B-228 decal geometry: meshes, vertex-light ranges and light entries given back (`free_decal`), each with the
    /// serial of its last possible use; reused once that serial has completed.
    free_meshes: Vec<(u64, u32)>,
    free_words: Vec<(u64, u32, u32)>,
    free_lights: Vec<(u64, u32)>,
    /// Light-word ranges (word offsets) rewritten in place by a reused vertex-light range, copied by `sync_light`.
    light_patches: Vec<(usize, usize)>,
    /// Newest host submission known complete (from `retire`).
    completed: u64,
    miss_logged: HashSet<SceneKey>,
    dyn_cull: bool,
    post_layout: vk::DescriptorSetLayout,
    post_pipe_layout: vk::PipelineLayout,
    post_module: vk::ShaderModule,
    post_pipes: HashMap<(&'static str, i32), vk::Pipeline>,
    /// B-266: post-material textures by path: (texture id, takes Xenos's gamma curve in the shader).
    post_tex: HashMap<String, (u32, bool)>,
    /// B-266: the entry points recorded last frame (logged when the set changes).
    post_mats_last: Vec<&'static str>,
    /// B-267: a wall clock for post materials that animate while the game is paused (the death screen).
    post_real0: std::time::Instant,
    pub pipe_pending: u32,
    sampler: vk::Sampler,
    lm_sampler: vk::Sampler,
    post_sampler: vk::Sampler,
    pools: Vec<vk::DescriptorPool>,
    textures: Vec<Tex>,
    meshes: Vec<GpuMesh>,
    mesh_info: Vec<MeshInfo>,
    skin_meshes: Vec<GpuMesh>,
    pub materials: Vec<MatInfo>,
    lights: Vec<LightInfo>,
    default_lm: vk::DescriptorSet,
    grey_mat: u32,
    light_cpu: Vec<u32>,
    light: Buf,
    light_dirty_from: usize,
    /// B-264: serial of the submission holding the last GPU copy that grew the light buffer; patches wait until it completes.
    light_copy_serial: u64,
    slots: Vec<Slot>,
    hdr: Option<Rt>,
    bloom: Option<[Rt; 3]>,
    /// W4: the graded scene (full res), when the level's MaterialEffect grade isn't the identity.
    graded: Option<Rt>,
    depth: Option<Rt>,
    /// Near plane of the frame's reverse-Z projection (the post chain linearises depth with it).
    pub near: f32,
    pending: Vec<Upload>,
    deferred: Vec<(u64, Garbage)>,
    last_serial: u64,
    bc_ok: [bool; 4],
    pub unsupported_textures: u32,
    /// BC textures decoded on the CPU because the GPU has no BC formats (bcdec.rs).
    pub decoded_textures: u32,
    pub shader_errors: u32,
    pub stats: FrameStats,
    pub lighting: Lighting,
    t_start: std::time::Instant,
    /// The game's world time (n1_set_time), when the host gives it.
    pub world_time: Option<f32>,
    /// B-240: the view's screen fade (n1_set_fade): out = pow(saturate(c * scale), 1 / 2.2) + overlay, as the port's
    /// composite applies it. Identity until the host sets it.
    pub fade: [[f32; 3]; 2],
    /// `n1_set_debug_bits` (debug A/B switches; 0 normally).
    pub debug_bits: u32,
    /// B-244: debug bit 1's one-shot log has been written for the current setting.
    dyn_logged: bool,
    // per frame
    gpu_insts: Vec<GpuInstance>,
    spheres: Vec<([f32; 3], f32)>,
    inst_env: Vec<u32>,
    draws: Vec<Draw>,
    pg: pgpu::PGpu,
    /// B-189: light-environment shadow maps and their depth pipelines.
    shadows: shadow::Shadows,
    /// G6: per-pass GPU timestamps, BOTS_N1_GPU_PROF=1 (`vkr/gprof.rs`).
    gprof: Option<gprof::GpuProf>,
    tg: tgpu::TGpu,
    /// The next `record`'s particles (set by the host wrapper, used once).
    pub pframe: crate::particles::PFrame,
    /// The next `record`'s trees (set by the host wrapper, used once).
    pub tframe: crate::trees::TFrame,
}

const POOL_SETS: u32 = 1024;

fn vk_format(f: TexFormat, srgb: bool) -> vk::Format {
    match (f, srgb) {
        (TexFormat::Bc1, false) => vk::Format::BC1_RGBA_UNORM_BLOCK,
        (TexFormat::Bc1, true) => vk::Format::BC1_RGBA_SRGB_BLOCK,
        (TexFormat::Bc2, false) => vk::Format::BC2_UNORM_BLOCK,
        (TexFormat::Bc2, true) => vk::Format::BC2_SRGB_BLOCK,
        (TexFormat::Bc3, false) => vk::Format::BC3_UNORM_BLOCK,
        (TexFormat::Bc3, true) => vk::Format::BC3_SRGB_BLOCK,
        (TexFormat::Bc5, _) => vk::Format::BC5_UNORM_BLOCK,
        (TexFormat::R8 | TexFormat::Bgra8, false) => vk::Format::B8G8R8A8_UNORM,
        (TexFormat::R8 | TexFormat::Bgra8, true) => vk::Format::B8G8R8A8_SRGB,
    }
}

pub fn is_srgb(f: vk::Format) -> bool {
    matches!(
        f,
        vk::Format::R8G8B8A8_SRGB
            | vk::Format::B8G8R8A8_SRGB
            | vk::Format::A8B8G8R8_SRGB_PACK32
            | vk::Format::R8G8B8_SRGB
            | vk::Format::B8G8R8_SRGB
    )
}

fn align(x: u64) -> u64 {
    (x + ALIGN - 1) & !(ALIGN - 1)
}

/// Row-major UE3 matrix * S (swap columns 1 and 2): local UE3 -> world Y-up.
fn swz_model(m: &[f32; 16]) -> [f32; 16] {
    let mut o = *m;
    for r in 0..4 {
        o.swap(r * 4 + 1, r * 4 + 2);
    }
    o
}

/// S * row-major VP (swap rows 1 and 2): world Y-up -> clip.
fn swz_vp(m: &[f32; 16]) -> [f32; 16] {
    let mut o = *m;
    for c in 0..4 {
        o.swap(4 + c, 8 + c);
    }
    o
}

fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

fn layout_barrier(
    image: vk::Image,
    aspect: vk::ImageAspectFlags,
    old: vk::ImageLayout,
    new: vk::ImageLayout,
    src: vk::AccessFlags,
    dst: vk::AccessFlags,
) -> vk::ImageMemoryBarrier<'static> {
    vk::ImageMemoryBarrier::default()
        .src_access_mask(src)
        .dst_access_mask(dst)
        .old_layout(old)
        .new_layout(new)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(image)
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: aspect,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        })
}

impl Renderer {
    /// Material and particle time: the game's world time when given, else seconds since start.
    pub fn time(&self) -> f32 {
        self.world_time.unwrap_or_else(|| self.t_start.elapsed().as_secs_f32())
    }

    /// # Safety
    /// The handles must be valid and outlive the renderer; the device must have Vulkan 1.3's dynamic rendering.
    pub unsafe fn new(
        ctx: &VkContext,
        color_format: vk::Format,
        cache_dir: Option<std::path::PathBuf>,
    ) -> Result<Self> {
        unsafe {
            let static_fn = ash::StaticFn {
                get_instance_proc_addr: ctx.get_instance_proc_addr,
            };
            let entry = ash::Entry::from_static_fn(static_fn);
            let instance = ash::Instance::load(entry.static_fn(), ctx.instance);
            let device = ash::Device::load(instance.fp_v1_0(), ctx.device);
            let allocator = Allocator::new(&AllocatorCreateDesc {
                instance: instance.clone(),
                device: device.clone(),
                physical_device: ctx.physical_device,
                debug_settings: Default::default(),
                buffer_device_address: false,
                allocation_sizes: Default::default(),
            })
            .map_err(|e| anyhow!("gpu-allocator: {e}"))?;
            let need = vk::FormatFeatureFlags::SAMPLED_IMAGE
                | vk::FormatFeatureFlags::SAMPLED_IMAGE_FILTER_LINEAR;
            let ok = |f| {
                instance
                    .get_physical_device_format_properties(ctx.physical_device, f)
                    .optimal_tiling_features
                    .contains(need)
            };
            let bc_ok = [
                ok(vk::Format::BC1_RGBA_SRGB_BLOCK),
                ok(vk::Format::BC2_SRGB_BLOCK),
                ok(vk::Format::BC3_SRGB_BLOCK),
                ok(vk::Format::BC5_UNORM_BLOCK),
            ];
            let b = |binding, ty, stages| {
                vk::DescriptorSetLayoutBinding::default()
                    .binding(binding)
                    .descriptor_type(ty)
                    .descriptor_count(1)
                    .stage_flags(stages)
            };
            let vf = vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT;
            let fr = vk::ShaderStageFlags::FRAGMENT;
            let mut set0 = vec![b(0, vk::DescriptorType::UNIFORM_BUFFER, vf)];
            for k in 1..=7 {
                set0.push(b(k, vk::DescriptorType::STORAGE_BUFFER, vf));
            }
            // B-189: shadow maps (array) and their comparison sampler.
            set0.push(b(8, vk::DescriptorType::SAMPLED_IMAGE, fr));
            set0.push(b(9, vk::DescriptorType::SAMPLER, fr));
            let set0_layout = device.create_descriptor_set_layout(
                &vk::DescriptorSetLayoutCreateInfo::default().bindings(&set0),
                None,
            )?;
            let lm = [
                b(0, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(1, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(2, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(3, vk::DescriptorType::SAMPLER, fr),
            ];
            let lm_layout = device.create_descriptor_set_layout(
                &vk::DescriptorSetLayoutCreateInfo::default().bindings(&lm),
                None,
            )?;
            let post = [
                b(0, vk::DescriptorType::UNIFORM_BUFFER, fr),
                b(1, vk::DescriptorType::SAMPLER, fr),
                b(2, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(3, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(4, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(5, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(6, vk::DescriptorType::SAMPLER, fr),
            ];
            let post_layout = device.create_descriptor_set_layout(
                &vk::DescriptorSetLayoutCreateInfo::default().bindings(&post),
                None,
            )?;
            let pl = [post_layout];
            let post_pipe_layout = device.create_pipeline_layout(
                &vk::PipelineLayoutCreateInfo::default().set_layouts(&pl),
                None,
            )?;
            let words = shaders::spirv(&shaders::post_source())?;
            let post_module = device
                .create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&words), None)?;
            let mk_sampler = |addr| {
                device.create_sampler(
                    &vk::SamplerCreateInfo::default()
                        .mag_filter(vk::Filter::LINEAR)
                        .min_filter(vk::Filter::LINEAR)
                        .mipmap_mode(vk::SamplerMipmapMode::LINEAR)
                        .address_mode_u(addr)
                        .address_mode_v(addr)
                        .address_mode_w(addr)
                        .max_lod(vk::LOD_CLAMP_NONE),
                    None,
                )
            };
            let sampler = mk_sampler(vk::SamplerAddressMode::REPEAT)?;
            let lm_sampler = mk_sampler(vk::SamplerAddressMode::CLAMP_TO_EDGE)?;
            let post_sampler = mk_sampler(vk::SamplerAddressMode::CLAMP_TO_EDGE)?;
            let knobs = Knobs::from_env();
            let pg = pgpu::PGpu::new(&device)?;
            let tg = tgpu::TGpu::new(&device)?;
            // Cull mode and front face per draw need Vulkan 1.3's dynamic state; without it they join the pipeline key.
            let dyn_cull = std::env::var("BOTS_N1_DYN_CULL").map(|v| v != "0").unwrap_or(true)
                && instance.get_physical_device_properties(ctx.physical_device).api_version >= vk::API_VERSION_1_3
                && (instance.fp_v1_0().get_device_proc_addr)(ctx.device, c"vkCmdSetCullMode".as_ptr()).is_some()
                && (instance.fp_v1_0().get_device_proc_addr)(ctx.device, c"vkCmdSetFrontFace".as_ptr()).is_some();
            let store = Store::new(
                device.clone(),
                set0_layout,
                lm_layout,
                pg.pipe_layout,
                cache_dir.map(|d| d.join("n1-gpu")),
                dyn_cull,
            )?;
            let mut allocator = allocator;
            let shadows = shadow::Shadows::new(&device, &mut allocator, set0_layout, store.cache, &knobs)?;
            let gprof = gprof::GpuProf::from_env(&instance, &device, ctx.physical_device);
            let mut r = Self {
                laps: Laps::new(),
                ctr: Ctr::default(),
                _instance: instance,
                device,
                allocator: Some(allocator),
                color_format,
                encode_srgb: !is_srgb(color_format),
                knobs,
                set0_layout,
                lm_layout,
                store,
                ready: HashMap::new(),
                unresolved: Vec::new(),
                free_mats: Vec::new(),
                free_meshes: Vec::new(),
                free_words: Vec::new(),
                free_lights: Vec::new(),
                light_patches: Vec::new(),
                completed: 0,
                miss_logged: HashSet::new(),
                dyn_cull,
                post_layout,
                post_pipe_layout,
                post_module,
                post_pipes: HashMap::new(),
                post_tex: HashMap::new(),
                post_mats_last: Vec::new(),
                post_real0: std::time::Instant::now(),
                pipe_pending: 0,
                sampler,
                lm_sampler,
                post_sampler,
                pools: Vec::new(),
                textures: Vec::new(),
                meshes: Vec::new(),
                mesh_info: Vec::new(),
                skin_meshes: Vec::new(),
                materials: Vec::new(),
                lights: Vec::new(),
                default_lm: vk::DescriptorSet::null(),
                grey_mat: NONE,
                light_cpu: vec![0, 0, 0],
                light: Buf::null(),
                light_dirty_from: 0,
                light_copy_serial: 0,
                slots: Vec::new(),
                hdr: None,
                bloom: None,
                graded: None,
                near: 5.0,
                depth: None,
                pending: Vec::new(),
                deferred: Vec::new(),
                last_serial: 0,
                bc_ok,
                unsupported_textures: 0,
                decoded_textures: 0,
                shader_errors: 0,
                stats: FrameStats::default(),
                lighting: Lighting::new(),
                t_start: std::time::Instant::now(),
                world_time: None,
                fade: [[1.0; 3], [0.0; 3]],
                debug_bits: 0,
                dyn_logged: false,
                gpu_insts: Vec::new(),
                spheres: Vec::new(),
                inst_env: Vec::new(),
                draws: Vec::new(),
                pg,
                shadows,
                gprof,
                tg,
                tframe: Default::default(),
                pframe: Default::default(),
            };
            r.light = r.buffer(
                4 << 20,
                vk::BufferUsageFlags::STORAGE_BUFFER | vk::BufferUsageFlags::TRANSFER_SRC | vk::BufferUsageFlags::TRANSFER_DST,
                "n1 light",
            )?;
            // Built-in textures: TEX_GREY (sRGB), TEX_NORMAL (flat), TEX_BLACK.
            r.upload_texture(vk::Format::B8G8R8A8_SRGB, 1, 1, &[&[153, 153, 153, 255]])?;
            r.upload_texture(vk::Format::B8G8R8A8_UNORM, 1, 1, &[&[255, 128, 128, 255]])?;
            r.upload_texture(vk::Format::B8G8R8A8_UNORM, 1, 1, &[&[0, 0, 0, 255]])?;
            r.default_lm = r.lm_set([TEX_BLACK; 3])?;
            // The post pipelines are the same every run: made here (from the pipeline cache when warm), not on the
            // first frame.
            r.post_pipe("fs_grade", HDR_FORMAT)?;
            r.post_pipe("fs_down", HDR_FORMAT)?;
            r.post_pipe("fs_blur", HDR_FORMAT)?;
            r.post_pipe("fs_comp", color_format)?;
            r.post_pipe("fs_copy", color_format)?;
            Ok(r)
        }
    }

    pub fn device(&self) -> &ash::Device {
        &self.device
    }

    fn buffer(&mut self, size: u64, usage: vk::BufferUsageFlags, name: &str) -> Result<Buf> {
        unsafe {
            let size = size.max(16);
            let buffer = self.device.create_buffer(
                &vk::BufferCreateInfo::default().size(size).usage(usage),
                None,
            )?;
            let requirements = self.device.get_buffer_memory_requirements(buffer);
            let alloc = self
                .allocator
                .as_mut()
                .unwrap()
                .allocate(&AllocationCreateDesc {
                    name,
                    requirements,
                    location: MemoryLocation::CpuToGpu,
                    linear: true,
                    allocation_scheme: AllocationScheme::GpuAllocatorManaged,
                })
                .map_err(|e| anyhow!("allocate {name} ({size} bytes): {e}"))?;
            self.device
                .bind_buffer_memory(buffer, alloc.memory(), alloc.offset())?;
            Ok(Buf {
                buffer,
                alloc: Some(alloc),
                cap: size,
            })
        }
    }

    fn defer_at(&mut self, serial: u64, g: Garbage) {
        self.deferred.push((serial, g));
    }

    fn defer(&mut self, g: Garbage) {
        // Freed once the host reports the submission after the last one we recorded into as complete.
        self.deferred.push((self.last_serial + 1, g));
    }

    fn free(&mut self, g: Garbage) {
        unsafe {
            match g {
                Garbage::Buffer(mut b) => {
                    self.device.destroy_buffer(b.buffer, None);
                    if let Some(a) = b.alloc.take() {
                        let _ = self.allocator.as_mut().unwrap().free(a);
                    }
                }
                Garbage::Image(image, view, a) => {
                    self.device.destroy_image_view(view, None);
                    self.device.destroy_image(image, None);
                    let _ = self.allocator.as_mut().unwrap().free(a);
                }
            }
        }
    }

    /// Frees what the GPU is done with: everything deferred at or before `completed`.
    pub fn retire(&mut self, completed: u64) {
        self.completed = self.completed.max(completed);
        let (done, keep): (Vec<_>, Vec<_>) = std::mem::take(&mut self.deferred)
            .into_iter()
            .partition(|(s, _)| *s <= completed);
        self.deferred = keep;
        for (_, g) in done {
            self.free(g);
        }
    }

    /// A descriptor set from the shared pools (a new pool when the current one is exhausted).
    fn alloc_set(&mut self, layout: vk::DescriptorSetLayout) -> Result<vk::DescriptorSet> {
        unsafe {
            let layouts = [layout];
            if let Some(&p) = self.pools.last() {
                match self.device.allocate_descriptor_sets(
                    &vk::DescriptorSetAllocateInfo::default()
                        .descriptor_pool(p)
                        .set_layouts(&layouts),
                ) {
                    Ok(s) => return Ok(s[0]),
                    Err(vk::Result::ERROR_OUT_OF_POOL_MEMORY)
                    | Err(vk::Result::ERROR_FRAGMENTED_POOL) => {}
                    Err(e) => return Err(e.into()),
                }
            }
            let sizes = [
                vk::DescriptorPoolSize {
                    ty: vk::DescriptorType::SAMPLER,
                    descriptor_count: POOL_SETS * 2,
                },
                vk::DescriptorPoolSize {
                    ty: vk::DescriptorType::SAMPLED_IMAGE,
                    descriptor_count: POOL_SETS * 6,
                },
                vk::DescriptorPoolSize {
                    ty: vk::DescriptorType::UNIFORM_BUFFER,
                    descriptor_count: POOL_SETS * 2,
                },
                vk::DescriptorPoolSize {
                    ty: vk::DescriptorType::STORAGE_BUFFER,
                    descriptor_count: 256,
                },
            ];
            let p = self.device.create_descriptor_pool(
                &vk::DescriptorPoolCreateInfo::default()
                    .max_sets(POOL_SETS)
                    .pool_sizes(&sizes),
                None,
            )?;
            self.pools.push(p);
            Ok(self.device.allocate_descriptor_sets(
                &vk::DescriptorSetAllocateInfo::default()
                    .descriptor_pool(p)
                    .set_layouts(&layouts),
            )?[0])
        }
    }

    fn image(
        &mut self,
        format: vk::Format,
        width: u32,
        height: u32,
        mips: u32,
        usage: vk::ImageUsageFlags,
        aspect: vk::ImageAspectFlags,
        name: &str,
    ) -> Result<(vk::Image, vk::ImageView, Allocation)> {
        unsafe {
            let image = self.device.create_image(
                &vk::ImageCreateInfo::default()
                    .image_type(vk::ImageType::TYPE_2D)
                    .format(format)
                    .extent(vk::Extent3D {
                        width,
                        height,
                        depth: 1,
                    })
                    .mip_levels(mips)
                    .array_layers(1)
                    .samples(vk::SampleCountFlags::TYPE_1)
                    .tiling(vk::ImageTiling::OPTIMAL)
                    .usage(usage),
                None,
            )?;
            let requirements = self.device.get_image_memory_requirements(image);
            let alloc = self
                .allocator
                .as_mut()
                .unwrap()
                .allocate(&AllocationCreateDesc {
                    name,
                    requirements,
                    location: MemoryLocation::GpuOnly,
                    linear: false,
                    allocation_scheme: AllocationScheme::GpuAllocatorManaged,
                })
                .map_err(|e| anyhow!("allocate {name}: {e}"))?;
            self.device
                .bind_image_memory(image, alloc.memory(), alloc.offset())?;
            let view = self.device.create_image_view(
                &vk::ImageViewCreateInfo::default()
                    .image(image)
                    .view_type(vk::ImageViewType::TYPE_2D)
                    .format(format)
                    .subresource_range(vk::ImageSubresourceRange {
                        aspect_mask: aspect,
                        base_mip_level: 0,
                        level_count: mips,
                        base_array_layer: 0,
                        layer_count: 1,
                    }),
                None,
            )?;
            Ok((image, view, alloc))
        }
    }

    fn upload_texture(
        &mut self,
        format: vk::Format,
        width: u32,
        height: u32,
        mips: &[&[u8]],
    ) -> Result<u32> {
        // A copy that reads past its staging data is a GPU page fault (a TDR on Windows): upload only the mips whose
        // data is complete, and refuse a texture whose top mip is short (the caller falls back to grey).
        let (bw, bb) = match format {
            vk::Format::BC1_RGBA_UNORM_BLOCK | vk::Format::BC1_RGBA_SRGB_BLOCK => (4u64, 8u64),
            vk::Format::BC2_UNORM_BLOCK
            | vk::Format::BC2_SRGB_BLOCK
            | vk::Format::BC3_UNORM_BLOCK
            | vk::Format::BC3_SRGB_BLOCK
            | vk::Format::BC5_UNORM_BLOCK => (4, 16),
            vk::Format::R8G8_UNORM => (1, 2),
            _ => (1, 4),
        };
        let complete = mips
            .iter()
            .enumerate()
            .take_while(|(i, m)| {
                let (w, h) = (((width >> i).max(1)) as u64, ((height >> i).max(1)) as u64);
                m.len() as u64 >= w.div_ceil(bw) * h.div_ceil(bw) * bb
            })
            .count();
        if complete == 0 || width == 0 || height == 0 {
            bail!("texture {width}x{height} {format:?}: top mip data short");
        }
        let mips = &mips[..complete];
        let total: usize = mips.iter().map(|m| m.len()).sum();
        self.ctr.upload_bytes += total as u64;
        self.ctr.textures_added += 1;
        let mut staging = self.buffer(
            total as u64,
            vk::BufferUsageFlags::TRANSFER_SRC,
            "n1 staging",
        )?;
        let mut regions = Vec::new();
        let mut off = 0usize;
        {
            let dst = staging.bytes();
            for (i, m) in mips.iter().enumerate() {
                dst[off..off + m.len()].copy_from_slice(m);
                let (w, h) = ((width >> i).max(1), (height >> i).max(1));
                regions.push(
                    vk::BufferImageCopy::default()
                        .buffer_offset(off as u64)
                        .image_subresource(vk::ImageSubresourceLayers {
                            aspect_mask: vk::ImageAspectFlags::COLOR,
                            mip_level: i as u32,
                            base_array_layer: 0,
                            layer_count: 1,
                        })
                        .image_extent(vk::Extent3D {
                            width: w,
                            height: h,
                            depth: 1,
                        }),
                );
                off += m.len();
            }
        }
        let mip_count = mips.len().max(1) as u32;
        let (image, view, alloc) = self.image(
            format,
            width,
            height,
            mip_count,
            vk::ImageUsageFlags::SAMPLED | vk::ImageUsageFlags::TRANSFER_DST,
            vk::ImageAspectFlags::COLOR,
            "n1 texture",
        )?;
        self.textures.push(Tex {
            image,
            view,
            alloc: Some(alloc),
            bc5: format == vk::Format::BC5_UNORM_BLOCK,
            gamma: false,
        });
        self.pending.push(Upload {
            staging,
            image,
            regions,
            mips: mip_count,
        });
        Ok(self.textures.len() as u32 - 1)
    }

    /// B-266: a post-material texture the host loaded (`n1_record` pumps them in). Colour textures that the game
    /// fetches with gamma are uploaded raw and decoded with Xenos's curve in `post.wgsl` (`xe_pwl_raw`).
    pub fn post_tex_add(&mut self, name: &str, t: &assetdb::TextureAsset) -> Result<()> {
        if self.post_tex.contains_key(name) {
            return Ok(());
        }
        let mips: Vec<&[u8]> = t.mips.iter().map(|x| x.as_slice()).collect();
        let pwl = t.srgb && !t.normal;
        let id = self.try_add_texture(&crate::particles::tex_desc(t, &mips, true))?;
        self.post_tex.insert(name.to_string(), (id, pwl));
        Ok(())
    }

    /// B-266: the frame's post-material passes whose textures are loaded: (entry point, params, textures, pwl flags).
    fn post_mat_passes(&self) -> Vec<(&'static str, [f32; 4], [u32; 2], [bool; 2])> {
        let mut v = Vec::new();
        for e in self.lighting.post.mats.iter() {
            let mut ids = [TEX_BLACK; 2];
            let mut pwl = [false; 2];
            let mut ready = true;
            for (k, t) in e.kind.textures().iter().enumerate() {
                if t.is_empty() {
                    continue;
                }
                match self.post_tex.get(*t) {
                    Some(&(id, g)) => (ids[k], pwl[k]) = (id, g),
                    None => ready = false,
                }
            }
            if ready && v.len() < POST_MATS {
                v.push((e.kind.fs(), e.params, ids, pwl));
            }
        }
        v
    }

    fn tex_view(&self, id: u32, fallback: u32) -> vk::ImageView {
        self.textures
            .get(id as usize)
            .or_else(|| self.textures.get(fallback as usize))
            .map(|t| t.view)
            .unwrap()
    }

    fn lm_set(&mut self, tex: [u32; 3]) -> Result<vk::DescriptorSet> {
        let set = self.alloc_set(self.lm_layout)?;
        let ii: Vec<[vk::DescriptorImageInfo; 1]> = tex
            .iter()
            .map(|&t| {
                [vk::DescriptorImageInfo {
                    sampler: vk::Sampler::null(),
                    image_view: self.tex_view(t, TEX_BLACK),
                    image_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                }]
            })
            .collect();
        let si = [vk::DescriptorImageInfo {
            sampler: self.lm_sampler,
            image_view: vk::ImageView::null(),
            image_layout: vk::ImageLayout::UNDEFINED,
        }];
        let mut writes: Vec<vk::WriteDescriptorSet> = (0..3)
            .map(|k| {
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(k as u32)
                    .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                    .image_info(&ii[k])
            })
            .collect();
        writes.push(
            vk::WriteDescriptorSet::default()
                .dst_set(set)
                .dst_binding(3)
                .descriptor_type(vk::DescriptorType::SAMPLER)
                .image_info(&si),
        );
        unsafe { self.device.update_descriptor_sets(&writes, &[]) };
        Ok(set)
    }

    fn mat_layout(&mut self, ntex: usize) -> Result<(vk::DescriptorSetLayout, vk::PipelineLayout)> {
        self.store.mat_layout(ntex)
    }

    fn shader_flags(&self, def: &MatDef, blend: Blend) -> ShaderFlags {
        let f = self.knobs.features;
        ShaderFlags {
            lm: f & 1 != 0,
            nmap: f & 2 != 0,
            spec: f & 4 != 0,
            fog: f & 8 != 0,
            dynl: f & 16 != 0,
            lm_pwl: scenekit::lm_pwl(),
            ..ShaderFlags::new(
                blend == Blend::Masked,
                def.lighting == "MLM_Unlit",
                blend.blended(),
                (def.clip.max(0.0) * 1000.0) as u32,
            )
        }
    }

    fn try_add_material(&mut self, m: &MaterialDesc) -> Result<u32> {
        // No material graph: sample the diffuse texture (or a flat grey), as the old path did.
        let fallback;
        let (def, textures): (&MatDef, Vec<u32>) = match m.def {
            Some(d) => (d, m.textures.to_vec()),
            None => {
                fallback = if m.texture != NONE {
                    MatDef {
                        textures: vec![TexRef {
                            path: String::new(),
                            normal: false,
                        }],
                        nodes: vec![
                            Node::Tex { tex: 0, uv: None },
                            Node::Mask {
                                x: 0,
                                m: [false, false, false, true],
                            },
                        ],
                        diffuse: Some(0),
                        opacity: Some(1),
                        opacity_mask: Some(1),
                        clip: 0.33,
                        ..Default::default()
                    }
                } else {
                    MatDef {
                        diffuse_const: Some([0.6, 0.6, 0.6, 1.0]),
                        ..Default::default()
                    }
                };
                let t = if m.texture != NONE { vec![m.texture] } else { vec![] };
                (&fallback, t)
            }
        };
        let is_bc5 = |k: usize| {
            textures
                .get(k)
                .and_then(|&t| self.textures.get(t as usize))
                .is_some_and(|t| t.bc5)
        };
        // A BC5 normal map without UnpackMin/Max still needs X and Y in [-1, 1].
        let unpack: Vec<Option<([f32; 3], [f32; 3])>> = (0..def.textures.len())
            .map(|k| {
                m.unpack
                    .get(k)
                    .copied()
                    .flatten()
                    .or_else(|| is_bc5(k).then_some(([-1.0; 3], [1.0; 3])))
            })
            .collect();
        // BC5 normal maps (characters, weapons) hold X and Y only: rebuild Z as UE3 does.
        let recon_z: Vec<bool> = (0..def.textures.len()).map(is_bc5).collect();
        // B-235: textures uploaded raw for the gamma fetch take the PWL curve.
        let gamma: Vec<bool> = (0..def.textures.len())
            .map(|k| textures.get(k).and_then(|&t| self.textures.get(t as usize)).is_some_and(|t| t.gamma))
            .collect();
        let c = matdef::codegen::compile_gamma(def, &unpack, &recon_z, &gamma);
        let fl = self.shader_flags(def, m.blend);
        let skey = crate::plan::fnv(format!("{fl:?}|{}", c.key).as_bytes());
        // The shader is built by the startup step or a worker, never here: if it isn't ready the material is drawn
        // with the grey stand-in until it is.
        let shader = match self.store.module_lookup(skey) {
            Some(id) => {
                // B-199: materials arrive with the level preload, before anything draws them; have the static-mesh
                // pipeline built now too (a worker, not urgent), so the first draw does not wait on the stand-in.
                let cull = if self.dyn_cull { 0 } else { m.two_sided as u8 };
                self.store.prewarm_scene_pipe(crate::store::SceneKey { shader: id, blend: blend_of(m.blend), skin: false, cull });
                id
            }
            None => {
                let after = vec![(blend_of(m.blend), false)];
                self.store.want_scene_shader(skey, c.clone(), fl, after);
                NO_SHADER
            }
        };
        let (sl, _) = self.mat_layout(c.ntex)?;
        let mut params = c.params.clone();
        if params.is_empty() {
            params.push([0.0; 4]);
        }
        let pb: &[u8] = bytemuck::cast_slice(&params);
        // A released material whose last use has completed, with the same set layout and room for the parameters:
        // rewrite it in place (run-time material variants come and go all the time: a tracer's per shot).
        let completed = self.completed;
        let reuse = self.free_mats.iter().position(|&(s, id)| {
            let m = &self.materials[id as usize];
            s <= completed && m.ntex == c.ntex && m._params.cap >= pb.len() as u64
        });
        let (id, set, pbuf) = match reuse {
            Some(k) => {
                let (_, id) = self.free_mats.swap_remove(k);
                let m = &mut self.materials[id as usize];
                m._params.bytes()[..pb.len()].copy_from_slice(pb);
                (id, m.set, m._params.buffer)
            }
            None => {
                let mut pbuf = self.buffer(
                    pb.len() as u64,
                    vk::BufferUsageFlags::UNIFORM_BUFFER,
                    "n1 material params",
                )?;
                pbuf.bytes()[..pb.len()].copy_from_slice(pb);
                let set = self.alloc_set(sl)?;
                let b = pbuf.buffer;
                self.materials.push(MatInfo {
                    shader: NO_SHADER,
                    skey: 0,
                    set,
                    _params: pbuf,
                    ntex: c.ntex,
                    blend: m.blend,
                    two_sided: m.two_sided,
                });
                (self.materials.len() as u32 - 1, set, b)
            }
        };
        let si = [vk::DescriptorImageInfo {
            sampler: self.sampler,
            image_view: vk::ImageView::null(),
            image_layout: vk::ImageLayout::UNDEFINED,
        }];
        let bi = [vk::DescriptorBufferInfo {
            buffer: pbuf,
            offset: 0,
            range: pb.len() as u64,
        }];
        let ii: Vec<[vk::DescriptorImageInfo; 1]> = def
            .textures
            .iter()
            .enumerate()
            .map(|(k, tr)| {
                let fb = if tr.normal { TEX_NORMAL } else { TEX_GREY };
                let id = textures.get(k).copied().unwrap_or(NONE);
                [vk::DescriptorImageInfo {
                    sampler: vk::Sampler::null(),
                    image_view: if id == NONE {
                        self.tex_view(fb, fb)
                    } else {
                        self.tex_view(id, fb)
                    },
                    image_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                }]
            })
            .collect();
        let mut writes = vec![
            vk::WriteDescriptorSet::default()
                .dst_set(set)
                .dst_binding(0)
                .descriptor_type(vk::DescriptorType::SAMPLER)
                .image_info(&si),
            vk::WriteDescriptorSet::default()
                .dst_set(set)
                .dst_binding(1)
                .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER)
                .buffer_info(&bi),
        ];
        for (k, info) in ii.iter().enumerate() {
            writes.push(
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(2 + k as u32)
                    .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                    .image_info(info),
            );
        }
        unsafe { self.device.update_descriptor_sets(&writes, &[]) };
        if shader == NO_SHADER && !self.unresolved.contains(&id) {
            self.unresolved.push(id);
            if self.store.log_misses {
                eprintln!("n1ffi: material #{id} needs shader {skey:016x} ({} params)", c.params.len());
            }
        }
        let mi = &mut self.materials[id as usize];
        mi.shader = shader;
        mi.skey = skey;
        mi.blend = m.blend;
        mi.two_sided = m.two_sided;
        Ok(id)
    }

    fn post_pipe(&mut self, fs: &'static str, out: vk::Format) -> Result<vk::Pipeline> {
        let k = (fs, out.as_raw());
        if let Some(&p) = self.post_pipes.get(&k) {
            return Ok(p);
        }
        unsafe {
            let name = std::ffi::CString::new(fs).unwrap();
            let stages = [
                vk::PipelineShaderStageCreateInfo::default()
                    .stage(vk::ShaderStageFlags::VERTEX)
                    .module(self.post_module)
                    .name(c"vs"),
                vk::PipelineShaderStageCreateInfo::default()
                    .stage(vk::ShaderStageFlags::FRAGMENT)
                    .module(self.post_module)
                    .name(&name),
            ];
            let vi = vk::PipelineVertexInputStateCreateInfo::default();
            let ia = vk::PipelineInputAssemblyStateCreateInfo::default()
                .topology(vk::PrimitiveTopology::TRIANGLE_LIST);
            let vp = vk::PipelineViewportStateCreateInfo::default()
                .viewport_count(1)
                .scissor_count(1);
            let rs = vk::PipelineRasterizationStateCreateInfo::default()
                .polygon_mode(vk::PolygonMode::FILL)
                .cull_mode(vk::CullModeFlags::NONE)
                .line_width(1.0);
            let ms = vk::PipelineMultisampleStateCreateInfo::default()
                .rasterization_samples(vk::SampleCountFlags::TYPE_1);
            let ds = vk::PipelineDepthStencilStateCreateInfo::default();
            let att = [vk::PipelineColorBlendAttachmentState::default()
                .color_write_mask(vk::ColorComponentFlags::RGBA)];
            let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&att);
            let dyns = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
            let dy = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyns);
            let formats = [out];
            let mut rendering =
                vk::PipelineRenderingCreateInfo::default().color_attachment_formats(&formats);
            let info = vk::GraphicsPipelineCreateInfo::default()
                .stages(&stages)
                .vertex_input_state(&vi)
                .input_assembly_state(&ia)
                .viewport_state(&vp)
                .rasterization_state(&rs)
                .multisample_state(&ms)
                .depth_stencil_state(&ds)
                .color_blend_state(&cb)
                .dynamic_state(&dy)
                .layout(self.post_pipe_layout)
                .push_next(&mut rendering);
            let _use = self.store.cache_use.read().unwrap_or_else(|p| p.into_inner());
            let p = self
                .device
                .create_graphics_pipelines(self.store.cache, &[info], None)
                .map_err(|(_, e)| anyhow!("post pipeline {fs}: {e}"))?[0];
            self.post_pipes.insert(k, p);
            Ok(p)
        }
    }

    fn rt(&mut self, format: vk::Format, w: u32, h: u32, depth: bool, name: &str) -> Result<Rt> {
        let (usage, aspect) = if depth {
            (
                vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT | vk::ImageUsageFlags::SAMPLED,
                vk::ImageAspectFlags::DEPTH,
            )
        } else {
            (
                vk::ImageUsageFlags::COLOR_ATTACHMENT | vk::ImageUsageFlags::SAMPLED,
                vk::ImageAspectFlags::COLOR,
            )
        };
        let (image, view, alloc) = self.image(format, w, h, 1, usage, aspect, name)?;
        Ok(Rt {
            image,
            view,
            alloc,
            width: w,
            height: h,
        })
    }

    fn ensure_targets(&mut self, w: u32, h: u32) -> Result<()> {
        if self.hdr.as_ref().is_some_and(|r| r.width == w && r.height == h) {
            return Ok(());
        }
        for r in [self.hdr.take(), self.depth.take(), self.graded.take()].into_iter().flatten() {
            self.defer(Garbage::Image(r.image, r.view, r.alloc));
        }
        if let Some(b) = self.bloom.take() {
            for r in b {
                self.defer(Garbage::Image(r.image, r.view, r.alloc));
            }
        }
        self.hdr = Some(self.rt(HDR_FORMAT, w, h, false, "n1 hdr")?);
        self.depth = Some(self.rt(DEPTH_FORMAT, w, h, true, "n1 depth")?);
        let (bw, bh) = ((w / 4).max(1), (h / 4).max(1));
        let a = self.rt(HDR_FORMAT, bw, bh, false, "n1 post q0")?;
        let b = self.rt(HDR_FORMAT, bw, bh, false, "n1 post q1")?;
        let c = self.rt(HDR_FORMAT, bw, bh, false, "n1 post q2")?;
        self.bloom = Some([a, b, c]);
        self.graded = Some(self.rt(HDR_FORMAT, w, h, false, "n1 graded")?);
        Ok(())
    }

    /// A per-frame slot whose last use has completed, with at least `need` bytes.
    fn slot(&mut self, need: u64, serial: u64, completed: u64) -> Result<usize> {
        let i = match self.slots.iter().position(|s| s.serial <= completed) {
            Some(i) => i,
            None => {
                let buf = self.buffer(
                    need.next_power_of_two(),
                    vk::BufferUsageFlags::UNIFORM_BUFFER | vk::BufferUsageFlags::STORAGE_BUFFER,
                    "n1 frame",
                )?;
                let set = self.alloc_set(self.set0_layout)?;
                let mut post_sets = [vk::DescriptorSet::null(); POST_PASSES];
                for s in &mut post_sets {
                    *s = self.alloc_set(self.post_layout)?;
                }
                let pset = self.alloc_set(self.pg.g_layout)?;
                self.slots.push(Slot {
                    buf,
                    set,
                    post_sets,
                    pbuf: Buf::null(),
                    pset,
                    serial: 0,
                });
                self.slots.len() - 1
            }
        };
        if self.slots[i].buf.cap < need {
            let buf = self.buffer(
                need.next_power_of_two(),
                vk::BufferUsageFlags::UNIFORM_BUFFER | vk::BufferUsageFlags::STORAGE_BUFFER,
                "n1 frame",
            )?;
            let old = std::mem::replace(&mut self.slots[i].buf, buf);
            self.free(Garbage::Buffer(old)); // its last use has completed
        }
        self.slots[i].serial = serial;
        Ok(i)
    }

    /// Brings the GPU copy of the vertex-light words (`light_cpu`) up to date. `cmd` is outside any render pass.
    ///
    /// B-264: when the words outgrow the buffer, the old way allocated a bigger one and rewrote *every* word from the CPU
    /// (plus a second temporary copy of them) in the first scene frame: 36-59 ms on a laptop once three levels' lights
    /// (27 MB) crossed 16 MB. Now the old words are copied by the GPU (`cmd_copy_buffer`, ~2 ms of GPU time) and the CPU
    /// writes only the words appended since the last sync; `upload` syncs too, so those arrive on loading frames, a few
    /// per frame, instead of all in the first scene frame. `BOTS_N1_LIGHT_GROW=0`: the old path.
    fn sync_light(&mut self, cmd: vk::CommandBuffer) -> Result<()> {
        let fast = self.knobs.light_grow;
        let plan = light_sync_plan(self.light_cpu.len(), self.light_dirty_from, self.light.cap, fast);
        if let Some(cap) = plan.grow_to {
            let usage = vk::BufferUsageFlags::STORAGE_BUFFER
                | vk::BufferUsageFlags::TRANSFER_SRC
                | vk::BufferUsageFlags::TRANSFER_DST;
            if plan.gpu_copy_words > 0 {
                // The words below the copy limit must be current in the old buffer when the GPU copies them: apply the
                // patches (reused decal ranges) to it first. Patches above the limit come with the CPU tail below.
                let limit = plan.gpu_copy_words;
                for &(from, to) in &self.light_patches {
                    let to = to.min(limit);
                    if from < to {
                        let src: &[u8] = bytemuck::cast_slice(&self.light_cpu[from..to]);
                        self.light.bytes()[from * 4..from * 4 + src.len()].copy_from_slice(src);
                    }
                }
            }
            let buf = self.buffer(cap, usage, "n1 light")?;
            let old = std::mem::replace(&mut self.light, buf);
            if plan.gpu_copy_words > 0 {
                let bytes = plan.gpu_copy_words as u64 * 4;
                unsafe {
                    self.device.cmd_copy_buffer(
                        cmd,
                        old.buffer,
                        self.light.buffer,
                        &[vk::BufferCopy { src_offset: 0, dst_offset: 0, size: bytes }],
                    );
                    let b = [vk::BufferMemoryBarrier::default()
                        .src_access_mask(vk::AccessFlags::TRANSFER_WRITE)
                        .dst_access_mask(vk::AccessFlags::SHADER_READ | vk::AccessFlags::TRANSFER_READ)
                        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                        .buffer(self.light.buffer)
                        .offset(0)
                        .size(bytes)];
                    self.device.cmd_pipeline_barrier(
                        cmd,
                        vk::PipelineStageFlags::TRANSFER,
                        vk::PipelineStageFlags::VERTEX_SHADER
                            | vk::PipelineStageFlags::FRAGMENT_SHADER
                            | vk::PipelineStageFlags::TRANSFER, // a second growth copies from this buffer
                        vk::DependencyFlags::empty(),
                        &[],
                        &b,
                        &[],
                    );
                }
                self.light_patches.clear(); // below the limit: in the old buffer, now copied; above: in the tail
                self.light_copy_serial = self.last_serial;
            }
            // The old buffer is freed once the submission this copy is part of has completed (`last_serial` is current).
            self.defer(Garbage::Buffer(old));
            self.light_dirty_from = plan.cpu_from;
            self.laps.lap("sync_light_grow");
            if record_log_ms().is_some() {
                eprintln!(
                    "n1ffi: record-log light buffer grown to {} MB for {} MB of words ({} MB by GPU copy)",
                    cap >> 20,
                    (self.light_cpu.len() * 4) >> 20,
                    (plan.gpu_copy_words * 4) >> 20
                );
            }
        }
        if self.light_dirty_from < self.light_cpu.len() {
            // Appends only: the GPU may still read the earlier words, which don't change.
            let from = self.light_dirty_from;
            let src: &[u8] = bytemuck::cast_slice(&self.light_cpu[from..]);
            if fast {
                self.light.bytes()[from * 4..from * 4 + src.len()].copy_from_slice(src);
            } else {
                let tmp = src.to_vec();
                self.light.bytes()[from * 4..from * 4 + tmp.len()].copy_from_slice(&tmp);
            }
            self.light_dirty_from = self.light_cpu.len();
            self.laps.lap("sync_light_copy");
        }
        // A patch written by the CPU while the growth copy has not run yet would be overwritten by the copy's older words;
        // it waits (a reused decal range shows its previous words for a frame or two, once per growth).
        if self.light_copy_serial > self.completed {
            return Ok(());
        }
        for (from, to) in std::mem::take(&mut self.light_patches) {
            let src: Vec<u8> = bytemuck::cast_slice(&self.light_cpu[from..to]).to_vec();
            self.light.bytes()[from * 4..from * 4 + src.len()].copy_from_slice(&src);
        }
        Ok(())
    }

    fn flush_uploads(&mut self, cmd: vk::CommandBuffer, serial: u64) -> u32 {
        let n = self.pending.len() as u32;
        if n == 0 {
            return 0;
        }
        let pending = std::mem::take(&mut self.pending);
        unsafe {
            let range = |mips| vk::ImageSubresourceRange {
                aspect_mask: vk::ImageAspectFlags::COLOR,
                base_mip_level: 0,
                level_count: mips,
                base_array_layer: 0,
                layer_count: 1,
            };
            let to_dst: Vec<vk::ImageMemoryBarrier> = pending
                .iter()
                .map(|u| {
                    vk::ImageMemoryBarrier::default()
                        .src_access_mask(vk::AccessFlags::empty())
                        .dst_access_mask(vk::AccessFlags::TRANSFER_WRITE)
                        .old_layout(vk::ImageLayout::UNDEFINED)
                        .new_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                        .image(u.image)
                        .subresource_range(range(u.mips))
                })
                .collect();
            self.device.cmd_pipeline_barrier(
                cmd,
                vk::PipelineStageFlags::TOP_OF_PIPE,
                vk::PipelineStageFlags::TRANSFER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &to_dst,
            );
            for u in &pending {
                self.device.cmd_copy_buffer_to_image(
                    cmd,
                    u.staging.buffer,
                    u.image,
                    vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                    &u.regions,
                );
            }
            let to_read: Vec<vk::ImageMemoryBarrier> = pending
                .iter()
                .map(|u| {
                    vk::ImageMemoryBarrier::default()
                        .src_access_mask(vk::AccessFlags::TRANSFER_WRITE)
                        .dst_access_mask(vk::AccessFlags::SHADER_READ)
                        .old_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                        .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
                        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                        .image(u.image)
                        .subresource_range(range(u.mips))
                })
                .collect();
            self.device.cmd_pipeline_barrier(
                cmd,
                vk::PipelineStageFlags::TRANSFER,
                vk::PipelineStageFlags::FRAGMENT_SHADER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &to_read,
            );
        }
        for u in pending {
            self.defer_at(serial, Garbage::Buffer(u.staging));
        }
        n
    }

    /// BC5 textures can be sampled (normal maps stay two-channel).
    pub fn bc5_ok(&self) -> bool {
        self.bc_ok[3]
    }

    /// Instances in the last frame lit by a light environment.
    pub fn env_bound(&self) -> u32 {
        self.inst_env.iter().filter(|&&e| e != NONE).count() as u32
    }

    /// Picks up shaders the workers finished since the last frame.
    fn resolve_shaders(&mut self) {
        if self.unresolved.is_empty() {
            return;
        }
        let store = self.store.clone();
        let mats = &mut self.materials;
        self.unresolved.retain(|&i| {
            let m = &mut mats[i as usize];
            match store.module_lookup(m.skey) {
                Some(id) => {
                    m.shader = id;
                    false
                }
                None => true,
            }
        });
    }

    fn scene_key(&self, k: &Key) -> SceneKey {
        SceneKey {
            shader: k.shader,
            blend: k.blend,
            skin: k.skin,
            cull: if self.dyn_cull { 0 } else { k.two_sided as u8 | (k.mirrored as u8) << 1 },
        }
    }

    /// Is the pipeline for this draw built? If not, asks for it (once) and says no.
    fn pipe_ready(&mut self, k: &Key) -> bool {
        if k.shader == NO_SHADER {
            return false;
        }
        let sk = self.scene_key(k);
        if self.ready.contains_key(&sk) {
            return true;
        }
        if let Some(p) = self.store.pipe_lookup(crate::store::AnyKeyRef::Scene(sk)) {
            self.ready.insert(sk, p);
            return true;
        }
        if self.miss_logged.insert(sk) {
            self.store.want_scene_pipe(sk);
        }
        false
    }

    /// Never block on a compile: a draw whose shader or pipeline isn't ready is drawn with the grey stand-in (opaque
    /// ones) or left out (blended ones), and the missing pipeline is built by a worker.
    fn fix_draws(&mut self, grey: u32) {
        let grey_shader = self.materials.get(grey as usize).map(|m| m.shader).unwrap_or(NO_SHADER);
        let mut draws = std::mem::take(&mut self.draws);
        let mut skipped = 0u32;
        let mut fallback = 0u64;
        draws.retain_mut(|d| {
            if self.pipe_ready(&d.key) {
                return true;
            }
            if !blended(d.key.blend) && grey_shader != NO_SHADER {
                let k = Key { shader: grey_shader, blend: 0, ..d.key };
                if self.pipe_ready(&k) {
                    d.key = k;
                    d.mat = grey;
                    fallback += 1;
                    return true;
                }
            }
            skipped += 1;
            false
        });
        self.draws = draws;
        self.ctr.fallback_draws += fallback;
        self.ctr.draws_no_pipe += skipped as u64;
        self.ctr.partial_frames += (skipped > 0) as u32;
    }

    /// Builds this frame's instances and sorted draw list. `env_map`: N1 env handle -> light environment index.
    fn build(
        &mut self,
        view_proj: &[f32; 16],
        eye: [f32; 3],
        instances: &[Instance],
        overrides: &[u32],
        skin: &SkinFrame,
        env_map: &[u32],
        st: &mut FrameStats,
    ) {
        let planes = scenekit::frustum_planes(view_proj);
        let grey = self.grey_material();
        self.laps.lap("build_grey");
        self.resolve_shaders();
        self.laps.lap("build_resolve_shaders");
        self.gpu_insts.clear();
        self.spheres.clear();
        self.inst_env.clear();
        self.draws.clear();
        st.instances = instances.len() as u32;
        let env_of = |h: u32| -> u32 {
            if h == NONE {
                NONE
            } else {
                env_map.get(h as usize).copied().unwrap_or(NONE)
            }
        };
        for inst in instances {
            let Some(mesh) = self.mesh_info.get(inst.mesh as usize) else {
                st.missing_mesh += 1;
                continue;
            };
            let m = &inst.transform;
            let c = mesh.center;
            let wc = [
                c[0] * m[0] + c[1] * m[4] + c[2] * m[8] + m[12],
                c[0] * m[1] + c[1] * m[5] + c[2] * m[9] + m[13],
                c[0] * m[2] + c[1] * m[6] + c[2] * m[10] + m[14],
            ];
            let row = |r: usize| {
                (m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2])
                    .sqrt()
            };
            let radius = mesh.radius * row(0).max(row(1)).max(row(2));
            if planes
                .iter()
                .any(|p| p[0] * wc[0] + p[1] * wc[1] + p[2] * wc[2] + p[3] < -radius)
            {
                st.culled += 1;
                continue;
            }
            let det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8])
                + m[2] * (m[4] * m[9] - m[5] * m[8]);
            let mut g = GpuInstance {
                model: swz_model(m),
                light_base: 0,
                flags: 0,
                p1: 0,
                p2: 0,
                sc: [[0.0; 4]; 3],
                lm_xf: [1.0, 1.0, 0.0, 0.0],
            };
            let mut lm = self.default_lm;
            match self.lights.get(inst.light as usize) {
                Some(LightInfo::Words { base, count, sc }) if *count == mesh.vertex_count => {
                    g.flags = 1;
                    g.light_base = *base as i32;
                    g.sc = *sc;
                }
                Some(LightInfo::Map { set, sc, xf }) => {
                    g.flags = 2;
                    g.sc = *sc;
                    g.lm_xf = *xf;
                    lm = *set;
                }
                _ => {}
            }
            let ii = self.gpu_insts.len() as u32;
            self.gpu_insts.push(g);
            self.spheres.push(([wc[0], wc[2], wc[1]], radius));
            self.inst_env.push(env_of(inst.env));
            let dist =
                ((wc[0] - eye[0]).powi(2) + (wc[1] - eye[1]).powi(2) + (wc[2] - eye[2]).powi(2))
                    .sqrt();
            for (si, s) in mesh.sections.iter().enumerate() {
                let mut mat = s.material;
                if (si as u32) < inst.mat_count
                    && let Some(&o) = overrides.get((inst.mat_offset + si as u32) as usize)
                    && o != NONE
                {
                    if o == scenekit::SKIP {
                        continue;
                    }
                    mat = o;
                }
                if mat as usize >= self.materials.len() {
                    mat = grey;
                }
                let Some(mt) = self.materials.get(mat as usize) else {
                    continue;
                };
                self.draws.push(Draw {
                    key: Key {
                        shader: mt.shader,
                        blend: blend_of(mt.blend),
                        two_sided: mt.two_sided,
                        mirrored: det < 0.0,
                        skin: false,
                    },
                    mat,
                    lm,
                    mesh: inst.mesh,
                    first_index: s.first_index,
                    index_count: s.index_count,
                    instance: ii,
                    depth: dist,
                });
            }
        }
        let skin_base = self.gpu_insts.len() as u32;
        for si in &skin.insts {
            let mut model = [0f32; 16];
            model[0] = 1.0;
            model[5] = 1.0;
            model[10] = 1.0;
            model[15] = 1.0;
            self.gpu_insts.push(GpuInstance {
                model,
                light_base: 0,
                flags: 0,
                p1: si.bone_base,
                p2: 0,
                sc: [[0.0; 4]; 3],
                lm_xf: [1.0, 1.0, 0.0, 0.0],
            });
            let b = si.bounds;
            self.spheres.push(([b[0], b[2], b[1]], b[3]));
            self.inst_env.push(si.env);
        }
        for d in &skin.draws {
            let Some(mt) = self.materials.get(d.material as usize) else {
                continue;
            };
            let b = skin.insts[d.instance as usize].bounds;
            let dist = ((b[0] - eye[0]).powi(2) + (b[1] - eye[1]).powi(2) + (b[2] - eye[2]).powi(2))
                .sqrt();
            self.draws.push(Draw {
                key: Key {
                    shader: mt.shader,
                    blend: blend_of(mt.blend),
                    two_sided: mt.two_sided,
                    mirrored: false,
                    skin: true,
                },
                mat: d.material,
                lm: self.default_lm,
                mesh: d.mesh,
                first_index: d.first_index,
                index_count: d.index_count,
                instance: skin_base + d.instance,
                depth: dist,
            });
        }
        self.laps.lap("build_insts");
        self.fix_draws(grey);
        self.laps.lap("build_fix_draws");
        // Opaque/masked by state; blended back to front. B-231 experiment (`BOTS_N1_SORT`): 1 = front to back inside
        // each pipeline, 2 = opaque/masked front to back (fewer shaded fragments behind early depth, more binds).
        let sort = self.knobs.opaque_sort;
        self.draws.sort_by(|a, b| {
            let (ab, bb) = (blended(a.key.blend), blended(b.key.blend));
            let near = |a: &Draw, b: &Draw| a.depth.partial_cmp(&b.depth).unwrap_or(std::cmp::Ordering::Equal);
            ab.cmp(&bb).then_with(|| {
                if ab {
                    b.depth
                        .partial_cmp(&a.depth)
                        .unwrap_or(std::cmp::Ordering::Equal)
                } else if sort == 1 {
                    a.key.cmp(&b.key).then_with(|| near(a, b))
                } else if sort == 2 {
                    near(a, b).then_with(|| (a.key, a.mat, a.lm, a.mesh).cmp(&(b.key, b.mat, b.lm, b.mesh)))
                } else {
                    (a.key, a.mat, a.lm, a.mesh).cmp(&(b.key, b.mat, b.lm, b.mesh))
                }
            })
        });
    }

    /// Records the frame into `cmd`, which must be outside any render pass / dynamic rendering. `serial` is the
    /// host's number for the submission `cmd` goes into (monotonic); `completed` the newest one known finished.
    /// `instances` carry renderer ids (`Backend::add_*` results), as does `overrides`.
    #[allow(clippy::too_many_arguments)]
    pub fn record(
        &mut self,
        cmd: vk::CommandBuffer,
        target: &Target,
        view_proj: &[f32; 16],
        eye: [f32; 3],
        instances: &[Instance],
        overrides: &[u32],
        skin: &SkinFrame,
        env_map: &[u32],
        serial: u64,
        completed: u64,
    ) -> Result<FrameStats> {
        let t0 = std::time::Instant::now();
        self.laps.begin();
        // B-199: `upload` may already have recorded into the same submission, so an equal serial is fine.
        if serial < self.last_serial {
            bail!("serial {serial} before {}", self.last_serial);
        }
        self.retire(completed);
        self.laps.lap("retire");
        // The serial must move before anything recorded into `cmd` is deferred: `defer` frees at `last_serial + 1`, and
        // with the old value the upload staging buffers were freed once the *previous* n1 submission's successor
        // completed. The host submits command buffers without n1 work in between (frames > scenes), so that could be
        // before `cmd` ran: the copy then read freed memory, a GPU page fault and a TDR on Intel/Windows.
        self.last_serial = serial;
        // Staging buffers are deferred to `serial` itself (B-199), so `upload` and this agree.
        let uploads = self.flush_uploads(cmd, serial);
        self.laps.lap("flush_uploads");
        let mut st = FrameStats {
            uploads,
            ..Default::default()
        };
        self.build(view_proj, eye, instances, overrides, skin, env_map, &mut st);
        self.laps.lap("build_tail_sort");
        self.sync_light(cmd)?;
        self.laps.lap("sync_light");

        // Per-instance light lists and environments.
        let dynl = self.knobs.features & 16 != 0;
        let inst_words: Vec<[u32; 2]> = if dynl {
            let env = &self.inst_env;
            self.lighting.dyn_frame.instance_words(
                &self.spheres,
                &|i| (env[i] != NONE).then_some(env[i] as usize),
                if self.knobs.dyn_lights { self.knobs.listed } else { 0 },
            )
        } else {
            vec![[0xFFFF_FFFF, 0]; self.gpu_insts.len()]
        };
        self.laps.lap("inst_words");
        // B-244 debug: bit 1 logs, once per set, every skinned instance's bounds and listed lights and every packed
        // dynamic light (position, radius, colour), to compare with the port's light passes on the same frame.
        if self.debug_bits & 2 != 0 {
            if !self.dyn_logged {
                self.dyn_logged = true;
                let ls = &self.lighting.dyn_frame.lights;
                eprintln!("n1ffi: dynlog {} lights, {} instances ({} skinned)", ls.len(), self.spheres.len(), skin.insts.len());
                for (i, l) in ls.iter().enumerate() {
                    eprintln!(
                        "n1ffi: dynlog light {i} pos ({:.1}, {:.1}, {:.1}) radius {:.1} rgb ({:.4}, {:.4}, {:.4}) falloff {:.2} spot {}",
                        l[0], l[1], l[2], 1.0 / l[3], l[4], l[5], l[6], l[7], l[12]
                    );
                }
                let skin_base = self.gpu_insts.len() - skin.insts.len();
                for (k, si) in skin.insts.iter().enumerate() {
                    let ii = skin_base + k;
                    let (c, r) = self.spheres[ii];
                    eprintln!(
                        "n1ffi: dynlog skin {k} bounds ({:.1}, {:.1}, {:.1}) r {:.1} env {} words {:08x} {}",
                        c[0], c[1], c[2], r, si.env as i32, inst_words[ii][0], inst_words[ii][1]
                    );
                }
            }
        } else {
            self.dyn_logged = false;
        }
        // B-230 debug A/B: bit 0 swaps in the other light colour mode (same lights, same order).
        let lights = if self.debug_bits & 1 != 0 && self.lighting.dyn_lights_alt.len() == self.lighting.dyn_frame.lights.len() {
            &self.lighting.dyn_lights_alt
        } else {
            &self.lighting.dyn_frame.lights
        };
        let lw: Vec<u8> = bytemuck::cast_slice(&lights.concat()).to_vec();
        // B-245 debug A/B: bit 8 (256) swaps in the light environments packed in the other colour mode.
        let envs = if self.debug_bits & 256 != 0 && self.lighting.envs_alt.len() == self.lighting.dyn_frame.envs.len() {
            &self.lighting.envs_alt
        } else {
            &self.lighting.dyn_frame.envs
        };
        let ew: Vec<u8> = bytemuck::cast_slice(&envs.concat()).to_vec();
        self.laps.lap("light_pack");
        let n_inst = self.gpu_insts.len().max(1) as u64;
        let inst_at = DYN_AT;
        let idyn_at = align(inst_at + n_inst * std::mem::size_of::<GpuInstance>() as u64);
        let lights_at = align(idyn_at + n_inst * 8);
        let envs_at = align(lights_at + (lw.len() as u64).max(64));
        let bones_at = align(envs_at + (ew.len() as u64).max(208));
        let end_bones = bones_at + skin.bones.len().max(1) as u64 * 64;
        let (rw, rh) = (target.rect.extent.width.max(1), target.rect.extent.height.max(1));
        // B-189: this frame's shadows, after the bones.
        self.select_shadows(&swz_vp(view_proj), rw, rh);
        self.shadow_receivers();
        self.laps.lap("shadows_select");
        let shadows_at = align(end_bones);
        let end = shadows_at + self.shadows.frame.len().max(1) as u64 * std::mem::size_of::<shadow::GpuShadow>() as u64;
        let si = self.slot(end, serial, completed)?;
        self.laps.lap("slot");

        self.ensure_targets(rw, rh)?;
        self.laps.lap("ensure_targets");
        let eye_y = lightfeed::swz(eye);
        let env = if self.debug_bits & 2048 != 0 { &self.lighting.env_alt } else { &self.lighting.env };
        let mut globals = env.globals(
            swz_vp(view_proj),
            eye_y,
            self.time(),
        );
        let post_on = self.knobs.post;
        let pp = &self.lighting.post;
        let grade_on = post_on && pp.grade.is_some();
        let blur_on = post_on && pp.blurs();
        let (bw, bh) = ((rw / 4).max(1), (rh / 4).max(1));
        let qt = [1.0 / bw as f32, 1.0 / bh as f32];
        let ft = [1.0 / rw as f32, 1.0 / rh as f32];
        // Kernels are in quarter-res texels of the game's 1280x720.
        let kscale = rh as f32 / 720.0;
        let ka = crate::light::kernel_taps(pp.uber.blur_kernel, kscale);
        let kb = crate::light::kernel_taps(pp.uber.bloom_kernel, kscale);
        let off = [target.rect.offset.x as f32, target.rect.offset.y as f32];
        let blk = |texel, dir, taps: &[(f32, f32)], offset| {
            pp.block(&crate::light::PostBlock {
                texel,
                dir,
                taps,
                unorm: self.encode_srgb,
                near: self.near,
                offset,
                fade: self.fade,
            })
        };
        // B-266: post materials after the composite (which then writes linear into a full-res target).
        let mats = if post_on { self.post_mat_passes() } else { vec![] };
        let names: Vec<&'static str> = mats.iter().map(|m| m.0).collect();
        if names != self.post_mats_last {
            eprintln!(
                "n1ffi: post mats {:?} (params {:?}, time {:.2})",
                names,
                mats.iter().map(|m| m.1).collect::<Vec<_>>(),
                self.time()
            );
            self.post_mats_last = names;
        }
        let time = self.time();
        let mut blocks = vec![
            blk(ft, [0.0, 0.0], &[], [0.0, 0.0]), // grade
            blk(ft, [0.0, 0.0], &[], [0.0, 0.0]), // down
            blk(qt, [1.0, 0.0], &ka, [0.0, 0.0]), // blur A h
            blk(qt, [0.0, 1.0], &ka, [0.0, 0.0]), // blur A v
            blk(qt, [0.0, 1.0], &kb, [0.0, 0.0]), // blur B: the port's second bloom pass reads A (one direction only)
            blk(ft, [0.0, 0.0], &[], off),        // composite
            blk(ft, [0.0, 0.0], &[], off),        // copy
            blk(ft, [0.0, 0.0], &[], [0.0, 0.0]), // linear composite
        ];
        for (j, (_, params, _, pwl)) in mats.iter().enumerate() {
            let last = j + 1 == mats.len();
            let mut o = blk(ft, [0.0, 0.0], &[], if last { off } else { [0.0, 0.0] });
            o[4][3] = last as u32 as f32;
            o[5] = *params;
            // g1.w: real time (wraps hourly, keeping f32 precision), for animation that runs while paused
            let real = (self.post_real0.elapsed().as_secs_f64() % 3600.0) as f32;
            o[6] = [time, pwl[0] as u32 as f32, pwl[1] as u32 as f32, real];
            blocks.push(o);
        }
        {
            let inst: Vec<u8> = bytemuck::cast_slice(&self.gpu_insts).to_vec();
            let iw: Vec<u8> = bytemuck::cast_slice(&inst_words).to_vec();
            let b = self.slots[si].buf.bytes();
            let put = |b: &mut [u8], at: u64, src: &[u8]| {
                b[at as usize..at as usize + src.len()].copy_from_slice(src)
            };
            globals.tune2[3] = if self.knobs.shadow_apply { self.shadows.frame.len() as f32 } else { 0.0 };
            // B-244 debug A/B: bit 2 shows the lighting alone (`BOTS_N1_ENVP=irr`), bit 3 drops the modulated shadows,
            // bit 4 the dynamic lights (gain 0).
            if self.debug_bits & 4 != 0 {
                globals.tune[3] = 2.0;
            }
            if self.debug_bits & 8 != 0 {
                globals.tune2[3] = 0.0;
            }
            if self.debug_bits & 16 != 0 {
                globals.tune3[2] = 0.0;
            }
            // B-244 flags (scene.wgsl `amb_up.w`); debug bits 5, 6 and 7 turn each off for a paired A/B (224: all).
            let post = self.knobs.dyn_post && self.debug_bits & 32 == 0;
            let lv = self.knobs.dyn_lightvec && self.debug_bits & 64 == 0;
            let spec = self.knobs.dyn_spec && self.debug_bits & 128 == 0;
            // B-245 flag bit 3: light-environment key specular as the port's directional pass (debug bit 9, 512, off).
            let env_spec = self.knobs.env_spec && self.debug_bits & 512 == 0;
            // B-261 flag bit 4: tangent-space ReflectionVector (debug bit 12, 4096, off).
            let refl_ts = self.knobs.refl_ts && self.debug_bits & 4096 == 0;
            globals.amb_up[3] =
                (post as u32 | (lv as u32) << 1 | (spec as u32) << 2 | (env_spec as u32) << 3 | (refl_ts as u32) << 4) as f32;
            put(b, GLOBALS_AT, bytemuck::bytes_of(&globals));
            for (k, blk) in blocks.iter().enumerate() {
                put(b, POST_AT + 256 * k as u64, bytemuck::cast_slice(blk));
            }
            put(b, inst_at, &inst);
            put(b, idyn_at, &iw);
            put(b, lights_at, &lw);
            put(b, envs_at, &ew);
            put(b, bones_at, bytemuck::cast_slice(&skin.bones));
            put(b, shadows_at, bytemuck::cast_slice(&self.shadows.frame));
        }
        self.laps.lap("globals_put");
        // Set 0: offsets move with the counts, so it is rewritten every frame (the slot's last use has completed).
        {
            let s = &self.slots[si];
            let bi = |offset: u64, range: u64| {
                [vk::DescriptorBufferInfo {
                    buffer: s.buf.buffer,
                    offset,
                    range,
                }]
            };
            let infos = [
                bi(GLOBALS_AT, std::mem::size_of::<Globals>() as u64),
                bi(inst_at, idyn_at - inst_at),
                [vk::DescriptorBufferInfo {
                    buffer: self.light.buffer,
                    offset: 0,
                    range: vk::WHOLE_SIZE,
                }],
                bi(lights_at, envs_at - lights_at),
                bi(idyn_at, lights_at - idyn_at),
                bi(envs_at, bones_at - envs_at),
                bi(bones_at, shadows_at - bones_at),
                bi(shadows_at, end - shadows_at),
            ];
            let writes: Vec<vk::WriteDescriptorSet> = infos
                .iter()
                .enumerate()
                .map(|(k, info)| {
                    vk::WriteDescriptorSet::default()
                        .dst_set(s.set)
                        .dst_binding(k as u32)
                        .descriptor_type(if k == 0 {
                            vk::DescriptorType::UNIFORM_BUFFER
                        } else {
                            vk::DescriptorType::STORAGE_BUFFER
                        })
                        .buffer_info(info)
                })
                .collect();
            let simg = [vk::DescriptorImageInfo {
                sampler: vk::Sampler::null(),
                image_view: self.shadows.array_view,
                image_layout: vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            }];
            let ssmp = [vk::DescriptorImageInfo {
                sampler: self.shadows.sampler,
                image_view: vk::ImageView::null(),
                image_layout: vk::ImageLayout::UNDEFINED,
            }];
            let mut writes = writes;
            writes.push(
                vk::WriteDescriptorSet::default()
                    .dst_set(s.set)
                    .dst_binding(8)
                    .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                    .image_info(&simg),
            );
            writes.push(
                vk::WriteDescriptorSet::default()
                    .dst_set(s.set)
                    .dst_binding(9)
                    .descriptor_type(vk::DescriptorType::SAMPLER)
                    .image_info(&ssmp),
            );
            unsafe { self.device.update_descriptor_sets(&writes, &[]) };
        }
        self.laps.lap("desc_set0");
        // Post sets: (uniform block, source, bloom, bloom2) per pass; binding 5 is the depth buffer.
        {
            let hdr = self.hdr.as_ref().unwrap().view;
            let scene = if grade_on { self.graded.as_ref().unwrap().view } else { hdr };
            let dv = self.depth.as_ref().unwrap().view;
            let [q0, q1, q2] = self.bloom.as_ref().map(|x| [x[0].view, x[1].view, x[2].view]).unwrap();
            let mut srcs = vec![
                (hdr, q0, q0),   // grade
                (scene, q0, q0), // down -> q0
                (q0, q0, q0),    // blur A h -> q1
                (q1, q1, q1),    // blur A v -> q0
                (q0, q0, q0),    // blur B -> q2
                (scene, q0, q2), // composite
                (hdr, q0, q0),   // copy
            ];
            if !mats.is_empty() {
                // B-266: the linear composite reads the scene and writes the other full-res target; the material
                // passes ping-pong between the two, the last writing the output.
                let other = if grade_on { hdr } else { self.graded.as_ref().unwrap().view };
                srcs.push((scene, q0, q2));
                let mut cur = other;
                for (_, _, ids, _) in &mats {
                    srcs.push((cur, self.tex_view(ids[0], TEX_BLACK), self.tex_view(ids[1], TEX_BLACK)));
                    cur = if cur == other { scene } else { other };
                }
            }
            let s = &self.slots[si];
            for (k, (src, bl, bl2)) in srcs.iter().enumerate() {
                let ub = [vk::DescriptorBufferInfo {
                    buffer: s.buf.buffer,
                    offset: POST_AT + 256 * k as u64,
                    range: 256,
                }];
                let smp = [vk::DescriptorImageInfo {
                    sampler: self.post_sampler,
                    image_view: vk::ImageView::null(),
                    image_layout: vk::ImageLayout::UNDEFINED,
                }];
                let wrap = [vk::DescriptorImageInfo {
                    sampler: self.sampler,
                    image_view: vk::ImageView::null(),
                    image_layout: vk::ImageLayout::UNDEFINED,
                }];
                let img = |v| {
                    [vk::DescriptorImageInfo {
                        sampler: vk::Sampler::null(),
                        image_view: v,
                        image_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                    }]
                };
                let (i1, i2, i3) = (img(*src), img(*bl), img(*bl2));
                let i4 = [vk::DescriptorImageInfo {
                    sampler: vk::Sampler::null(),
                    image_view: dv,
                    image_layout: vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                }];
                let set = s.post_sets[k];
                let writes = [
                    vk::WriteDescriptorSet::default()
                        .dst_set(set)
                        .dst_binding(0)
                        .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER)
                        .buffer_info(&ub),
                    vk::WriteDescriptorSet::default()
                        .dst_set(set)
                        .dst_binding(1)
                        .descriptor_type(vk::DescriptorType::SAMPLER)
                        .image_info(&smp),
                    vk::WriteDescriptorSet::default()
                        .dst_set(set)
                        .dst_binding(2)
                        .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                        .image_info(&i1),
                    vk::WriteDescriptorSet::default()
                        .dst_set(set)
                        .dst_binding(3)
                        .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                        .image_info(&i2),
                    vk::WriteDescriptorSet::default()
                        .dst_set(set)
                        .dst_binding(4)
                        .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                        .image_info(&i3),
                    vk::WriteDescriptorSet::default()
                        .dst_set(set)
                        .dst_binding(5)
                        .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                        .image_info(&i4),
                    vk::WriteDescriptorSet::default()
                        .dst_set(set)
                        .dst_binding(6)
                        .descriptor_type(vk::DescriptorType::SAMPLER)
                        .image_info(&wrap),
                ];
                unsafe { self.device.update_descriptor_sets(&writes, &[]) };
            }
        }
        self.laps.lap("desc_post");
        // Particles (B-186): globals and instances into this slot's buffer.
        let pf = std::mem::take(&mut self.pframe);
        let pg_sprites = pf.has_sprites();
        let pg_meshes = pf.has_meshes();
        if pg_sprites || pg_meshes {
            self.pg_stage(si, &pf, view_proj, eye, rw, rh)?;
        }
        self.laps.lap("pg_stage");
        // Trees (B-187).
        let tf = std::mem::take(&mut self.tframe);
        self.tg_stage(si, &tf, &globals, &pf)?;
        self.laps.lap("tg_stage");
        // Pipelines are built by the startup step and the workers; this frame draws what is ready.
        self.pipe_pending = self.store.backlog() as u32;
        self.pg_resolve(&pf);
        self.tg_pipelines(&tf);
        self.laps.lap("pg_tg_pipes");
        let (p_grade, p_down, p_blur, p_comp, p_copy) = (
            self.post_pipe("fs_grade", HDR_FORMAT)?,
            self.post_pipe("fs_down", HDR_FORMAT)?,
            self.post_pipe("fs_blur", HDR_FORMAT)?,
            self.post_pipe("fs_comp", self.color_format)?,
            self.post_pipe("fs_copy", self.color_format)?,
        );
        let mut p_mats = Vec::with_capacity(mats.len());
        if !mats.is_empty() {
            p_mats.push(self.post_pipe("fs_comp_lin", HDR_FORMAT)?);
            for (j, (fs, ..)) in mats.iter().enumerate() {
                let fmt = if j + 1 == mats.len() { self.color_format } else { HDR_FORMAT };
                p_mats.push(self.post_pipe(fs, fmt)?);
            }
        }
        self.laps.lap("post_pipe");

        // B-189: shadow depth passes (before the scene pass samples them).
        let set0 = self.slots[si].set;
        if let Some(g) = self.gprof.as_mut() {
            g.begin(&self.device, cmd);
        }
        st.draws += self.record_shadows(cmd, set0);
        self.laps.lap("record_shadows");
        if let Some(g) = &self.gprof {
            g.mark(&self.device, cmd, 1);
        }

        let d = &self.device;
        let hdr = self.hdr.as_ref().unwrap();
        let depth = self.depth.as_ref().unwrap();
        let bloom = self.bloom.as_ref().unwrap();
        let graded = self.graded.as_ref().unwrap();
        let color = vk::ImageAspectFlags::COLOR;
        let barrier = |stage_src, stage_dst, b: &[vk::ImageMemoryBarrier]| unsafe {
            d.cmd_pipeline_barrier(cmd, stage_src, stage_dst, vk::DependencyFlags::empty(), &[], &[], b)
        };
        let frag = vk::PipelineStageFlags::FRAGMENT_SHADER;
        let cao = vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT;
        let tests =
            vk::PipelineStageFlags::EARLY_FRAGMENT_TESTS | vk::PipelineStageFlags::LATE_FRAGMENT_TESTS;
        let to_att = |img| {
            layout_barrier(
                img,
                color,
                vk::ImageLayout::UNDEFINED,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                vk::AccessFlags::empty(),
                vk::AccessFlags::COLOR_ATTACHMENT_WRITE,
            )
        };
        let to_read = |img| {
            layout_barrier(
                img,
                color,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                vk::AccessFlags::COLOR_ATTACHMENT_WRITE,
                vk::AccessFlags::SHADER_READ,
            )
        };
        let viewport = |x: f32, y: f32, w: u32, h: u32| unsafe {
            d.cmd_set_viewport(
                cmd,
                0,
                &[vk::Viewport {
                    x,
                    y,
                    width: w as f32,
                    height: h as f32,
                    min_depth: 0.0,
                    max_depth: 1.0,
                }],
            );
        };
        let full = |w, h| vk::Rect2D {
            offset: vk::Offset2D { x: 0, y: 0 },
            extent: vk::Extent2D {
                width: w,
                height: h,
            },
        };
        unsafe {
            // ---- scene -> HDR
            barrier(frag, cao, &[to_att(hdr.image)]);
            // Source includes the fragment stage: the last frame's sprite pass and post chain sample this depth
            // (write-after-read; the layout change from UNDEFINED discards and rewrites it).
            barrier(
                tests | frag,
                tests,
                &[layout_barrier(
                    depth.image,
                    vk::ImageAspectFlags::DEPTH,
                    vk::ImageLayout::UNDEFINED,
                    vk::ImageLayout::DEPTH_ATTACHMENT_OPTIMAL,
                    vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE,
                    vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_READ
                        | vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE,
                )],
            );
            let cc = target.clear.unwrap_or([0.0, 0.0, 0.0, 1.0]);
            let clear = [
                srgb_to_linear(cc[0]),
                srgb_to_linear(cc[1]),
                srgb_to_linear(cc[2]),
                1.0,
            ];
            let catt = [vk::RenderingAttachmentInfo::default()
                .image_view(hdr.view)
                .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
                .load_op(vk::AttachmentLoadOp::CLEAR)
                .store_op(vk::AttachmentStoreOp::STORE)
                .clear_value(vk::ClearValue {
                    color: vk::ClearColorValue { float32: clear },
                })];
            let datt = vk::RenderingAttachmentInfo::default()
                .image_view(depth.view)
                .image_layout(vk::ImageLayout::DEPTH_ATTACHMENT_OPTIMAL)
                .load_op(vk::AttachmentLoadOp::CLEAR)
                .store_op(if pg_sprites {
                    vk::AttachmentStoreOp::STORE
                } else {
                    vk::AttachmentStoreOp::DONT_CARE
                })
                .clear_value(vk::ClearValue {
                    depth_stencil: vk::ClearDepthStencilValue {
                        depth: 0.0,
                        stencil: 0,
                    },
                });
            d.cmd_begin_rendering(
                cmd,
                &vk::RenderingInfo::default()
                    .render_area(full(rw, rh))
                    .layer_count(1)
                    .color_attachments(&catt)
                    .depth_attachment(&datt),
            );
            viewport(0.0, 0.0, rw, rh);
            d.cmd_set_scissor(cmd, 0, &[full(rw, rh)]);
            let (mut cur_key, mut cur_mat, mut cur_lm, mut cur_mesh) =
                (None, u32::MAX, vk::DescriptorSet::null(), (false, u32::MAX));
            let mut cur_layout = vk::PipelineLayout::null();
            let mut cur_pipe = vk::Pipeline::null();
            for dr in &self.draws {
                let sk = SceneKey {
                    shader: dr.key.shader,
                    blend: dr.key.blend,
                    skin: dr.key.skin,
                    cull: if self.dyn_cull { 0 } else { dr.key.two_sided as u8 | (dr.key.mirrored as u8) << 1 },
                };
                let Some(&pipe) = self.ready.get(&sk) else { continue };
                if cur_key != Some(dr.key) {
                    if pipe != cur_pipe {
                        d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pipe);
                        cur_pipe = pipe;
                    }
                    if self.dyn_cull {
                        d.cmd_set_cull_mode(
                            cmd,
                            if dr.key.two_sided { vk::CullModeFlags::NONE } else { vk::CullModeFlags::BACK },
                        );
                        d.cmd_set_front_face(
                            cmd,
                            if dr.key.mirrored { vk::FrontFace::CLOCKWISE } else { vk::FrontFace::COUNTER_CLOCKWISE },
                        );
                    }
                    let ntex = self.store.ntex_of(dr.key.shader);
                    let layout = self.store.mat_layout(ntex).map(|l| l.1).unwrap_or(cur_layout);
                    if layout != cur_layout {
                        // Set 0 and 1 have the same layout in every pipeline layout, so they stay bound; binding
                        // again after a layout switch is cheap and keeps validation layers quiet.
                        d.cmd_bind_descriptor_sets(
                            cmd,
                            vk::PipelineBindPoint::GRAPHICS,
                            layout,
                            0,
                            &[self.slots[si].set],
                            &[],
                        );
                        cur_layout = layout;
                        cur_lm = vk::DescriptorSet::null();
                        cur_mat = u32::MAX;
                    }
                    cur_key = Some(dr.key);
                    st.pipeline_changes += 1;
                }
                if cur_lm != dr.lm {
                    d.cmd_bind_descriptor_sets(
                        cmd,
                        vk::PipelineBindPoint::GRAPHICS,
                        cur_layout,
                        1,
                        &[dr.lm],
                        &[],
                    );
                    cur_lm = dr.lm;
                }
                if cur_mat != dr.mat {
                    d.cmd_bind_descriptor_sets(
                        cmd,
                        vk::PipelineBindPoint::GRAPHICS,
                        cur_layout,
                        2,
                        &[self.materials[dr.mat as usize].set],
                        &[],
                    );
                    cur_mat = dr.mat;
                    st.bind_changes += 1;
                }
                if cur_mesh != (dr.key.skin, dr.mesh) {
                    let m = if dr.key.skin {
                        &self.skin_meshes[dr.mesh as usize]
                    } else {
                        &self.meshes[dr.mesh as usize]
                    };
                    d.cmd_bind_vertex_buffers(cmd, 0, &[m.vbuf.buffer], &[0]);
                    d.cmd_bind_index_buffer(cmd, m.ibuf.buffer, 0, vk::IndexType::UINT32);
                    cur_mesh = (dr.key.skin, dr.mesh);
                }
                d.cmd_draw_indexed(cmd, dr.index_count, 1, dr.first_index, 0, dr.instance);
                st.draws += 1;
                st.triangles += dr.index_count as u64 / 3;
            }
            if let Some(g) = &self.gprof {
                g.mark(d, cmd, 2);
            }
            self.laps.lap("scene_draws");
            st.draws += self.tg_record(cmd, si, &tf);
            if pg_meshes {
                st.draws += self.pg_record(cmd, si, &pf, true);
            }
            d.cmd_end_rendering(cmd);
            self.laps.lap("tg_pg_record");
            if let Some(g) = &self.gprof {
                g.mark(d, cmd, 3);
            }
            if pg_sprites {
                // Sprite pass: same HDR target, depth bound read-only (tested, sampled for soft particles).
                let dep = layout_barrier(
                    depth.image,
                    vk::ImageAspectFlags::DEPTH,
                    vk::ImageLayout::DEPTH_ATTACHMENT_OPTIMAL,
                    vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE,
                    vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_READ | vk::AccessFlags::SHADER_READ,
                );
                let mem = [vk::MemoryBarrier::default()
                    .src_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE)
                    .dst_access_mask(
                        vk::AccessFlags::COLOR_ATTACHMENT_READ | vk::AccessFlags::COLOR_ATTACHMENT_WRITE,
                    )];
                d.cmd_pipeline_barrier(
                    cmd,
                    cao | tests,
                    cao | tests | frag,
                    vk::DependencyFlags::empty(),
                    &mem,
                    &[],
                    &[dep],
                );
                let catt2 = [vk::RenderingAttachmentInfo::default()
                    .image_view(hdr.view)
                    .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
                    .load_op(vk::AttachmentLoadOp::LOAD)
                    .store_op(vk::AttachmentStoreOp::STORE)];
                let datt2 = vk::RenderingAttachmentInfo::default()
                    .image_view(depth.view)
                    .image_layout(vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL)
                    .load_op(vk::AttachmentLoadOp::LOAD)
                    .store_op(vk::AttachmentStoreOp::NONE);
                d.cmd_begin_rendering(
                    cmd,
                    &vk::RenderingInfo::default()
                        .render_area(full(rw, rh))
                        .layer_count(1)
                        .color_attachments(&catt2)
                        .depth_attachment(&datt2),
                );
                viewport(0.0, 0.0, rw, rh);
                d.cmd_set_scissor(cmd, 0, &[full(rw, rh)]);
                st.draws += self.pg_record(cmd, si, &pf, false);
                d.cmd_end_rendering(cmd);
            }
            barrier(cao, frag, &[to_read(hdr.image)]);
            if let Some(g) = &self.gprof {
                g.mark(d, cmd, 4);
            }

            // ---- post
            let pass = |view: vk::ImageView,
                        layout: vk::ImageLayout,
                        rect: vk::Rect2D,
                        pipe: vk::Pipeline,
                        set: vk::DescriptorSet| {
                let att = [vk::RenderingAttachmentInfo::default()
                    .image_view(view)
                    .image_layout(layout)
                    .load_op(vk::AttachmentLoadOp::DONT_CARE)
                    .store_op(vk::AttachmentStoreOp::STORE)];
                d.cmd_begin_rendering(
                    cmd,
                    &vk::RenderingInfo::default()
                        .render_area(rect)
                        .layer_count(1)
                        .color_attachments(&att),
                );
                viewport(
                    rect.offset.x as f32,
                    rect.offset.y as f32,
                    rect.extent.width,
                    rect.extent.height,
                );
                d.cmd_set_scissor(cmd, 0, &[rect]);
                d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pipe);
                d.cmd_bind_descriptor_sets(
                    cmd,
                    vk::PipelineBindPoint::GRAPHICS,
                    self.post_pipe_layout,
                    0,
                    &[set],
                    &[],
                );
                d.cmd_draw(cmd, 3, 1, 0, 0);
                d.cmd_end_rendering(cmd);
            };
            let ps = self.slots[si].post_sets;
            let catt_layout = vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL;
            let bq = full(bloom[0].width, bloom[0].height);
            // Depth: attachment (or read-only after the sprite pass) -> sampled by the post chain.
            let dep = layout_barrier(
                depth.image,
                vk::ImageAspectFlags::DEPTH,
                if pg_sprites {
                    vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL
                } else {
                    vk::ImageLayout::DEPTH_ATTACHMENT_OPTIMAL
                },
                vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE,
                vk::AccessFlags::SHADER_READ,
            );
            if post_on {
                barrier(tests, frag, &[dep]);
                if grade_on {
                    barrier(frag, cao, &[to_att(graded.image)]);
                    pass(graded.view, catt_layout, full(rw, rh), p_grade, ps[0]);
                    barrier(cao, frag, &[to_read(graded.image)]);
                }
                if blur_on {
                    barrier(frag, cao, &[to_att(bloom[0].image)]);
                    pass(bloom[0].view, catt_layout, bq, p_down, ps[1]);
                    barrier(cao, frag | cao, &[to_read(bloom[0].image), to_att(bloom[1].image)]);
                    pass(bloom[1].view, catt_layout, bq, p_blur, ps[2]);
                    barrier(cao | frag, frag | cao, &[to_read(bloom[1].image), to_att(bloom[0].image)]);
                    pass(bloom[0].view, catt_layout, bq, p_blur, ps[3]);
                    barrier(cao | frag, frag | cao, &[to_read(bloom[0].image), to_att(bloom[2].image)]);
                    pass(bloom[2].view, catt_layout, bq, p_blur, ps[4]);
                    barrier(cao, frag, &[to_read(bloom[2].image)]);
                } else {
                    // The composite shader binds the bloom targets even with blur off (its use is behind a uniform
                    // branch, so nothing is read), but they must be in the descriptor's layout: validation's
                    // VUID-vkCmdDraw-None-09600 (an image sampled in UNDEFINED) on scenes without DOF/bloom.
                    let ro = |img| {
                        layout_barrier(
                            img,
                            color,
                            vk::ImageLayout::UNDEFINED,
                            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                            vk::AccessFlags::empty(),
                            vk::AccessFlags::SHADER_READ,
                        )
                    };
                    barrier(frag, frag, &[ro(bloom[0].image), ro(bloom[2].image)]);
                }
                if mats.is_empty() {
                    pass(target.view, target.layout, target.rect, p_comp, ps[5]);
                    if let Some(g) = &self.gprof {
                        g.mark(d, cmd, 5);
                    }
                } else {
                    // B-266: linear composite into the full-res target the composite doesn't read, then each post
                    // material, ping-ponging; the last writes the output with the gamma and screen fade.
                    let (scene_rt, other_rt) = if grade_on { (graded, hdr) } else { (hdr, graded) };
                    barrier(frag, cao, &[to_att(other_rt.image)]);
                    pass(other_rt.view, catt_layout, full(rw, rh), p_mats[0], ps[7]);
                    barrier(cao, frag, &[to_read(other_rt.image)]);
                    if let Some(g) = &self.gprof {
                        g.mark(d, cmd, 5);
                    }
                    let (mut cur, mut next) = (other_rt, scene_rt); // read, write
                    for j in 0..mats.len() {
                        let (pipe, set) = (p_mats[j + 1], ps[8 + j]);
                        if j + 1 == mats.len() {
                            pass(target.view, target.layout, target.rect, pipe, set);
                        } else {
                            barrier(frag, cao, &[to_att(next.image)]);
                            pass(next.view, catt_layout, full(rw, rh), pipe, set);
                            barrier(cao, frag, &[to_read(next.image)]);
                            std::mem::swap(&mut cur, &mut next);
                        }
                    }
                }
            } else {
                pass(target.view, target.layout, target.rect, p_copy, ps[6]);
                if let Some(g) = &self.gprof {
                    g.mark(d, cmd, 5);
                }
            }
            if let Some(g) = &self.gprof {
                g.mark(d, cmd, 6);
            }
        }
        self.laps.lap("post_pass");
        st.record_ms = t0.elapsed().as_secs_f32() * 1000.0;
        self.laps.report(
            "Renderer::record",
            st.record_ms,
            &format!(
                "(draws {} insts {} meshes {} mats {} slots {})",
                self.draws.len(),
                self.gpu_insts.len(),
                self.meshes.len(),
                self.materials.len(),
                self.slots.len()
            ),
        );
        self.stats = st;
        Ok(st)
    }

    /// Records pending texture copies into `cmd` on a frame without a scene (loading screen, preload hold). Same
    /// serial rules as `record`; returns the copies recorded.
    pub fn upload(&mut self, cmd: vk::CommandBuffer, serial: u64, completed: u64) -> Result<u32> {
        if serial < self.last_serial {
            bail!("serial {serial} before {}", self.last_serial);
        }
        self.retire(completed);
        let n = self.flush_uploads(cmd, serial);
        self.last_serial = serial;
        if self.knobs.light_grow {
            // B-264: the words this level's lights appended go to the GPU on loading frames, not all in the first scene frame.
            self.sync_light(cmd)?;
        }
        Ok(n)
    }

    /// Texture copies waiting for a command buffer, and the staging bytes they hold.
    pub fn pending_uploads(&self) -> (u32, u64) {
        (self.pending.len() as u32, self.pending.iter().map(|u| u.staging.cap).sum())
    }

    /// Uploads a skinned mesh (vertices with mesh bone indices); returns its id for `SkinDraw::mesh`.
    pub fn add_skin_mesh(&mut self, verts: &[SkinVertex], indices: &[u32]) -> Result<u32> {
        let vb: &[u8] = bytemuck::cast_slice(verts);
        let ib: &[u8] = bytemuck::cast_slice(indices);
        let mut vbuf = self.buffer(
            vb.len() as u64,
            vk::BufferUsageFlags::VERTEX_BUFFER,
            "n1 skin vertices",
        )?;
        vbuf.bytes()[..vb.len()].copy_from_slice(vb);
        let mut ibuf = self.buffer(
            ib.len() as u64,
            vk::BufferUsageFlags::INDEX_BUFFER,
            "n1 skin indices",
        )?;
        ibuf.bytes()[..ib.len()].copy_from_slice(ib);
        self.ctr.upload_bytes += (vb.len() + ib.len()) as u64;
        self.ctr.meshes_added += 1;
        self.skin_meshes.push(GpuMesh { vbuf, ibuf });
        Ok(self.skin_meshes.len() as u32 - 1)
    }

    /// (meshes, materials, textures, lights, shaders, pipelines)
    pub fn counts(&self) -> (usize, usize, usize, usize, usize, usize) {
        (
            self.mesh_info.len(),
            self.materials.len(),
            self.textures.len() - 3,
            self.lights.len(),
            self.store.counts().0,
            self.store.counts().1,
        )
    }

    fn try_add_texture(&mut self, t: &TextureDesc) -> Result<u32> {
        let supported = match t.format {
            TexFormat::Bc1 => self.bc_ok[0],
            TexFormat::Bc2 => self.bc_ok[1],
            TexFormat::Bc3 => self.bc_ok[2],
            TexFormat::Bc5 => self.bc_ok[3],
            _ => true,
        };
        if !supported {
            // No BC on this GPU (most Android GPUs): decode on the CPU to RGBA8 (BC5: RG8), keeping the encoding.
            let srgb = t.srgb && t.format != TexFormat::Bc5;
            let fmt = match (t.format, srgb) {
                (TexFormat::Bc5, _) => vk::Format::R8G8_UNORM,
                (_, true) => vk::Format::R8G8B8A8_SRGB,
                (_, false) => vk::Format::R8G8B8A8_UNORM,
            };
            let decoded: Vec<Vec<u8>> = t
                .mips
                .iter()
                .enumerate()
                .map_while(|(i, m)| {
                    crate::bcdec::decode_mip(t.format, (t.width >> i).max(1), (t.height >> i).max(1), m)
                })
                .collect();
            if decoded.is_empty() {
                self.unsupported_textures += 1;
                return Ok(if t.normal { TEX_NORMAL } else { TEX_GREY });
            }
            self.decoded_textures += 1;
            let mips: Vec<&[u8]> = decoded.iter().map(|v| v.as_slice()).collect();
            let id = self.upload_texture(fmt, t.width, t.height, &mips)?;
            self.textures[id as usize].gamma = t.gamma;
            return Ok(id);
        }
        let fmt = vk_format(t.format, t.srgb && t.format != TexFormat::Bc5);
        if t.format == TexFormat::R8 {
            let expanded: Vec<Vec<u8>> = t
                .mips
                .iter()
                .map(|m| m.iter().flat_map(|&g| [g, g, g, 255]).collect())
                .collect();
            let mips: Vec<&[u8]> = expanded.iter().map(|v| v.as_slice()).collect();
            let id = self.upload_texture(fmt, t.width, t.height, &mips)?;
            self.textures[id as usize].gamma = t.gamma;
            return Ok(id);
        }
        let id = self.upload_texture(fmt, t.width, t.height, t.mips)?;
        self.textures[id as usize].gamma = t.gamma;
        Ok(id)
    }

    fn try_add_mesh(&mut self, m: &MeshDesc) -> Result<u32> {
        self.try_add_mesh_at(m, false)
    }

    fn try_add_mesh_at(&mut self, m: &MeshDesc, reuse: bool) -> Result<u32> {
        let verts: Vec<Vertex> = m
            .positions
            .iter()
            .enumerate()
            .map(|(i, p)| {
                let t = m.tangents.get(i).copied().unwrap_or([1.0, 0.0, 0.0, 1.0]);
                Vertex {
                    pos: *p,
                    nrm: m.normals.get(i).copied().unwrap_or([0.0, 0.0, 1.0]),
                    // The Y/Z swizzle in the model matrix is a reflection: the bitangent sign flips.
                    tan: [t[0], t[1], t[2], -t[3]],
                    uv0: m.uvs.get(i).copied().unwrap_or([0.0, 0.0]),
                    uv1: m.uv1.get(i).copied().unwrap_or([0.0, 0.0]),
                    col: m.colors.get(i).copied().unwrap_or([255; 4]),
                }
            })
            .collect();
        let vb: &[u8] = bytemuck::cast_slice(&verts);
        let ib: &[u8] = bytemuck::cast_slice(m.indices);
        let mut vbuf = self.buffer(
            vb.len() as u64,
            vk::BufferUsageFlags::VERTEX_BUFFER,
            "n1 vertices",
        )?;
        vbuf.bytes()[..vb.len()].copy_from_slice(vb);
        let mut ibuf = self.buffer(
            ib.len() as u64,
            vk::BufferUsageFlags::INDEX_BUFFER,
            "n1 indices",
        )?;
        ibuf.bytes()[..ib.len()].copy_from_slice(ib);
        self.ctr.upload_bytes += (vb.len() + ib.len()) as u64;
        self.ctr.meshes_added += 1;
        let n = m.indices.len() as u32;
        let info = MeshInfo {
            vertex_count: m.positions.len() as u32,
            sections: m
                .sections
                .iter()
                .filter(|s| s.index_count > 0 && s.first_index + s.index_count <= n)
                .copied()
                .collect(),
            center: m.center,
            radius: m.radius,
        };
        // B-228: a decal mesh given back reuses its slot once the GPU is done with it.
        if reuse {
            let done = self.completed;
            if let Some(k) = self.free_meshes.iter().position(|&(s, _)| s <= done) {
                let (_, id) = self.free_meshes.swap_remove(k);
                self.meshes[id as usize] = GpuMesh { vbuf, ibuf };
                self.mesh_info[id as usize] = info;
                return Ok(id);
            }
        }
        self.meshes.push(GpuMesh { vbuf, ibuf });
        self.mesh_info.push(info);
        Ok(self.mesh_info.len() as u32 - 1)
    }

    /// B-228: a decal's clipped geometry (world space), in a reused slot when one is free.
    pub fn add_decal_mesh(&mut self, m: &MeshDesc) -> Option<u32> {
        match self.try_add_mesh_at(m, true) {
            Ok(id) => Some(id),
            Err(e) => {
                eprintln!("n1ffi: decal mesh upload failed: {e:#}");
                None
            }
        }
    }

    /// B-228: gives back a decal mesh (`add_decal_mesh`) and its own vertex light (`add_decal_light`, or NONE). Their
    /// slots are reused once the newest submission that could have drawn them has completed.
    pub fn free_decal(&mut self, mesh: u32, light: u32) {
        let serial = self.last_serial + 1;
        if let Some(gm) = self.meshes.get_mut(mesh as usize) {
            let old = std::mem::replace(gm, GpuMesh { vbuf: Buf::null(), ibuf: Buf::null() });
            self.mesh_info[mesh as usize].sections.clear();
            self.defer_at(serial, Garbage::Buffer(old.vbuf));
            self.defer_at(serial, Garbage::Buffer(old.ibuf));
            self.free_meshes.push((serial, mesh));
        }
        if let Some(LightInfo::Words { base, count, .. }) = self.lights.get(light as usize) {
            let (base, count) = (*base, *count);
            self.lights[light as usize] = LightInfo::Words { base: 0, count: u32::MAX, sc: [[0.0; 4]; 3] };
            self.free_words.push((serial, base, count));
            self.free_lights.push((serial, light));
        }
    }

    /// B-228: what a receiver's baked light is, for a decal on it: its vertex light words when it is vertex lit with
    /// `vertex_count` vertices (with the per-coefficient scale), else whether it is a lightmap (the decal then uses
    /// the same light id with the receiver's UV1).
    pub fn decal_light_source(&self, light: u32, vertex_count: u32) -> DecalLightSource<'_> {
        match self.lights.get(light as usize) {
            Some(LightInfo::Words { base, count, sc }) if *count == vertex_count => {
                let b = *base as usize * 3;
                match self.light_cpu.get(b..b + *count as usize * 3) {
                    Some(w) => DecalLightSource::Words(w, *sc),
                    None => DecalLightSource::None,
                }
            }
            Some(LightInfo::Map { .. }) => DecalLightSource::Map,
            _ => DecalLightSource::None,
        }
    }

    /// B-228: a vertex light for a decal mesh (three words per vertex), in a reused range when one fits.
    pub fn add_decal_light(&mut self, words: &[u32], sc: [[f32; 4]; 3]) -> u32 {
        let count = (words.len() / 3) as u32;
        let done = self.completed;
        let base = match self.free_words.iter().position(|&(s, _, n)| s <= done && n >= count) {
            Some(k) => {
                let (s, base, n) = self.free_words.swap_remove(k);
                if n > count {
                    self.free_words.push((s, base + count, n - count));
                }
                let from = base as usize * 3;
                self.light_cpu[from..from + words.len()].copy_from_slice(words);
                self.light_patches.push((from, from + words.len()));
                base
            }
            None => self.push_words(words),
        };
        let info = LightInfo::Words { base, count, sc };
        if let Some(k) = self.free_lights.iter().position(|&(s, _)| s <= done) {
            let (_, id) = self.free_lights.swap_remove(k);
            self.lights[id as usize] = info;
            return id;
        }
        self.lights.push(info);
        self.lights.len() as u32 - 1
    }

    /// A flat grey material for primitives whose own material isn't known (yet).
    pub fn grey_material(&mut self) -> u32 {
        if self.grey_mat == NONE {
            self.grey_mat = self.add_material(&MaterialDesc {
                texture: NONE,
                blend: Blend::Opaque,
                two_sided: false,
                def: None,
                textures: &[],
                unpack: &[],
            });
        }
        self.grey_mat
    }

    fn push_words(&mut self, words: &[u32]) -> u32 {
        let base = (self.light_cpu.len() / 3) as u32;
        self.light_cpu.extend_from_slice(words);
        base
    }
}

/// What `sync_light` does with the vertex-light buffer (B-264): the new capacity if the words (`len_words` of them, 4 bytes
/// each) no longer fit `cap` bytes, the words the GPU copies from the old buffer, and where the CPU copy starts.
#[derive(Debug, PartialEq, Eq)]
struct LightSyncPlan {
    grow_to: Option<u64>,
    gpu_copy_words: usize,
    cpu_from: usize,
}

fn light_sync_plan(len_words: usize, dirty_from: usize, cap: u64, fast: bool) -> LightSyncPlan {
    let need = len_words as u64 * 4;
    if need <= cap {
        return LightSyncPlan { grow_to: None, gpu_copy_words: 0, cpu_from: dirty_from };
    }
    let mut new_cap = cap.max(1 << 20);
    while new_cap < need {
        new_cap *= 2;
    }
    // The old buffer holds `dirty_from` current words (the rest were never written to it).
    let keep = if fast { dirty_from.min(len_words) } else { 0 };
    LightSyncPlan { grow_to: Some(new_cap), gpu_copy_words: keep, cpu_from: keep }
}

/// Legacy single-colour vertex light as three identical words (bytes A, R, G, B; the shader reads `.yzw`), scale 4.
fn legacy_word(c: [f32; 3]) -> u32 {
    let q = |v: f32| ((v / 4.0).clamp(0.0, 1.0) * 255.0 + 0.5) as u32;
    255 | q(c[0]) << 8 | q(c[1]) << 16 | q(c[2]) << 24
}

fn sc4(s: [[f32; 3]; 3]) -> [[f32; 4]; 3] {
    s.map(|c| [c[0], c[1], c[2], 0.0])
}

impl Backend for Renderer {
    fn add_texture(&mut self, t: &TextureDesc) -> u32 {
        self.try_add_texture(t).unwrap_or_else(|e| {
            eprintln!("n1ffi: texture upload failed: {e:#}");
            TEX_GREY
        })
    }
    fn add_material(&mut self, m: &MaterialDesc) -> u32 {
        match self.try_add_material(m) {
            Ok(id) => id,
            Err(e) => {
                eprintln!("n1ffi: material failed: {e:#}");
                NONE
            }
        }
    }
    fn remove_material(&mut self, id: u32) {
        // Reused once the newest submission that could have drawn it has completed (the resolver only releases
        // materials no instance named for many frames, so that is long past by then).
        if (id as usize) < self.materials.len() && id != self.grey_mat && !self.free_mats.iter().any(|x| x.1 == id) {
            self.free_mats.push((self.last_serial + 1, id));
        }
    }
    fn add_mesh(&mut self, m: &MeshDesc) -> u32 {
        // A failed upload must still return an id the draw list can index: add an empty mesh then.
        match self.try_add_mesh(m) {
            Ok(id) => id,
            Err(e) => {
                eprintln!("n1ffi: mesh upload failed: {e:#}");
                let empty = MeshDesc {
                    positions: &[[0.0; 3]],
                    uvs: &[],
                    uv1: &[],
                    normals: &[],
                    tangents: &[],
                    colors: &[],
                    indices: &[0, 0, 0],
                    sections: &[],
                    center: [0.0; 3],
                    radius: 0.0,
                };
                self.try_add_mesh(&empty).expect("empty mesh")
            }
        }
    }
    fn add_light(&mut self, colors: &[[f32; 3]]) -> u32 {
        let words: Vec<u32> = colors
            .iter()
            .flat_map(|c| {
                let w = legacy_word(*c);
                [w, w, w]
            })
            .collect();
        let base = self.push_words(&words);
        self.lights.push(LightInfo::Words {
            base,
            count: colors.len() as u32,
            sc: [[4.0, 4.0, 4.0, 0.0]; 3],
        });
        self.lights.len() as u32 - 1
    }
    fn add_component_light(&mut self, l: &ComponentLight, lightmaps: [u32; 3]) -> u32 {
        match l {
            ComponentLight::Legacy(c) => self.add_light(c),
            ComponentLight::Dir { words, scale } => {
                let base = self.push_words(&words[..words.len() / 3 * 3]);
                self.lights.push(LightInfo::Words {
                    base,
                    count: (words.len() / 3) as u32,
                    sc: sc4(*scale),
                });
                self.lights.len() as u32 - 1
            }
            ComponentLight::Map {
                scale,
                coord_scale,
                coord_bias,
                ..
            } => {
                if lightmaps[0] == NONE {
                    return NONE;
                }
                let t = lightmaps.map(|x| if x == NONE { TEX_BLACK } else { x });
                match self.lm_set(t) {
                    Ok(set) => {
                        self.lights.push(LightInfo::Map {
                            set,
                            sc: sc4(*scale),
                            xf: [coord_scale[0], coord_scale[1], coord_bias[0], coord_bias[1]],
                        });
                        self.lights.len() as u32 - 1
                    }
                    Err(e) => {
                        eprintln!("n1ffi: lightmap set failed: {e:#}");
                        NONE
                    }
                }
            }
        }
    }
}

impl Drop for Renderer {
    /// The host must have waited for the GPU to finish with everything recorded.
    fn drop(&mut self) {
        unsafe {
            let deferred = std::mem::take(&mut self.deferred);
            for (_, g) in deferred {
                self.free(g);
            }
            for u in std::mem::take(&mut self.pending) {
                self.free(Garbage::Buffer(u.staging));
            }
            if let Some(g) = self.gprof.take() {
                g.destroy(&self.device);
            }
            for s in std::mem::take(&mut self.slots) {
                self.free(Garbage::Buffer(s.buf));
                self.free(Garbage::Buffer(s.pbuf));
            }
            self.pg_drop_buffers();
            self.tg_drop_buffers();
            let light = std::mem::replace(&mut self.light, Buf::null());
            self.free(Garbage::Buffer(light));
            for m in std::mem::take(&mut self.meshes)
                .into_iter()
                .chain(std::mem::take(&mut self.skin_meshes))
            {
                self.free(Garbage::Buffer(m.vbuf));
                self.free(Garbage::Buffer(m.ibuf));
            }
            for m in std::mem::take(&mut self.materials) {
                self.free(Garbage::Buffer(m._params));
            }
            for mut t in std::mem::take(&mut self.textures) {
                if let Some(a) = t.alloc.take() {
                    self.free(Garbage::Image(t.image, t.view, a));
                }
            }
            let mut rts: Vec<Rt> = [self.hdr.take(), self.depth.take(), self.graded.take()].into_iter().flatten().collect();
            if let Some(b) = self.bloom.take() {
                rts.extend(b);
            }
            for r in rts {
                self.free(Garbage::Image(r.image, r.view, r.alloc));
            }
            // Workers first: a startup step still running then gives up instead of saving half a manifest (B-223).
            self.store.shutdown();
            crate::precompile::flush(&self.store);
            self.pg.destroy(&self.device);
            self.tg.destroy(&self.device);
            let pipes: Vec<vk::Pipeline> = self.post_pipes.drain().map(|x| x.1).collect();
            for p in pipes {
                self.device.destroy_pipeline(p, None);
            }
            for p in self.pools.drain(..) {
                self.device.destroy_descriptor_pool(p, None);
            }
            self.store.destroy();
            for s in [self.sampler, self.lm_sampler, self.post_sampler] {
                self.device.destroy_sampler(s, None);
            }
            self.device.destroy_pipeline_layout(self.post_pipe_layout, None);
            for l in [self.set0_layout, self.lm_layout, self.post_layout] {
                self.device.destroy_descriptor_set_layout(l, None);
            }
            self.device.destroy_shader_module(self.post_module, None);
            self.shadows.destroy(&self.device, self.allocator.as_mut().unwrap());
            self.allocator.take();
        }
    }
}

#[cfg(test)]
mod light_sync_tests {
    use super::*;

    #[test]
    fn fits_means_no_growth() {
        let p = light_sync_plan(1000, 400, 4000, true);
        assert_eq!(p, LightSyncPlan { grow_to: None, gpu_copy_words: 0, cpu_from: 400 });
    }

    #[test]
    fn growth_copies_the_synced_words_on_the_gpu() {
        // 27 MB of words in a 16 MB buffer, 20 MB synced before: 32 MB buffer, 20 MB by GPU copy, the CPU writes the rest.
        let (len, synced) = (27 << 18, 20 << 18);
        let p = light_sync_plan(len, synced, 16 << 20, true);
        assert_eq!(p, LightSyncPlan { grow_to: Some(32 << 20), gpu_copy_words: synced, cpu_from: synced });
    }

    #[test]
    fn growth_doubles_until_it_fits() {
        let p = light_sync_plan(100 << 18, 1 << 18, 4 << 20, true);
        assert_eq!(p.grow_to, Some(128 << 20)); // 100 MB of words -> 128 MB
        assert_eq!(p.gpu_copy_words, 1 << 18);
    }

    #[test]
    fn switch_off_rewrites_everything_from_the_cpu() {
        let p = light_sync_plan(27 << 18, 20 << 18, 16 << 20, false);
        assert_eq!(p, LightSyncPlan { grow_to: Some(32 << 20), gpu_copy_words: 0, cpu_from: 0 });
    }

    #[test]
    fn nothing_synced_yet_copies_nothing() {
        let p = light_sync_plan(5 << 18, 0, 4 << 20, true);
        assert_eq!(p, LightSyncPlan { grow_to: Some(8 << 20), gpu_copy_words: 0, cpu_from: 0 });
    }
}
