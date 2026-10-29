//! B-186: N9's particles in the live renderer. CPU side: handle tables, a loader thread for particle materials
//! (expression graph -> WGSL -> SPIR-V) and meshes, the per-frame conversion of the feed's particle rows into
//! sprite and mesh instances, ordering and the sprite cap. The Vulkan side is `vkr/pgpu.rs`.
//!
//! Input is binary: `N1Emitter` headers plus a flat float array of per-particle rows (see `include/n1ffi.h`). The
//! same instance builder (`scenefmt::particles::push_particle`) serves the offline JSON path.

use crate::shaders;
use crate::vkr::Renderer;
use anyhow::{Result, anyhow};
use assetdb::{AssetDb, MeshAsset, TexFmt, TextureAsset};
use scenefmt::particles::{EmitterDraw, Kind, lock_sprite, matprog, push_particle, shader};
use scenekit::Blend;
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, mpsc};

pub const PNONE: u32 = u32::MAX;

/// One live emitter instance as the host passes it. 128 bytes; rows for it are `count * row_floats` floats from
/// `row_offset` in the frame's row array. A row is [location 3, old_location 3, size 3, rotation, colour 4,
/// relative_time, payload...] in UE3 units.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct N1Emitter {
    /// 0 sprite, 1 mesh.
    pub kind: u32,
    /// `n1_pmaterial` handle.
    pub material: u32,
    /// `n1_pmesh` handle (mesh emitters) or N1_NONE.
    pub mesh: u32,
    /// EParticleScreenAlignment: 0 square, 1 rectangle, 2 velocity.
    pub align: u32,
    pub sub_h: u32,
    pub sub_v: u32,
    /// InterpolationMethod (0: no sub-UV animation).
    pub sub_method: u32,
    pub local_space: u32,
    pub count: u32,
    pub row_offset: u32,
    pub row_floats: u32,
    /// EParticleAxisLock of the emitter's OrientationAxisLock module (0: none, camera-facing).
    pub lock_axis: u32,
    /// Emitter location (UE3), for ordering.
    pub origin: [f32; 3],
    /// Padding; 0 normally. B-236 debug: a small integer selects a sprite orientation variant (see `prepare`).
    pub pad1: f32,
    /// Component LocalToWorld (UE3), applied to local-space rows.
    pub transform: [f32; 16],
    /// B-247: the component's Bounds.Origin (UE3) in xyz, w = 1 when set (0: an older host; `origin` orders then).
    /// UE3 sorts translucent primitives back to front by the view-space depth of their bounds origin.
    pub sort_origin: [f32; 4],
}

/// A material compiled on the loader thread.
pub struct LoadedMat {
    pub name: String,
    pub blend: Blend,
    pub two_sided: bool,
    pub textures: Vec<Option<Arc<TextureAsset>>>,
    pub sprite_spv: Vec<u32>,
    pub mesh_spv: Vec<u32>,
    pub unknown: Vec<String>,
}

enum Job {
    Mat(u32, String),
    Mesh(u32, String),
}

enum Done {
    Mat(u32, Result<LoadedMat, String>),
    Mesh(u32, Option<Arc<MeshAsset>>),
}

enum St {
    Idle,
    Loading,
    Ready(u32),
    Failed,
}

