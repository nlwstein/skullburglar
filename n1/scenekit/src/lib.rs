//! scenekit: the GPU-API-independent half of the live UE3 scene renderer.
//!
//! - **Types** the renderers and the C ABI share: resource descriptions (`TextureDesc`, `MeshDesc`, ...), the
//!   per-frame `Instance`, `FrameStats`.
//! - **`SceneCore`**: the CPU-side record of what was uploaded (mesh bounds and sections, materials, baked-light
//!   ranges) and the per-frame draw-list builder (frustum culling, material overrides, sort by state, blended back
//!   to front). A backend (livegfx on wgpu, n1ffi on ash) keeps one and only turns its `DrawList` into API calls.
//! - **`Backend`**: what the resolver needs from a renderer to upload assets.
//! - **`Resolver`** (resolver.rs): scene paths -> stable handles -> loaded (sync or on loader threads) -> uploaded.
//! - **The scene feed's JSON** and the camera maths.
//!
//! Conventions: UE3 space and UE3 `FMatrix` values as stored (16 floats, row-major, row vectors, `v' = v * M`).
//! Shaders read them as column-major, i.e. transposed, so `M * v` there is UE3's `v * M`. Reverse-Z.

use bytemuck::{Pod, Zeroable};

mod resolver;
pub use resolver::*;

pub const NONE: u32 = u32::MAX;
/// A material override that means "don't draw this section": a run-time material variant (a path with `?` parameters)
/// whose own material isn't loaded yet. Drawing the mesh's cooked material instead put a full-length, unfaded copy of
/// every new bullet tracer on screen for its first frames.
pub const SKIP: u32 = u32::MAX - 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TexFormat {
    Bc1,
    Bc2,
    Bc3,
    Bc5,
    R8,
    Bgra8,
}

impl TexFormat {
    /// Block size in texels and bytes per block.
    pub fn block(self) -> (u32, u32) {
        match self {
            TexFormat::Bc1 => (4, 8),
            TexFormat::Bc2 | TexFormat::Bc3 | TexFormat::Bc5 => (4, 16),
            TexFormat::R8 => (1, 1),
            TexFormat::Bgra8 => (1, 4),
        }
    }
}

pub struct TextureDesc<'a> {
    pub format: TexFormat,
    pub srgb: bool,
    pub width: u32,
    pub height: u32,
    /// Largest first, tightly packed.
    pub mips: &'a [&'a [u8]],
    /// Tangent-space normal map (never sampled as sRGB).
    pub normal: bool,
    /// B-235: the original fetches this texture with Xenos's gamma sign. The uploader keeps it raw (`srgb` is false)
    /// and the material or lightmap shader applies the piecewise-linear curve after filtering.
    pub gamma: bool,
}

#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Debug)]
pub enum Blend {
    Opaque,
    Masked,
    Translucent,
    Additive,
    Modulate,
}

impl Blend {
    /// UE3 EBlendMode name (`BLEND_Masked`, ...); unknown or empty is opaque.
    pub fn from_ue3(s: &str) -> Self {
        match s {
            "BLEND_Masked" => Blend::Masked,
            "BLEND_Translucent" => Blend::Translucent,
            "BLEND_Additive" => Blend::Additive,
            "BLEND_Modulate" => Blend::Modulate,
            _ => Blend::Opaque,
        }
    }
    pub fn blended(self) -> bool {
        matches!(self, Blend::Translucent | Blend::Additive | Blend::Modulate)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct MaterialDesc<'a> {
    pub texture: u32, // NONE = flat grey
    pub blend: Blend,
    pub two_sided: bool,
    /// N8's material graph, when the material resolved to one (backends that can't use it draw `texture`).
    pub def: Option<&'a assetdb::MatDef>,
    /// Texture ids for `def.textures`, in order (NONE = missing).
    pub textures: &'a [u32],
    /// Unpack range per `def.textures` entry (normal maps).
    pub unpack: &'a [Option<([f32; 3], [f32; 3])>],
}

#[derive(Clone, Copy, Debug)]
pub struct SectionDesc {
    pub first_index: u32,
    pub index_count: u32,
    pub material: u32, // default material when an instance doesn't override it
}

pub struct MeshDesc<'a> {
    pub positions: &'a [[f32; 3]],
    pub uvs: &'a [[f32; 2]],
    /// The rest may be empty (a backend then uses defaults).
    pub normals: &'a [[f32; 3]],
    pub tangents: &'a [[f32; 4]],
    pub uv1: &'a [[f32; 2]],
    /// Bytes in file order (A, R, G, B).
    pub colors: &'a [[u8; 4]],
    pub indices: &'a [u32],
    pub sections: &'a [SectionDesc],
    pub center: [f32; 3],
    pub radius: f32,
}