#[derive(Clone, Copy, Debug)]
pub struct PDraw {
    /// Renderer material id.
    pub mat: u32,
    /// 0 sprite, 1 velocity-aligned sprite, 2 mesh.
    pub variant: u8,
    /// Renderer particle mesh id (mesh draws).
    pub mesh: u32,
    /// First instance in the frame's sprite or mesh array, and how many.
    pub first: u32,
    pub count: u32,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct PStats {
    pub emitters: u32,
    pub sprites: u32,
    pub mesh_instances: u32,
    pub dropped_sprites: u32,
    pub mats_ready: u32,
    pub mats_loading: u32,
    pub mats_failed: u32,
    pub meshes_ready: u32,
    pub draws: u32,
    pub skipped_emitters: u32,
    /// Mesh particles over `BOTS_N1_PARTICLE_MESH_CAP`.
    pub dropped_meshes: u32,
    /// Sprites in emitters dropped by the fill budget (`BOTS_N1_PARTICLE_FILL`).
    pub dropped_fill: u32,
    /// Rows with non-finite or absurd values (position, size, transform), never drawn.
    pub bad_rows: u32,
    /// Estimated sprite coverage drawn this frame, in screens (sum of each sprite's on-screen fraction).
    pub fill_screens: f32,
}

/// Everything the renderer draws for particles this frame.
#[derive(Default)]
pub struct PFrame {
    /// 20 floats per instance (`scenefmt::particles::SpriteInst` / `MeshInst`), Y-up, drawn in this order.
    pub sprites: Vec<f32>,
    pub meshes: Vec<f32>,
    pub draws: Vec<PDraw>,
    /// Camera axes and position in the renderer's Y-up space.
    pub right: [f32; 3],
    pub up: [f32; 3],
    pub fwd: [f32; 3],
    pub pos: [f32; 3],
    pub near: f32,
    pub stats: PStats,
}

impl PFrame {
    pub fn has_sprites(&self) -> bool {
        self.draws.iter().any(|d| d.variant < 2)
    }
    pub fn has_meshes(&self) -> bool {
        self.draws.iter().any(|d| d.variant == 2)
    }
}

pub struct ParticleSystem {
    db: Arc<AssetDb>,
    tx: mpsc::Sender<Job>,
    rx: mpsc::Receiver<Done>,
    queue: VecDeque<Done>,
    pub enabled: bool,
    /// Most sprites drawn in a frame (`BOTS_N1_PARTICLE_CAP`); the farthest emitters are dropped first.
    pub cap: usize,
    /// Most mesh particles drawn in a frame (`BOTS_N1_PARTICLE_MESH_CAP`), farthest emitters dropped first.
    pub mesh_cap: usize,
    /// Fill budget in screens (`BOTS_N1_PARTICLE_FILL`, 0 = off): the estimated on-screen area of all sprites in a
    /// frame, each sprite counting at most one screen. Emitters that would pass it are dropped, nearest kept first.
    /// A guard against a frame that keeps a low-end GPU busy for seconds (a TDR on Windows), not a quality knob.
    pub fill_cap: f32,
    mat_paths: Vec<String>,
    mat_by_path: HashMap<String, u32>,
    mat_state: Vec<St>,
    mesh_paths: Vec<String>,
    mesh_by_path: HashMap<String, u32>,
    mesh_state: Vec<St>,
    raw: Vec<N1Emitter>,
    rows: Vec<f32>,
    scratch: EmitterDraw,
    pub last: PStats,
    pub log: bool,
    /// B-199: material and mesh loads a draw had to ask for; preloads queued; preloaded ones not in yet.
    pub lazy: u64,
    pub preloads: u64,
}

fn env_num<T: std::str::FromStr>(name: &str, default: T) -> T {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

fn tex_format(f: TexFmt) -> scenekit::TexFormat {
    match f {
        TexFmt::Bc1 => scenekit::TexFormat::Bc1,
        TexFmt::Bc2 => scenekit::TexFormat::Bc2,
        TexFmt::Bc3 => scenekit::TexFormat::Bc3,
        TexFmt::Bc5 => scenekit::TexFormat::Bc5,
        TexFmt::R8 => scenekit::TexFormat::R8,
        TexFmt::Bgra8 => scenekit::TexFormat::Bgra8,
    }
}

/// Texture description for the renderer's uploader. `raw`: upload an sRGB colour texture without the sRGB view, for a
/// particle shader that applies Xenos's curve to the stored values (`pwl_raw`).
pub fn tex_desc<'a>(t: &'a TextureAsset, mips: &'a [&'a [u8]], raw: bool) -> scenekit::TextureDesc<'a> {
    scenekit::TextureDesc {
        format: tex_format(t.fmt),
        srgb: t.srgb && !t.normal && !raw,
        width: t.width,
        height: t.height,
        mips,
        normal: t.normal,
        // Particles apply Xenos's curve in their own shader (`xe_pwl_raw` on a raw upload, G6; `xe_pwl` on the sRGB view,
        // B-229); trees sample sRGB.
        gamma: false,
    }
}

/// A particle material's two shaders (sprite and mesh variants) and the state their pipelines depend on.
pub struct PShaders {
    pub sprite_spv: Vec<u32>,
    pub mesh_spv: Vec<u32>,
    pub blend: Blend,
    pub two_sided: bool,
}

fn particle_pwl() -> bool {
    static PWL: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *PWL.get_or_init(|| std::env::var("BOTS_N1_PARTICLE_PWL").map(|v| v != "0").unwrap_or(true))
}

/// G6: particle colour textures that take Xenos's curve are uploaded raw and decoded from the stored value, as the port
/// does, instead of through the sRGB view plus a `pow` re-encode (`BOTS_N1_PARTICLE_PWL_RAW=0`: the B-229 way).
pub fn pwl_raw() -> bool {
    static RAW: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *RAW.get_or_init(|| particle_pwl() && std::env::var("BOTS_N1_PARTICLE_PWL_RAW").map(|v| v != "0").unwrap_or(true))
}

/// Whether a particle texture is uploaded raw (`tex_desc`'s `raw`): exactly the textures `tex_info` gives the curve.
pub fn tex_raw(t: &TextureAsset) -> bool {
    pwl_raw() && t.srgb && !t.normal
}

/// A particle texture's unpack range, and whether its colour takes Xenos's gamma curve (sRGB colour textures;
/// `BOTS_N1_PARTICLE_PWL=0` keeps plain sRGB).
pub fn tex_info(db: &AssetDb, t: &str) -> matprog::TexInfo {
    let pwl = particle_pwl();
    match db.texture_meta(t) {
        Some(m) => matprog::TexInfo { unpack: m.unpack, pwl: pwl && m.srgb && !m.normal, raw: pwl_raw() },
        None => matprog::TexInfo::default(),
    }
}

fn compile_material(db: &AssetDb, path: &str) -> Result<(matprog::Compiled, PShaders)> {
    let j = db.particle_material_json(path).ok_or_else(|| anyhow!("no material graph"))?;
    let mut c = matprog::compile_with(&j, &|t| tex_info(db, t));
    let sprite_spv = shaders::spirv(&shader::module_source(&c, false))?;
    // Mesh particles are drawn inside the opaque pass, where the depth buffer can't be sampled.
    c.soft = false;
    let mesh_spv = shaders::spirv(&shader::module_source(&c, true))?;
    let blend = match c.blend {
        matprog::Blend::Opaque => Blend::Opaque,
        matprog::Blend::Masked => Blend::Masked,
        matprog::Blend::Translucent => Blend::Translucent,
        matprog::Blend::Additive => Blend::Additive,
        matprog::Blend::Modulate => Blend::Modulate,
    };
    let two_sided = c.two_sided;
    Ok((c, PShaders { sprite_spv, mesh_spv, blend, two_sided }))
}

/// Shaders only (no textures): what the startup step needs to build a particle material's pipelines.
pub fn material_shaders(db: &AssetDb, path: &str) -> Result<PShaders> {
    compile_material(db, path).map(|x| x.1)
}

fn load_material(db: &AssetDb, path: &str) -> Result<LoadedMat> {
    let (c, sh) = compile_material(db, path)?;
    let textures = c.textures.iter().map(|t| db.texture(t)).collect();
    Ok(LoadedMat {
        name: path.to_string(),
        blend: sh.blend,
        two_sided: sh.two_sided,
        textures,
        sprite_spv: sh.sprite_spv,
        mesh_spv: sh.mesh_spv,
        unknown: c.unknown,
    })
}