/// One primitive this frame. `#[repr(C)]` so the C ABI can pass an array of these straight through.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Instance {
    /// UE3 LocalToWorld, row-major, row vectors.
    pub transform: [f32; 16],
    pub mesh: u32,
    /// Light id from `add_light` (per-vertex baked light), or NONE for the ambient colour.
    pub light: u32,
    /// Range in the frame's material-override array; an entry of NONE (or past the end) keeps the section's own.
    pub mat_offset: u32,
    pub mat_count: u32,
    /// Light environment index in the frame's lighting (`n1_set_lighting`), or NONE.
    pub env: u32,
}

/// Interleaved vertex as both backends upload it.
#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
pub struct Vertex {
    pub pos: [f32; 3],
    pub uv: [f32; 2],
}

/// Per-instance shader data (`Inst` in the shader).
#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
pub struct GpuInstance {
    pub model: [f32; 16],
    pub light_base: i32,
    pub has_light: i32,
    pub _pad: [i32; 2],
}

/// Per-frame shader constants (`Globals` in the shader).
#[repr(C)]
#[derive(Clone, Copy, Pod, Zeroable)]
pub struct Globals {
    pub view_proj: [f32; 16],
    pub ambient: [f32; 4],
    /// x: 1 to encode linear -> sRGB in the shader (UNORM target), 0 when the target format does it.
    pub flags: [f32; 4],
}

#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Debug)]
pub struct PipeKey {
    pub blend: Blend,
    pub two_sided: bool,
    pub mirrored: bool,
}

#[repr(C)]
#[derive(Default, Clone, Copy, Debug)]
pub struct FrameStats {
    pub instances: u32,
    pub culled: u32,
    pub draws: u32,
    pub pipeline_changes: u32,
    pub bind_changes: u32,
    /// Instances skipped because their mesh id was unknown.
    pub missing_mesh: u32,
    pub triangles: u64,
    /// CPU time to build and record the frame, ms.
    pub record_ms: f32,
    /// Assets uploaded this frame (n1ffi).
    pub uploads: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct DrawItem {
    pub key: PipeKey,
    /// Texture binding (0 = the flat grey default).
    pub bind: u32,
    pub mesh: u32,
    pub first_index: u32,
    pub index_count: u32,
    pub instance: u32,
    pub depth: f32,
}

pub struct MeshInfo {
    pub vertex_count: u32,
    pub sections: Vec<SectionDesc>,
    pub center: [f32; 3],
    pub radius: f32,
}

pub struct MaterialInfo {
    pub bind: u32,
    pub blend: Blend,
    pub two_sided: bool,
}

pub fn pack_light(c: [f32; 3], scale: f32) -> u32 {
    let q = |v: f32| ((v * scale / 4.0).clamp(0.0, 1.0) * 255.0 + 0.5) as u32;
    q(c[0]) | q(c[1]) << 8 | q(c[2]) << 16 | 255 << 24
}

/// What the renderer knows about uploaded assets, and the per-frame draw list.
pub struct SceneCore {
    pub meshes: Vec<MeshInfo>,
    pub materials: Vec<MaterialInfo>,
    /// (offset, count) in `light_cpu`.
    pub lights: Vec<(u32, u32)>,
    pub light_cpu: Vec<u32>,
    pub ambient: f32,
    pub light_scale: f32,
    pub cull: bool,
    // per frame
    pub gpu_instances: Vec<GpuInstance>,
    pub draws: Vec<DrawItem>,
}

impl Default for SceneCore {
    fn default() -> Self {
        Self {
            meshes: Vec::new(),
            materials: Vec::new(),
            lights: Vec::new(),
            light_cpu: Vec::new(),
            ambient: 0.75,
            light_scale: 1.0,
            cull: true,
            gpu_instances: Vec::new(),
            draws: Vec::new(),
        }
    }
}

impl SceneCore {
    /// Records a mesh's metadata; returns its id. `n` = the index count, to drop out-of-range sections.
    pub fn add_mesh(&mut self, m: &MeshDesc) -> u32 {
        let n = m.indices.len() as u32;
        let sections = m
            .sections
            .iter()
            .filter(|s| s.index_count > 0 && s.first_index + s.index_count <= n)
            .copied()
            .collect();
        self.meshes.push(MeshInfo {
            vertex_count: m.positions.len() as u32,
            sections,
            center: m.center,
            radius: m.radius,
        });
        self.meshes.len() as u32 - 1
    }

    /// `bind` = the backend's texture binding (0 = flat grey).
    pub fn add_material(&mut self, m: &MaterialDesc, bind: u32) -> u32 {
        self.materials.push(MaterialInfo {
            bind,
            blend: m.blend,
            two_sided: m.two_sided,
        });
        self.materials.len() as u32 - 1
    }