impl ParticleSystem {
    pub fn new(db: Arc<AssetDb>, enabled: bool) -> ParticleSystem {
        let (tx, jobs) = mpsc::channel::<Job>();
        let (done_tx, rx) = mpsc::channel::<Done>();
        let d = db.clone();
        std::thread::Builder::new()
            .name("n1-particles".into())
            .spawn(move || {
                while let Ok(job) = jobs.recv() {
                    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| match job {
                        Job::Mat(h, p) => Done::Mat(h, load_material(&d, &p).map_err(|e| format!("{p}: {e:#}"))),
                        Job::Mesh(h, p) => Done::Mesh(h, d.mesh(&p)),
                    }));
                    match r {
                        Ok(done) => {
                            if done_tx.send(done).is_err() {
                                break;
                            }
                        }
                        Err(_) => eprintln!("n1ffi: particle loader panicked"),
                    }
                }
            })
            .expect("particle loader thread");
        let cap = env_num("BOTS_N1_PARTICLE_CAP", 2048usize);
        let mesh_cap = env_num("BOTS_N1_PARTICLE_MESH_CAP", 2048usize);
        let fill_cap = env_num("BOTS_N1_PARTICLE_FILL", 256.0f32).max(0.0);
        ParticleSystem {
            db,
            tx,
            rx,
            queue: VecDeque::new(),
            enabled,
            cap,
            mesh_cap,
            fill_cap,
            mat_paths: Vec::new(),
            mat_by_path: HashMap::new(),
            mat_state: Vec::new(),
            mesh_paths: Vec::new(),
            mesh_by_path: HashMap::new(),
            mesh_state: Vec::new(),
            raw: Vec::new(),
            rows: Vec::new(),
            scratch: EmitterDraw {
                kind: Kind::Sprite,
                prim: 0,
                emitter: String::new(),
                material: String::new(),
                mesh: String::new(),
                align: scenefmt::particles::Align::Square,
                sub_images: [1, 1],
                origin: [0.0; 3],
                sprites: Vec::new(),
                meshes: Vec::new(),
            },
            last: PStats::default(),
            log: std::env::var("BOTS_N1_PARTICLE_LOG").is_ok(),
            lazy: 0,
            preloads: 0,
        }
    }

    pub fn material_handle(&mut self, path: &str) -> u32 {
        if let Some(&h) = self.mat_by_path.get(path) {
            return h;
        }
        let h = self.mat_paths.len() as u32;
        self.mat_paths.push(path.to_string());
        self.mat_state.push(St::Idle);
        self.mat_by_path.insert(path.to_string(), h);
        h
    }

    pub fn mesh_handle(&mut self, path: &str) -> u32 {
        if let Some(&h) = self.mesh_by_path.get(path) {
            return h;
        }
        let h = self.mesh_paths.len() as u32;
        self.mesh_paths.push(path.to_string());
        self.mesh_state.push(St::Idle);
        self.mesh_by_path.insert(path.to_string(), h);
        h
    }

    /// B-199: build a particle material (and its textures) before any emitter uses it. True when it queued.
    pub fn preload_material(&mut self, path: &str) -> bool {
        let h = self.material_handle(path) as usize;
        if !matches!(self.mat_state[h], St::Idle) {
            return false;
        }
        self.mat_state[h] = St::Loading;
        self.preloads += 1;
        let _ = self.tx.send(Job::Mat(h as u32, path.to_string()));
        true
    }

    /// B-199: load a mesh-particle mesh before any emitter uses it.
    pub fn preload_mesh(&mut self, path: &str) -> bool {
        let h = self.mesh_handle(path) as usize;
        if !matches!(self.mesh_state[h], St::Idle) {
            return false;
        }
        self.mesh_state[h] = St::Loading;
        self.preloads += 1;
        let _ = self.tx.send(Job::Mesh(h as u32, path.to_string()));
        true
    }

    /// Materials and meshes still loading (preloaded or not) and finished loads not taken in yet.
    pub fn pending(&self) -> u32 {
        (self.mat_state.iter().chain(&self.mesh_state).filter(|s| matches!(s, St::Loading)).count() + self.queue.len())
            as u32
    }

    /// The next `prepare`'s emitters (copied; used once).
    pub fn set_frame(&mut self, emitters: &[N1Emitter], rows: &[f32]) {
        self.raw.clear();
        self.raw.extend_from_slice(emitters);
        self.rows.clear();
        self.rows.extend_from_slice(rows);
    }

    pub fn db(&self) -> &Arc<AssetDb> {
        &self.db
    }

    /// Takes finished loads into the renderer (at most `budget` per call), then builds this frame's instances.
    /// Applies up to `budget` finished loads (stops early at `deadline`); returns how many.
    pub fn pump_until(&mut self, r: &mut Renderer, budget: usize, deadline: Option<std::time::Instant>) -> usize {
        while let Ok(d) = self.rx.try_recv() {
            self.queue.push_back(d);
        }
        let mut n = 0;
        for _ in 0..budget.max(1) {
            if deadline.is_some_and(|d| std::time::Instant::now() >= d) {
                break;
            }
            let Some(d) = self.queue.pop_front() else { break };
            n += 1;
            match d {
                Done::Mat(h, Ok(m)) => {
                    if !m.unknown.is_empty() && (self.log || std::env::var("BOTS_N1_MATLOG").map_or(true, |v| v != "0")) {
                        eprintln!("n1ffi: particle material {} has unknown expressions {:?} (drawn as constant 1)", m.name, m.unknown);
                    }
                    match r.pmat_add(&m) {
                        Ok(id) => self.mat_state[h as usize] = St::Ready(id),
                        Err(e) => {
                            eprintln!("n1ffi: particle material {}: {e:#}", m.name);
                            self.mat_state[h as usize] = St::Failed;
                        }
                    }
                }
                Done::Mat(h, Err(e)) => {
                    if self.log {
                        eprintln!("n1ffi: particle material failed: {e}");
                    }
                    self.mat_state[h as usize] = St::Failed;
                }
                Done::Mesh(h, Some(m)) => match r.pmesh_add(&m) {
                    Ok(id) => self.mesh_state[h as usize] = St::Ready(id),
                    Err(e) => {
                        eprintln!("n1ffi: particle mesh: {e:#}");
                        self.mesh_state[h as usize] = St::Failed;
                    }
                },
                Done::Mesh(h, None) => self.mesh_state[h as usize] = St::Failed,
            }
        }
        n
    }

    pub fn prepare(&mut self, r: &mut Renderer, view_proj: &[f32; 16], eye: [f32; 3], budget: usize) -> PFrame {
        self.pump_until(r, budget, None);
        let mut pf = PFrame::default();
        let col = |c: usize| {
            let v = [view_proj[c], view_proj[4 + c], view_proj[8 + c]];
            let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt().max(1e-9);
            [v[0] / l, v[2] / l, v[1] / l]
        };
        pf.right = col(0);
        pf.up = col(1);
        pf.fwd = col(3);
        pf.pos = [eye[0], eye[2], eye[1]];
        let mut st = PStats::default();
        for s in &self.mat_state {
            match s {
                St::Ready(_) => st.mats_ready += 1,
                St::Loading => st.mats_loading += 1,
                St::Failed => st.mats_failed += 1,
                St::Idle => {}
            }
        }
        st.meshes_ready = self.mesh_state.iter().filter(|s| matches!(s, St::Ready(_))).count() as u32;
        if !self.enabled || self.raw.is_empty() {
            self.raw.clear();
            self.rows.clear();
            self.last = st;
            return pf;
        }
        // Debug: BOTS_N1_PARTICLE_SKIP=a,b skips emitters whose material path contains one of the words;
        // BOTS_N1_PARTICLE_ONLY=a keeps only those.
        let skip: Vec<String> = std::env::var("BOTS_N1_PARTICLE_SKIP").map(|v| v.split(',').map(str::to_string).collect()).unwrap_or_default();
        let only = std::env::var("BOTS_N1_PARTICLE_ONLY").ok();
        // Which emitters can be drawn now; the rest start their loads.
        let mut order: Vec<(f32, usize, u32, u32)> = Vec::new();
        for (i, e) in self.raw.iter().enumerate() {
            if e.count == 0 || e.row_floats < 15 {
                continue;
            }
            let mp = self.mat_paths.get(e.material as usize).map(|x| x.as_str()).unwrap_or("");
            if skip.iter().any(|w| !w.is_empty() && mp.contains(w.as_str())) || only.as_ref().is_some_and(|w| !mp.contains(w.as_str())) {
                continue;
            }
            let mat = match self.mat_state.get(e.material as usize) {
                Some(St::Ready(id)) => *id,
                Some(St::Idle) => {
                    self.lazy += 1;
                    let h = e.material as usize;
                    scenekit::lazy_log("particle material", &self.mat_paths[h]);
                    let _ = self.tx.send(Job::Mat(h as u32, self.mat_paths[h].clone()));
                    self.mat_state[h] = St::Loading;
                    st.skipped_emitters += 1;
                    continue;
                }
                _ => {
                    st.skipped_emitters += 1;
                    continue;
                }
            };
            let mut mesh = PNONE;
            if e.kind == 1 {
                match self.mesh_state.get(e.mesh as usize) {
                    Some(St::Ready(id)) => mesh = *id,
                    Some(St::Idle) => {
                        self.lazy += 1;
                        let h = e.mesh as usize;
                        scenekit::lazy_log("particle mesh", &self.mesh_paths[h]);
                        let _ = self.tx.send(Job::Mesh(h as u32, self.mesh_paths[h].clone()));
                        self.mesh_state[h] = St::Loading;
                        st.skipped_emitters += 1;
                        continue;
                    }
                    _ => {
                        st.skipped_emitters += 1;
                        continue;
                    }
                }
            }
            let o = [e.origin[0], e.origin[2], e.origin[1]];
            let d2 = (o[0] - pf.pos[0]).powi(2) + (o[1] - pf.pos[1]).powi(2) + (o[2] - pf.pos[2]).powi(2);
            order.push((d2, i, mat, mesh));
        }
        // Keep the nearest emitters within the sprite cap; draw far to near.
        order.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));
        let mut kept = Vec::with_capacity(order.len());
        let mut sprites = 0usize;
        let mut meshes = 0usize;
        let mut fill = 0.0f32;
        for o in order {
            let e = &self.raw[o.1];
            if e.kind == 0 {
                if sprites + e.count as usize > self.cap {
                    st.dropped_sprites += e.count;
                    continue;
                }
                if self.fill_cap > 0.0 {
                    let f = sprite_fill(e, &self.rows, view_proj);
                    if fill + f > self.fill_cap {
                        st.dropped_sprites += e.count;
                        st.dropped_fill += e.count;
                        continue;
                    }
                    fill += f;
                }
                sprites += e.count as usize;
            } else {
                if meshes + e.count as usize > self.mesh_cap {
                    st.dropped_meshes += e.count;
                    continue;
                }
                meshes += e.count as usize;
            }
            kept.push(o);
        }
        st.fill_screens = fill;
        // Far to near between components, but in Emitters array order within one (UE3's
        // FParticleSystemSceneProxy draws its emitters in array order; they share the component's origin, so they tie
        // here). A plain reverse would draw a system's emitters backwards: Explosion_Quick2_PS's dark Smoke emitters
        // (0, 1) landed on top of its fireball (4). The sort is stable, so ties keep the feed's (array) order.
        //
        // B-247: the key between components is UE3's (FTranslucentPrimSet): the view-space depth of the component's
        // Bounds.Origin, not the distance to the emitter's location. The two disagree for a rocket hit: the explosion
        // and the flesh-impact systems sit at almost the same location, but the explosion's bounds (spread by its
        // sparks) put it behind the impact, so UE3 draws the blood spray over the fireball; by location n1 drew the
        // fireball last and hid the blood. The rocket's trail, whose bounds reach back along its path, sorts in front.
        // BOTS_N1_PARTICLE_SORT=0 or debug bit 1024 ("hybrid n1debug 1024") keeps the old key.
        static UE3_SORT: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        let ue3_sort = *UE3_SORT.get_or_init(|| std::env::var("BOTS_N1_PARTICLE_SORT").map(|v| v != "0").unwrap_or(true))
            && r.debug_bits & 1024 == 0;
        let mut kept: Vec<(f32, usize, u32, u32)> = kept
            .into_iter()
            .map(|(d2, i, mat, mesh)| {
                let s = self.raw[i].sort_origin;
                if ue3_sort && s[3] != 0.0 {
                    let o = [s[0] - pf.pos[0], s[2] - pf.pos[1], s[1] - pf.pos[2]];
                    (o[0] * pf.fwd[0] + o[1] * pf.fwd[1] + o[2] * pf.fwd[2], i, mat, mesh)
                } else {
                    (d2, i, mat, mesh)
                }
            })
            .collect();
        kept.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap_or(std::cmp::Ordering::Equal));
        // Debug: BOTS_N1_PARTICLE_CENSUS=<frames> prints every drawn emitter (material, count, colour, size) once
        // per that many frames.
        static CENSUS: std::sync::OnceLock<u64> = std::sync::OnceLock::new();
        static FRAME: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let every = *CENSUS.get_or_init(|| std::env::var("BOTS_N1_PARTICLE_CENSUS").ok().and_then(|v| v.parse().ok()).unwrap_or(0));
        let census = every > 0 && FRAME.fetch_add(1, std::sync::atomic::Ordering::Relaxed) % every == 0;
        if census {
            let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0);
            eprintln!("n1ffi: census frame, {} emitters drawn, unix {t:.1}", kept.len());
        }
        for (_, i, mat, mesh) in kept {
            let e = self.raw[i];
            let d = &mut self.scratch;
            d.kind = if e.kind == 1 { Kind::Mesh } else { Kind::Sprite };
            d.sub_images = [e.sub_h.max(1), e.sub_v.max(1)];
            d.align = scenefmt::particles::parse_align("", e.align as i64);
            d.sprites.clear();
            d.meshes.clear();
            let rf = e.row_floats as usize;
            let base = e.row_offset as usize;
            let l2w = (e.local_space != 0).then_some(&e.transform);
            // UE3 scales a sprite by its component's scale (Scale x Scale3D x the owner's DrawScale), which
            // LocalToWorld carries: FDynamicSpriteEmitterData sizes each quad Particle.Size * Source.Scale. Older feeds
            // send a zero transform for world-space emitters: scale 1.
            let sscale = sprite_scale(&e.transform);
            for k in 0..e.count as usize {
                let at = base + k * rf;
                let Some(row) = self.rows.get(at..at + rf) else { break };
                push_particle(d, row, e.local_space != 0, l2w, e.sub_method as i64);
                if e.kind == 0 && sscale != [1.0, 1.0] {
                    if let Some(s) = d.sprites.last_mut() {
                        // PSA_Square takes Y from the scaled X, as UE3 does after scaling.
                        s[8] *= sscale[0];
                        s[9] = if e.align != 1 && e.align != 2 { s[8] } else { s[9] * sscale[1] };
                    }
                }
                // B-236 debug A/B (skullburglar "hybrid quad ... v<k>" or "hybrid n1variant <k>", passed in `pad1`):
                // bits 0-1 add k quarter turns, bit 2 mirrors the quad (negates its width), bit 3 puts back the old
                // (mirrored) roll sense on camera-facing sprites.
                if e.kind == 0 && e.pad1 >= 0.5 && e.pad1 < 64.0 {
                    let v = e.pad1 as u32;
                    if let Some(s) = d.sprites.last_mut() {
                        if v & 8 != 0 && e.lock_axis == 0 {
                            s[3] = -s[3];
                        }
                        s[3] += (v & 3) as f32 * std::f32::consts::FRAC_PI_2;
                        if v & 4 != 0 {
                            s[8] = -s[8];
                        }
                    }
                }
                if e.kind == 0 && e.lock_axis != 0 {
                    if let Some(s) = d.sprites.last_mut() {
                        lock_sprite(s, e.lock_axis, if e.local_space != 0 { l2w } else { None });
                    }
                }
                // Rows are the game's memory: never hand the GPU a non-finite or absurd instance.
                let arr = if e.kind == 1 { &mut d.meshes } else { &mut d.sprites };
                if arr.last().is_some_and(|x| !sane_instance(x, e.kind == 1)) {
                    arr.pop();
                    st.bad_rows += 1;
                }
            }
            if self.log && (e.kind == 1 || e.align == 2) {
                let first = if e.kind == 1 { d.meshes.first().map(|m| m.to_vec()) } else { d.sprites.first().map(|m| m.to_vec()) };
                eprintln!(
                    "n1ffi: emitter kind {} align {} n {} mat {} local {} origin {:?} first {:?}",
                    e.kind, e.align, e.count, self.mat_paths.get(e.material as usize).map(|s| s.as_str()).unwrap_or("?"), e.local_space, e.origin, first
                );
            }
            if census {
                let v: Vec<&[f32; 20]> = if e.kind == 1 { d.meshes.iter().collect() } else { d.sprites.iter().collect() };
                if !v.is_empty() {
                    let n = v.len() as f32;
                    let mut mean = [0.0f32; 4];
                    let mut mx = [0.0f32; 4];
                    let mut sz = 0.0f32;
                    // Nearest sprite centre's view depth (negative: behind the camera).
                    let mut wmin = f32::MAX;
                    for x in &v {
                        if e.kind == 0 {
                            wmin = wmin.min((0..3).map(|c| (x[c] - pf.pos[c]) * pf.fwd[c]).sum());
                        }
                        for c in 0..4 {
                            mean[c] += x[16 + c] / n;
                            mx[c] = mx[c].max(x[16 + c]);
                        }
                        sz += if e.kind == 1 { 0.0 } else { x[8].max(x[9]) / n };
                    }
                    eprintln!(
                        "n1ffi: census kind {} lock {} n {} mat {} col mean {:.2?} max {:.2?} size {:.0} wmin {:.0} scale {:.2?}",
                        e.kind, e.lock_axis, v.len(), self.mat_paths.get(e.material as usize).map(|s| s.as_str()).unwrap_or("?"), mean, mx, sz, wmin, sscale
                    );
                    // Sub-UV emitters: the grid, method, the first particle's payload and the cells n1 picked.
                    if e.kind == 0 && rf > 15 {
                        let row = self.rows.get(base..base + rf).unwrap_or(&[]);
                        let s0 = v[0];
                        eprintln!(
                            "n1ffi: census   subuv {}x{} method {} rt {:.3} payload {:?} uv1 {:.3?} uv2 {:.3?} interp {:.3}",
                            e.sub_h, e.sub_v, e.sub_method, row.get(14).copied().unwrap_or(-1.0), row.get(15..).unwrap_or(&[]), &s0[10..12], &s0[12..14], s0[7]
                        );
                    }
                }
            }
            let (arr, variant) = if e.kind == 1 {
                (&mut pf.meshes, 2u8)
            } else {
                (&mut pf.sprites, (e.align == 2) as u8)
            };
            let first = (arr.len() / 20) as u32;
            let n = if e.kind == 1 { d.meshes.len() } else { d.sprites.len() };
            if n == 0 {
                continue;
            }
            if e.kind == 1 {
                for m in &d.meshes {
                    arr.extend_from_slice(m);
                }
                st.mesh_instances += n as u32;
            } else {
                for s in &d.sprites {
                    arr.extend_from_slice(s);
                }
                st.sprites += n as u32;
            }
            st.emitters += 1;
            pf.draws.push(PDraw { mat, variant, mesh, first, count: n as u32 });
        }
        st.draws = pf.draws.len() as u32;
        self.raw.clear();
        self.rows.clear();
        self.last = st;
        pf.stats = st;
        pf
    }
}