    /// Per-vertex baked light (linear RGB, as the lightmap stores it after its scale); returns its id. The packed
    /// words are appended to `light_cpu`; a backend uploads `light_cpu[from..]` when it grew.
    pub fn add_light(&mut self, colors: &[[f32; 3]]) -> u32 {
        let off = self.light_cpu.len() as u32;
        let s = self.light_scale;
        self.light_cpu
            .extend(colors.iter().map(|c| pack_light(*c, s)));
        self.lights.push((off, colors.len() as u32));
        self.lights.len() as u32 - 1
    }

    /// Builds `gpu_instances` and the sorted `draws` for one frame.
    pub fn build(
        &mut self,
        view_proj: &[f32; 16],
        eye: [f32; 3],
        instances: &[Instance],
        material_overrides: &[u32],
    ) -> FrameStats {
        let mut st = FrameStats {
            instances: instances.len() as u32,
            ..Default::default()
        };
        let planes = frustum_planes(view_proj);
        self.gpu_instances.clear();
        self.draws.clear();
        for inst in instances {
            let Some(mesh) = self.meshes.get(inst.mesh as usize) else {
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
            if self.cull
                && planes
                    .iter()
                    .any(|p| p[0] * wc[0] + p[1] * wc[1] + p[2] * wc[2] + p[3] < -radius)
            {
                st.culled += 1;
                continue;
            }
            let det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8])
                + m[2] * (m[4] * m[9] - m[5] * m[8]);
            let (light_base, has_light) = match self.lights.get(inst.light as usize) {
                Some(&(off, n)) if n == mesh.vertex_count => (off as i32, 1),
                _ => (0, 0),
            };
            let ii = self.gpu_instances.len() as u32;
            self.gpu_instances.push(GpuInstance {
                model: *m,
                light_base,
                has_light,
                _pad: [0; 2],
            });
            let dist =
                ((wc[0] - eye[0]).powi(2) + (wc[1] - eye[1]).powi(2) + (wc[2] - eye[2]).powi(2))
                    .sqrt();
            for (si, s) in mesh.sections.iter().enumerate() {
                let mut mat = s.material;
                if (si as u32) < inst.mat_count
                    && let Some(&o) = material_overrides.get((inst.mat_offset + si as u32) as usize)
                    && o != NONE
                {
                    if o == SKIP {
                        continue;
                    }
                    mat = o;
                }
                let (bind, blend, two_sided) = match self.materials.get(mat as usize) {
                    Some(mt) => (mt.bind, mt.blend, mt.two_sided),
                    None => (0, Blend::Opaque, false),
                };
                self.draws.push(DrawItem {
                    key: PipeKey {
                        blend,
                        two_sided,
                        mirrored: det < 0.0,
                    },
                    bind,
                    mesh: inst.mesh,
                    first_index: s.first_index,
                    index_count: s.index_count,
                    instance: ii,
                    depth: dist,
                });
            }
        }
        // Opaque/masked by state; blended back to front.
        self.draws.sort_by(|a, b| {
            let (ab, bb) = (a.key.blend.blended(), b.key.blend.blended());
            ab.cmp(&bb).then_with(|| {
                if ab {
                    b.depth
                        .partial_cmp(&a.depth)
                        .unwrap_or(std::cmp::Ordering::Equal)
                } else {
                    (a.key, a.bind, a.mesh).cmp(&(b.key, b.bind, b.mesh))
                }
            })
        });
        st
    }

    /// The pipeline keys this frame's draws need.
    pub fn keys(&self) -> Vec<PipeKey> {
        let mut k: Vec<PipeKey> = self.draws.iter().map(|d| d.key).collect();
        k.sort();
        k.dedup();
        k
    }
}

/// What the resolver needs from a renderer.
pub trait Backend {
    /// Returns the texture's binding id (>= 1; 0 is the flat grey default).
    fn add_texture(&mut self, t: &TextureDesc) -> u32;
    fn add_material(&mut self, m: &MaterialDesc) -> u32;
    /// The resolver no longer needs material `id` (a run-time variant nothing drew for a while): the backend may reuse
    /// its resources for a later `add_material` once the GPU is done with it. Default: keep it.
    fn remove_material(&mut self, id: u32) {
        let _ = id;
    }
    fn add_mesh(&mut self, m: &MeshDesc) -> u32;
    fn add_light(&mut self, colors: &[[f32; 3]]) -> u32;
    /// A component's full baked light; `lightmaps` are texture ids for `ComponentLight::Map` (NONE = missing). The
    /// default keeps the old per-vertex colour path: directional light averaged to one colour, lightmaps dropped.
    fn add_component_light(&mut self, l: &assetdb::ComponentLight, lightmaps: [u32; 3]) -> u32 {
        let _ = lightmaps;
        match l {
            assetdb::ComponentLight::Legacy(c) => self.add_light(c),
            assetdb::ComponentLight::Dir { words, scale } => {
                let c: Vec<[f32; 3]> = words
                    .chunks_exact(3)
                    .map(|w| {
                        let mut o = [0f32; 3];
                        for (k, x) in w.iter().enumerate() {
                            let b = x.to_le_bytes();
                            for ch in 0..3 {
                                o[ch] += b[1 + ch] as f32 / 255.0 * scale[k][ch] / 3.0;
                            }
                        }
                        o
                    })
                    .collect();
                self.add_light(&c)
            }
            assetdb::ComponentLight::Map { .. } => NONE,
        }
    }
}