/// UE3 row-vector transform: p' = [p 1] * M.
fn xform(m: &[f32; 16], p: [f32; 3]) -> [f32; 3] {
    [
        p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12],
        p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
        p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14],
    ]
}

/// Largest coordinate or size an instance may carry (UE3 units; levels span about 1e5).
const SANE: f32 = 1.0e7;

/// An instance the GPU can draw: every float finite, position and size (or matrix) within `SANE`.
pub fn sane_instance(x: &[f32; 20], mesh: bool) -> bool {
    if x.iter().any(|v| !v.is_finite()) {
        return false;
    }
    if mesh {
        x[..16].iter().all(|v| v.abs() < SANE)
    } else {
        x[..3].iter().all(|v| v.abs() < SANE) && x[8].abs() < SANE && x[9].abs() < SANE
    }
}

/// Estimated on-screen area of a sprite emitter's particles, in screens: each sprite's projected square
/// (its larger size) as a fraction of the viewport, capped at 1 (a sprite can't cover more than the screen in one
/// layer); a sprite straddling the camera counts 1, one behind it 0. `view_proj` is column-major, UE3 space.
/// A sprite emitter's size scale from its component's LocalToWorld (UE3 row-major: rows 0 and 1 are the scaled X and
/// Y axes). [1, 1] for a zero or non-finite transform (feeds that only send it for local-space emitters).
pub fn sprite_scale(t: &[f32; 16]) -> [f32; 2] {
    let len = |a: f32, b: f32, c: f32| (a * a + b * b + c * c).sqrt();
    let (x, y) = (len(t[0], t[1], t[2]), len(t[4], t[5], t[6]));
    if x.is_finite() && y.is_finite() && x > 1.0e-3 && y > 1.0e-3 && x < 1.0e3 && y < 1.0e3 { [x, y] } else { [1.0, 1.0] }
}