/// The shader both backends use (WGSL; n1ffi compiles it to SPIR-V with naga at build time).
pub const SHADER_WGSL: &str = include_str!("scene.wgsl");

/// UE3 row-vector reverse-Z infinite projection from the x/y scales of a UE3 projection matrix (`proj[0]`,
/// `proj[5]`) and the near plane: clip = (x*sx, y*sy, near, z), so depth = near / z (1 at the near plane, 0 at
/// infinity).
/// Frustum planes (normalised, inside: p . (x, y, z, 1) >= 0) from a row-vector view-projection: clip_j = v .
/// column j. Keeps -w<=x<=w, -w<=y<=w and z<=w (the near plane in reverse Z); the infinite far plane needs none.
pub fn frustum_planes(view_proj: &[f32; 16]) -> Vec<[f32; 4]> {
    let col = |j: usize| {
        [
            view_proj[j],
            view_proj[4 + j],
            view_proj[8 + j],
            view_proj[12 + j],
        ]
    };
    let (c0, c1, c2, c3) = (col(0), col(1), col(2), col(3));
    let add = |a: [f32; 4], b: [f32; 4], s: f32| {
        [
            a[0] + s * b[0],
            a[1] + s * b[1],
            a[2] + s * b[2],
            a[3] + s * b[3],
        ]
    };
    [
        add(c3, c0, 1.0),
        add(c3, c0, -1.0),
        add(c3, c1, 1.0),
        add(c3, c1, -1.0),
        add(c3, c2, -1.0),
    ]
    .into_iter()
    .map(|p| {
        let l = (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]).sqrt().max(1e-20);
        [p[0] / l, p[1] / l, p[2] / l, p[3] / l]
    })
    .collect()
}

pub fn reverse_z_projection(sx: f32, sy: f32, near: f32) -> [f32; 16] {
    [
        sx, 0.0, 0.0, 0.0, 0.0, sy, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, near, 0.0,
    ]
}

/// Row-major 4x4 product a * b (UE3 order: v * a * b).
pub fn mul(a: &[f32; 16], b: &[f32; 16]) -> [f32; 16] {
    let mut r = [0f32; 16];
    for i in 0..4 {
        for j in 0..4 {
            r[i * 4 + j] = (0..4).map(|k| a[i * 4 + k] * b[k * 4 + j]).sum();
        }
    }
    r
}

/// The reverse-Z view-projection for a UE3 view matrix and UE3 projection (only its x/y scales are used; the
/// near plane is given: 5 in this game's FSceneView).
pub fn view_proj(view: &[f32; 16], proj: &[f32; 16], near: f32) -> [f32; 16] {
    mul(view, &reverse_z_projection(proj[0], proj[5], near))
}

/// B-235: decode the scene's gamma-fetched textures (material colour textures, lightmaps) with Xenos's piecewise-linear
/// curve, as the port's shader translator does, instead of sampling them as sRGB. `BOTS_N1_SCENE_PWL=0` restores sRGB.
pub fn scene_pwl() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("BOTS_N1_SCENE_PWL").map(|v| v != "0").unwrap_or(true))
}

/// B-189: lightmaps are linearised (sRGB, or PWL with `scene_pwl`) before their scale vectors apply.
/// `BOTS_N1_LM_SRGB=0` restores the old raw sampling.
pub fn lm_linear() -> bool {
    static V: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *V.get_or_init(|| std::env::var("BOTS_N1_LM_SRGB").map(|v| v != "0").unwrap_or(true))
}

/// The lightmaps take the PWL curve in the scene shader (`ShaderFlags::lm_pwl`).
pub fn lm_pwl() -> bool {
    scene_pwl() && lm_linear()
}

/// B-235: does a material texture with these header facts take the PWL curve? The colour textures UE3 marks SRGB:
/// its Xenon RHI makes their fetch formats gamma on RGB (the port logs signs 3,3,3,0 for them). Normal maps and BC5
/// are never gamma.
pub fn tex_gamma(srgb: bool, normal: bool, fmt: assetdb::TexFmt) -> bool {
    scene_pwl() && srgb && !normal && fmt != assetdb::TexFmt::Bc5
}