pub fn sprite_fill(e: &N1Emitter, rows: &[f32], view_proj: &[f32; 16]) -> f32 {
    let m = view_proj;
    let len = |a: f32, b: f32, c: f32| (a * a + b * b + c * c).sqrt();
    let (sx, sy) = (len(m[0], m[4], m[8]), len(m[1], m[5], m[9]));
    let rf = e.row_floats as usize;
    let mut total = 0.0f32;
    for k in 0..e.count as usize {
        let at = e.row_offset as usize + k * rf;
        let Some(r) = rows.get(at..at + 15) else { break };
        let mut p = [r[0], r[1], r[2]];
        let mut s = r[6].abs().max(r[7].abs());
        if e.local_space != 0 {
            p = xform(&e.transform, p);
            let t = &e.transform;
            s *= len(t[0], t[1], t[2]).max(len(t[4], t[5], t[6]));
        } else {
            let k = sprite_scale(&e.transform);
            s *= k[0].max(k[1]);
        }
        if !(s.is_finite() && p.iter().all(|v| v.is_finite())) {
            continue;
        }
        let w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        let f = if w <= -s {
            0.0
        } else if w < s.max(1.0) {
            1.0
        } else {
            ((s * sx / w) * (s * sy / w) / 4.0).min(1.0)
        };
        total += f;
    }
    total
}
