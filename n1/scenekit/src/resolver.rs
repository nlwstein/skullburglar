//! The resolver: joins a live UE3 scene (the scene feed's primitives and camera) to the disc's assets and to the
//! livegfx renderer.
//!
//! The scene side names things by path (mesh `MIS_Helicopter.C1StaticMeshes.Canyon04A`, material, the component's
//! own path for its baked light). `Resolver` hands out a stable **handle** per path the first time it is asked
//! (cheap, no I/O), loads the asset (synchronously, or on a background thread), and uploads it to the renderer.
//! Per frame the caller passes instances by handle; `Resolver::translate` maps handles to renderer ids, dropping
//! instances whose mesh isn't loaded yet. This is the shape the C ABI exposes: the port asks for handles once per
//! component and then sends plain structs every frame.

use crate::{
    Backend, Blend, Instance, MaterialDesc, MeshDesc, NONE, SKIP, SectionDesc, TexFormat, TextureDesc,
};
use anyhow::{Context, Result};
use assetdb::{AssetDb, ComponentLight, MaterialAsset, MeshAsset, TexFmt, TextureAsset};
use serde::Deserialize;
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SlotState {
    Pending,
    Ready(u32),
    Missing,
}

struct Slot {
    path: String,
    state: SlotState,
    requested: bool,
    /// `translate` call that last named it, or the one in progress when it was loaded (B-287: what eviction goes by).
    last_used: u64,
    /// Requested by a preload (B-199), not by a draw.
    preload: bool,
    /// B-287: given back by eviction and not asked for since: not waiting for anything.
    idle: bool,
}

/// A run-time material instance: the scene feed puts its parameter values after `?` in the path.
fn is_variant(path: &str) -> bool {
    path.contains('?')
}

/// What the loader thread hands back: CPU-side assets ready to upload.
enum Loaded {
    Mesh {
        handle: u32,
        mesh: Option<Arc<MeshAsset>>,
        materials: Vec<(String, MatLoad)>,
    },
    Material {
        handle: u32,
        material: MatLoad,
    },
    Light {
        handle: u32,
        light: Option<Arc<ComponentLight>>,
        lightmaps: [Option<(String, Arc<TextureAsset>)>; 3],
    },
}

/// A material and every texture it samples, loaded.
struct MatLoad {
    material: Arc<MaterialAsset>,
    diffuse: Option<Arc<TextureAsset>>,
    /// `material.def.textures`, in order.
    textures: Vec<Option<Arc<TextureAsset>>>,
}

/// What `Resolver::preload` loads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PreloadKind {
    Mesh,
    Material,
    Light,
}

enum Request {
    Mesh(u32, String),
    Material(u32, String),
    Light(u32, String),
}

/// B-287: what level-change eviction has done, cumulative since start (`Resolver::evict`).
#[derive(Default, Clone, Copy, Debug)]
pub struct EvictCounters {
    /// Level boundaries seen (`level_boundary`) and sweeps that ran.
    pub boundaries: u64,
    pub sweeps: u64,
    pub meshes: u64,
    pub materials: u64,
    pub textures: u64,
    pub lights: u64,
    /// Textures a sweep wanted to give back but couldn't yet (copy still queued): kept until the next sweep.
    pub textures_kept: u64,
    /// The last sweep and the longest one, ms of render-thread time (CPU-side marking and releasing; the GPU frees and
    /// the CPU-copy drops are spread over later frames).
    pub last_ms: f32,
    pub max_ms: f32,
    /// CPU copies still waiting to be dropped (`drain_forgets`).
    pub forgets_pending: u32,
}

/// B-287: the clock value eviction goes by: what was last named before it is dead. `keep` levels stay resident.
pub fn eviction_cutoff(boundaries: &[u64], keep: usize) -> Option<u64> {
    let keep = keep.max(1);
    (boundaries.len() >= keep).then(|| boundaries[boundaries.len() - keep])
}

/// B-287: texture ids still wanted: those live materials sample plus live lights' light maps.
fn live_textures<'a>(
    mats: impl Iterator<Item = &'a Vec<u32>>,
    lights: impl Iterator<Item = &'a [u32; 3]>,
) -> HashSet<u32> {
    let mut s = HashSet::new();
    for m in mats {
        s.extend(m.iter().copied());
    }
    for l in lights {
        s.extend(l.iter().copied().filter(|&t| t != NONE));
    }
    s
}

/// B-287: a CPU-side copy `AssetDb` holds for something eviction gave back: dropped later, a few per frame.
enum Forget {
    Mesh(String),
    Material(String),
    Texture(String),
}

#[derive(Default, Clone, Copy, Debug)]
pub struct ResolveStats {
    pub meshes_ready: usize,
    pub meshes_missing: usize,
    pub meshes_pending: usize,
    /// Meshes, materials and lights requested from the loader and not back yet.
    pub in_flight: usize,
    /// B-199: of those, the ones a preload asked for.
    pub preload_in_flight: usize,
}

/// B-199: `BOTS_N1_LAZY_LOG=<n>` prints the first n loads a draw asked for (what the level preload missed).
pub fn lazy_log(kind: &str, path: &str) {
    use std::sync::atomic::{AtomicU32, Ordering};
    static LIMIT: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    static N: AtomicU32 = AtomicU32::new(0);
    let limit = *LIMIT.get_or_init(|| std::env::var("BOTS_N1_LAZY_LOG").ok().and_then(|v| v.parse().ok()).unwrap_or(0));
    if limit > 0 && N.fetch_add(1, Ordering::Relaxed) < limit {
        eprintln!("n1ffi: lazy {kind} {path}");
    }
}

/// B-199 counters since start: loads a draw had to ask for (nothing preloaded them: "lazy"), draws that found a
/// preloaded asset still loading ("late"), preload requests, and what the uploads cost on the calling thread.
#[derive(Default, Clone, Copy, Debug)]
pub struct LoadCounters {
    pub lazy_meshes: u64,
    pub lazy_materials: u64,
    /// Run-time parameter variants of a resident material (`Base?&s:Name=v...`, a MIC the game builds per effect
    /// instance): no package holds them, so no scan can preload them. They are built on the loader thread from data
    /// already resident and drawn with their base material until then, so they are counted here, not as lazy.
    pub variant_materials: u64,
    pub lazy_lights: u64,
    pub late_skips: u64,
    pub preload_requests: u64,
    pub uploads: u64,
    pub upload_us: u64,
    pub upload_max_us: u64,
}

pub struct Resolver {
    pub db: Arc<AssetDb>,
    meshes: Vec<Slot>,
    mesh_by_path: HashMap<String, u32>,
    materials: Vec<Slot>,
    material_by_path: HashMap<String, u32>,
    lights: Vec<Slot>,
    light_by_path: HashMap<String, u32>,
    // renderer ids of things uploaded by path
    tex_ids: HashMap<String, u32>,
    mat_ids: HashMap<String, u32>,
    tx: Option<mpsc::Sender<Request>>,
    rx: Option<Mutex<mpsc::Receiver<Loaded>>>,
    pub ctr: LoadCounters,
    /// B-199: level packages whose lit components were all preloaded: a component of one that isn't in the list has
    /// no baked light, and is marked so without a loader round trip.
    lights_complete: std::collections::HashSet<String>,
    // per-frame scratch
    out_instances: Vec<Instance>,
    out_overrides: Vec<u32>,
    /// `translate` calls so far (the clock for `last_used`).
    frame: u64,
    /// B-287: renderer material ids each mesh slot's sections use, texture ids each material samples (diffuse first),
    /// and the light-map texture ids of each light slot: what eviction counts to know what is still referenced.
    mesh_mats: HashMap<u32, Vec<u32>>,
    mat_tex: HashMap<u32, Vec<u32>>,
    light_tex: HashMap<u32, [u32; 3]>,
    /// B-287: the clock value at each level change (`level_boundary`).
    boundaries: Vec<u64>,
    forgets: VecDeque<Forget>,
    pub evict: EvictCounters,
}

fn tex_format(f: TexFmt) -> TexFormat {
    match f {
        TexFmt::Bc1 => TexFormat::Bc1,
        TexFmt::Bc2 => TexFormat::Bc2,
        TexFmt::Bc3 => TexFormat::Bc3,
        TexFmt::Bc5 => TexFormat::Bc5,
        TexFmt::R8 => TexFormat::R8,
        TexFmt::Bgra8 => TexFormat::Bgra8,
    }
}

fn load_material_assets(db: &AssetDb, path: &str) -> MatLoad {
    let m = db.material(path);
    let diffuse = m.diffuse.as_ref().and_then(|d| db.texture(d));
    let textures = m
        .def
        .as_ref()
        .map(|d| d.textures.iter().map(|t| db.texture(&t.path)).collect())
        .unwrap_or_default();
    MatLoad { material: m, diffuse, textures }
}

fn load_light(db: &AssetDb, component: &str) -> Loaded {
    let light = db.component_light_full(component);
    let mut lightmaps: [Option<(String, Arc<TextureAsset>)>; 3] = Default::default();
    if let Some(ComponentLight::Map { textures, .. }) = light.as_deref() {
        for (k, t) in textures.iter().enumerate() {
            if let Some((p, i)) = t {
                lightmaps[k] = db.lightmap_texture(p, *i).map(|a| (format!("{p}::#{i}"), a));
            }
        }
    }
    Loaded::Light { handle: 0, light, lightmaps }
}

impl Resolver {
    pub fn new(db: Arc<AssetDb>) -> Self {
        Self {
            db,
            meshes: Vec::new(),
            mesh_by_path: HashMap::new(),
            materials: Vec::new(),
            material_by_path: HashMap::new(),
            lights: Vec::new(),
            light_by_path: HashMap::new(),
            tex_ids: HashMap::new(),
            mat_ids: HashMap::new(),
            tx: None,
            rx: None,
            ctr: LoadCounters::default(),
            lights_complete: Default::default(),
            out_instances: Vec::new(),
            out_overrides: Vec::new(),
            frame: 0,
            mesh_mats: HashMap::new(),
            mat_tex: HashMap::new(),
            light_tex: HashMap::new(),
            boundaries: Vec::new(),
            forgets: VecDeque::new(),
            evict: EvictCounters::default(),
        }
    }

    /// Load on `threads` background threads from now on (`pump` uploads what they finished).
    pub fn start_loader(&mut self, threads: usize) {
        let (tx, rx) = mpsc::channel::<Request>();
        let (ltx, lrx) = mpsc::channel::<Loaded>();
        let rx = Arc::new(Mutex::new(rx));
        for _ in 0..threads.max(1) {
            let (db, rx, ltx) = (self.db.clone(), rx.clone(), ltx.clone());
            std::thread::Builder::new()
                .name("livescene-loader".into())
                .spawn(move || {
                    loop {
                        let req = { rx.lock().unwrap().recv() };
                        let Ok(req) = req else { break };
                        let out = match req {
                            Request::Mesh(h, p) => {
                                let mesh = db.mesh(&p);
                                let materials = mesh
                                    .iter()
                                    .flat_map(|m| m.sections.iter().map(|s| s.material.clone()))
                                    .filter(|m| !m.is_empty())
                                    .map(|mp| {
                                        let m = load_material_assets(&db, &mp);
                                        (mp, m)
                                    })
                                    .collect();
                                Loaded::Mesh {
                                    handle: h,
                                    mesh,
                                    materials,
                                }
                            }
                            Request::Material(h, p) => Loaded::Material {
                                handle: h,
                                material: load_material_assets(&db, &p),
                            },
                            Request::Light(h, p) => match load_light(&db, &p) {
                                Loaded::Light { light, lightmaps, .. } => Loaded::Light { handle: h, light, lightmaps },
                                l => l,
                            },
                        };
                        if ltx.send(out).is_err() {
                            break;
                        }
                    }
                })
                .expect("spawn loader");
        }
        self.tx = Some(tx);
        self.rx = Some(Mutex::new(lrx));
    }

    fn handle(slots: &mut Vec<Slot>, by_path: &mut HashMap<String, u32>, path: &str) -> u32 {
        if let Some(&h) = by_path.get(path) {
            return h;
        }
        slots.push(Slot {
            path: path.to_string(),
            state: SlotState::Pending,
            requested: false,
            last_used: 0,
            preload: false,
            idle: false,
        });
        let h = slots.len() as u32 - 1;
        by_path.insert(path.to_string(), h);
        h
    }

    /// Handle for a StaticMesh path (no I/O; loading starts on first use).
    pub fn mesh_handle(&mut self, path: &str) -> u32 {
        Self::handle(&mut self.meshes, &mut self.mesh_by_path, path)
    }
    /// Handle for a material path ("" gives NONE: keep the mesh section's own).
    pub fn material_handle(&mut self, path: &str) -> u32 {
        if path.is_empty() {
            return NONE;
        }
        Self::handle(&mut self.materials, &mut self.material_by_path, path)
    }
    /// Handle for a placed component's baked light (its full path).
    pub fn light_handle(&mut self, component: &str) -> u32 {
        Self::handle(&mut self.lights, &mut self.light_by_path, component)
    }

    pub fn mesh_state(&self, h: u32) -> Option<SlotState> {
        self.meshes.get(h as usize).map(|s| s.state)
    }
    pub fn mesh_path(&self, h: u32) -> Option<&str> {
        self.meshes.get(h as usize).map(|s| s.path.as_str())
    }
    /// A light handle's state (no load is requested).
    pub fn light_state(&self, h: u32) -> Option<SlotState> {
        self.lights.get(h as usize).map(|s| s.state)
    }

    pub fn stats(&self) -> ResolveStats {
        let mut s = ResolveStats::default();
        for m in &self.meshes {
            match m.state {
                SlotState::Ready(_) => s.meshes_ready += 1,
                SlotState::Missing => s.meshes_missing += 1,
                // B-287: a slot eviction gave back and nothing has asked for since isn't waiting for anything.
                SlotState::Pending if !m.requested && m.idle => {}
                SlotState::Pending => s.meshes_pending += 1,
            }
        }
        for x in [&self.meshes, &self.materials, &self.lights].iter().flat_map(|v| v.iter()) {
            if x.requested && x.state == SlotState::Pending {
                s.in_flight += 1;
                s.preload_in_flight += x.preload as usize;
            }
        }
        s
    }

    /// B-199: asks the loader for a mesh (and its materials and textures), a material or a component's baked light
    /// ahead of any draw. Async mode only; a no-op for anything already requested. Returns true when it queued.
    pub fn preload(&mut self, kind: PreloadKind, path: &str) -> bool {
        if self.tx.is_none() || path.is_empty() {
            return false;
        }
        let (slots, h) = match kind {
            PreloadKind::Mesh => {
                let h = self.mesh_handle(path);
                (&mut self.meshes, h)
            }
            PreloadKind::Material => {
                let h = self.material_handle(path);
                (&mut self.materials, h)
            }
            PreloadKind::Light => {
                let h = self.light_handle(path);
                (&mut self.lights, h)
            }
        };
        let slot = &mut slots[h as usize];
        if slot.requested || slot.state != SlotState::Pending {
            return false;
        }
        slot.requested = true;
        slot.preload = true;
        let req = match kind {
            PreloadKind::Mesh => Request::Mesh(h, slot.path.clone()),
            PreloadKind::Material => Request::Material(h, slot.path.clone()),
            PreloadKind::Light => Request::Light(h, slot.path.clone()),
        };
        let _ = self.tx.as_ref().unwrap().send(req);
        self.ctr.preload_requests += 1;
        true
    }

    /// B-199: every lit component of level package `pkg` has been preloaded (see `lights_complete`).
    pub fn set_lights_complete(&mut self, pkg: &str) {
        self.lights_complete.insert(pkg.to_string());
    }

    /// `gamma`: upload raw for the shader's PWL curve (B-235); it overrides `srgb`.
    fn upload_texture(&mut self, r: &mut impl Backend, name: &str, t: &TextureAsset, srgb: bool, gamma: bool) -> u32 {
        let srgb = srgb && !gamma;
        let key = format!("{name}|{}{}", srgb as u8, gamma as u8);
        if let Some(&id) = self.tex_ids.get(&key) {
            return id;
        }
        let mips: Vec<&[u8]> = t.mips.iter().map(|m| m.as_slice()).collect();
        let id = r.add_texture(&TextureDesc {
            format: tex_format(t.fmt),
            srgb,
            width: t.width,
            height: t.height,
            mips: &mips,
            normal: t.normal,
            gamma,
        });
        self.tex_ids.insert(key, id);
        id
    }

    fn upload_material(&mut self, r: &mut impl Backend, path: &str, l: &MatLoad) -> u32 {
        if let Some(&id) = self.mat_ids.get(path) {
            return id;
        }
        let m = &l.material;
        // Diffuse is sampled as sRGB, as in n1gfx (or PWL, B-235, when the original fetches it with the gamma sign).
        let texture = match (m.diffuse.as_ref(), l.diffuse.as_ref()) {
            (Some(name), Some(t)) => self.upload_texture(r, name, t, true, crate::tex_gamma(t.srgb, t.normal, t.fmt)),
            _ => NONE,
        };
        let def = m.def.as_deref();
        let mut textures = Vec::new();
        let mut unpack = Vec::new();
        if let Some(d) = def {
            for (k, tr) in d.textures.iter().enumerate() {
                match l.textures.get(k).and_then(|t| t.as_ref()) {
                    Some(t) => {
                        textures.push(self.upload_texture(
                            r,
                            &tr.path,
                            t,
                            t.srgb && !t.normal,
                            crate::tex_gamma(t.srgb, t.normal, t.fmt),
                        ));
                        unpack.push(t.unpack);
                    }
                    None => {
                        textures.push(NONE);
                        unpack.push(None);
                    }
                }
            }
        }
        let mut used: Vec<u32> = std::iter::once(texture).chain(textures.iter().copied()).filter(|&t| t != NONE).collect();
        used.sort_unstable();
        used.dedup();
        let id = r.add_material(&MaterialDesc {
            texture,
            blend: Blend::from_ue3(def.map(|d| d.blend.as_str()).filter(|b| !b.is_empty()).unwrap_or(&m.blend)),
            two_sided: m.two_sided || def.is_some_and(|d| d.two_sided),
            def,
            textures: &textures,
            unpack: &unpack,
        });
        static LOG: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        if *LOG.get_or_init(|| std::env::var_os("BOTS_N1_COMPILE_LOG").is_some()) {
            eprintln!("n1ffi: material #{id} is {path}");
        }
        self.mat_ids.insert(path.to_string(), id);
        self.mat_tex.insert(id, used);
        id
    }

    fn upload_mesh(
        &mut self,
        r: &mut impl Backend,
        m: &MeshAsset,
        mats: &[(String, MatLoad)],
    ) -> (u32, Vec<u32>) {
        let sections: Vec<SectionDesc> = m
            .sections
            .iter()
            .map(|s| {
                let mat = if s.material.is_empty() {
                    NONE
                } else if let Some(&id) = self.mat_ids.get(&s.material) {
                    id
                } else if let Some((p, ml)) = mats.iter().find(|x| x.0 == s.material) {
                    self.upload_material(r, p, ml)
                } else {
                    NONE
                };
                SectionDesc {
                    first_index: s.first_index,
                    index_count: s.index_count,
                    material: mat,
                }
            })
            .collect();
        let mut used: Vec<u32> = sections.iter().map(|s| s.material).filter(|&x| x != NONE).collect();
        used.sort_unstable();
        used.dedup();
        let id = r.add_mesh(&MeshDesc {
            positions: &m.positions,
            uvs: &m.uvs,
            normals: &m.normals,
            tangents: &m.tangents,
            uv1: &m.uv1,
            colors: &m.colors,
            indices: &m.indices,
            sections: &sections,
            center: m.center,
            radius: m.radius,
        });
        (id, used)
    }

    fn apply(&mut self, r: &mut impl Backend, l: Loaded) {
        match l {
            Loaded::Mesh {
                handle,
                mesh,
                materials,
            } => {
                let st = match mesh {
                    Some(m) => {
                        let (id, mats) = self.upload_mesh(r, &m, &materials);
                        self.mesh_mats.insert(handle, mats);
                        SlotState::Ready(id)
                    }
                    None => SlotState::Missing,
                };
                self.meshes[handle as usize].state = st;
                self.meshes[handle as usize].last_used = self.frame;
            }
            Loaded::Material { handle, material } => {
                let path = self.materials[handle as usize].path.clone();
                let id = self.upload_material(r, &path, &material);
                self.materials[handle as usize].state = SlotState::Ready(id);
                self.materials[handle as usize].last_used = self.frame;
            }
            Loaded::Light { handle, light, lightmaps } => {
                let mut ids = [NONE; 3];
                // B-189: the port fetches lightmap textures with the gamma sign (sg3F in BOTS_TRACE_SIGNS), so they are
                // linearised before the scale vectors apply. B-235: with the gamma sign the hardware's curve is
                // piecewise-linear and far from sRGB in the darks (0.1 -> 0.025 vs 0.010), so they are uploaded raw and
                // the scene shader applies it (`lm_pwl`). BOTS_N1_SCENE_PWL=0 samples them as sRGB again;
                // BOTS_N1_LM_SRGB=0 restores the old raw sampling.
                let (lm_srgb, lm_gamma) = (crate::lm_linear(), crate::lm_pwl());
                for (k, t) in lightmaps.iter().enumerate() {
                    if let Some((name, t)) = t {
                        ids[k] = self.upload_texture(r, name, t, lm_srgb, lm_gamma);
                    }
                }
                if std::env::var_os("BOTS_N1_LIGHT_LOG").is_some() {
                    let kind = match light.as_deref() {
                        None => "none",
                        Some(ComponentLight::Legacy(_)) => "legacy",
                        Some(ComponentLight::Dir { .. }) => "dir",
                        Some(ComponentLight::Map { .. }) => "map",
                    };
                    eprintln!("n1 light {} {kind} lm_ids {:?}", self.lights[handle as usize].path, ids);
                }
                self.lights[handle as usize].state = match light.map(|l| r.add_component_light(&l, ids)) {
                    Some(id) if id != NONE => SlotState::Ready(id),
                    _ => SlotState::Missing,
                };
                self.lights[handle as usize].last_used = self.frame;
                self.light_tex.insert(handle, ids);
            }
        }
    }

    /// Async mode: upload what the loader finished, at most `budget` assets. Sync mode: no-op.
    pub fn pump(&mut self, r: &mut impl Backend, budget: usize) -> usize {
        self.pump_until(r, budget, None)
    }

    /// As `pump`, and stops once `deadline` has passed (at least one asset is taken per call when one is waiting).
    pub fn pump_until(&mut self, r: &mut impl Backend, budget: usize, deadline: Option<std::time::Instant>) -> usize {
        let mut n = 0;
        while n < budget {
            if n > 0 && deadline.is_some_and(|d| std::time::Instant::now() >= d) {
                break;
            }
            let l = match &self.rx {
                Some(rx) => match rx.lock().unwrap().try_recv() {
                    Ok(l) => l,
                    Err(_) => break,
                },
                None => break,
            };
            let t0 = std::time::Instant::now();
            self.apply(r, l);
            let us = t0.elapsed().as_micros() as u64;
            self.ctr.uploads += 1;
            self.ctr.upload_us += us;
            self.ctr.upload_max_us = self.ctr.upload_max_us.max(us);
            n += 1;
        }
        n
    }

    fn ensure_mesh(&mut self, r: &mut impl Backend, h: u32) -> Option<u32> {
        let frame = self.frame;
        let slot = self.meshes.get_mut(h as usize)?;
        slot.last_used = frame;
        match slot.state {
            SlotState::Ready(id) => return Some(id),
            SlotState::Missing => return None,
            SlotState::Pending if slot.requested => {
                self.ctr.late_skips += slot.preload as u64;
                return None;
            }
            SlotState::Pending => {}
        }
        slot.requested = true;
        let path = slot.path.clone();
        if let Some(tx) = &self.tx {
            self.ctr.lazy_meshes += 1;
            lazy_log("mesh", &path);
            let _ = tx.send(Request::Mesh(h, path));
            return None;
        }
        let mesh = self.db.mesh(&path);
        let materials = mesh
            .iter()
            .flat_map(|m| m.sections.iter().map(|s| s.material.clone()))
            .filter(|m| !m.is_empty())
            .map(|mp| {
                let m = load_material_assets(&self.db, &mp);
                (mp, m)
            })
            .collect();
        self.apply(
            r,
            Loaded::Mesh {
                handle: h,
                mesh,
                materials,
            },
        );
        match self.meshes[h as usize].state {
            SlotState::Ready(id) => Some(id),
            _ => None,
        }
    }

    /// The renderer's material id for a material handle, requesting the load if needed (NONE until it is in).
    pub fn material_id(&mut self, r: &mut impl Backend, h: u32) -> u32 {
        if h == NONE {
            NONE
        } else {
            self.ensure_material(r, h)
        }
    }

    /// The renderer id for material handle `h`: NONE while it loads or when it is missing (the mesh's own material is
    /// drawn), except SKIP for a run-time variant still loading (see `SKIP`).
    fn ensure_material(&mut self, r: &mut impl Backend, h: u32) -> u32 {
        let frame = self.frame;
        let Some(slot) = self.materials.get_mut(h as usize) else {
            return NONE;
        };
        slot.last_used = frame;
        let pending = if is_variant(&slot.path) { SKIP } else { NONE };
        match slot.state {
            SlotState::Ready(id) => return id,
            SlotState::Missing => return NONE,
            SlotState::Pending if slot.requested => return pending,
            SlotState::Pending => {}
        }
        let path = slot.path.clone();
        self.materials[h as usize].requested = true;
        if let Some(tx) = &self.tx {
            // A variant still loading is skipped, not drawn with its base: a bullet tracer's base is the cooked
            // full-length, unfaded slab (B-200).
            if is_variant(&path) {
                self.ctr.variant_materials += 1;
            } else {
                self.ctr.lazy_materials += 1;
                lazy_log("material", &path);
            }
            let _ = tx.send(Request::Material(h, path));
            return pending;
        }
        let material = load_material_assets(&self.db, &path);
        self.apply(r, Loaded::Material { handle: h, material });
        match self.materials[h as usize].state {
            SlotState::Ready(id) => id,
            _ => NONE,
        }
    }

    /// Releases the renderer materials of run-time variants no instance named in the last `idle` frames (the handle
    /// stays valid and reloads if it is named again). Tracer smoke makes a variant per shot, so without this a long
    /// firefight grows without bound. Returns how many were released.
    pub fn release_idle_variants(&mut self, r: &mut impl Backend, idle: u64) -> usize {
        let mut n = 0;
        for slot in &mut self.materials {
            let SlotState::Ready(id) = slot.state else { continue };
            if self.frame.saturating_sub(slot.last_used) <= idle || !is_variant(&slot.path) {
                continue;
            }
            if self.mat_ids.remove(&slot.path).is_some() {
                r.remove_material(id);
                self.mat_tex.remove(&id);
            }
            self.db.forget_material(&slot.path);
            slot.state = SlotState::Pending;
            slot.requested = false;
            n += 1;
        }
        n
    }

    /// B-287: a level change. Counts the boundary and, when `keep` levels have passed, gives back every mesh, material,
    /// light and texture nothing named since the boundary `keep` levels ago (so the last `keep` levels stay resident:
    /// what the level you just left and the one before it used, plus anything that shows up every level). Handles stay
    /// valid; a later reference reloads lazily. Returns true when a sweep ran.
    pub fn level_boundary(&mut self, r: &mut impl Backend, keep: usize) -> bool {
        self.frame += 1;
        self.boundaries.push(self.frame);
        self.evict.boundaries += 1;
        let Some(cutoff) = eviction_cutoff(&self.boundaries, keep) else {
            return false;
        };
        let t0 = std::time::Instant::now();
        self.sweep(r, cutoff);
        let ms = t0.elapsed().as_secs_f32() * 1000.0;
        self.evict.sweeps += 1;
        self.evict.last_ms = ms;
        self.evict.max_ms = self.evict.max_ms.max(ms);
        true
    }

    fn sweep(&mut self, r: &mut impl Backend, cutoff: u64) {
        // Meshes first (they hold materials), then materials, lights, and last the textures nothing left uses.
        let mut live_mats: HashSet<u32> = HashSet::new();
        for (h, slot) in self.meshes.iter_mut().enumerate() {
            let SlotState::Ready(id) = slot.state else { continue };
            if slot.last_used >= cutoff {
                if let Some(m) = self.mesh_mats.get(&(h as u32)) {
                    live_mats.extend(m.iter().copied());
                }
                continue;
            }
            r.free_mesh(id);
            self.mesh_mats.remove(&(h as u32));
            self.forgets.push_back(Forget::Mesh(slot.path.clone()));
            slot.state = SlotState::Pending;
            slot.requested = false;
            slot.idle = true;
            self.evict.meshes += 1;
        }
        for slot in &self.materials {
            if let SlotState::Ready(id) = slot.state {
                if slot.last_used >= cutoff {
                    live_mats.insert(id);
                }
            }
        }
        let dead_mats: Vec<(String, u32)> =
            self.mat_ids.iter().filter(|(_, id)| !live_mats.contains(id)).map(|(p, &id)| (p.clone(), id)).collect();
        let dead_ids: HashSet<u32> = dead_mats.iter().map(|d| d.1).collect();
        for (path, id) in dead_mats {
            self.mat_ids.remove(&path);
            r.remove_material(id);
            self.mat_tex.remove(&id);
            self.forgets.push_back(Forget::Material(path));
            self.evict.materials += 1;
        }
        for slot in &mut self.materials {
            if matches!(slot.state, SlotState::Ready(id) if dead_ids.contains(&id)) {
                slot.state = SlotState::Pending;
                slot.requested = false;
            }
        }
        let mut live_lights: Vec<u32> = Vec::new();
        for (h, slot) in self.lights.iter_mut().enumerate() {
            let SlotState::Ready(id) = slot.state else { continue };
            if slot.last_used >= cutoff {
                live_lights.push(h as u32);
                continue;
            }
            r.free_light(id);
            // Its package's lights were all preloaded once (`lights_complete`: a light not known yet is "none"). Now one
            // is gone, so a later reference must load it again instead of reading as unlit.
            if let Some(pkg) = slot.path.split('.').next() {
                self.lights_complete.remove(pkg);
            }
            self.light_tex.remove(&(h as u32));
            slot.state = SlotState::Pending;
            slot.requested = false;
            self.evict.lights += 1;
        }
        let live_tex = live_textures(self.mat_ids.values().filter_map(|id| self.mat_tex.get(id)), live_lights.iter().filter_map(|h| self.light_tex.get(h)));
        let dead_tex: Vec<(String, u32)> =
            self.tex_ids.iter().filter(|(_, id)| !live_tex.contains(id)).map(|(k, &id)| (k.clone(), id)).collect();
        for (key, id) in dead_tex {
            if r.free_texture(id) {
                self.tex_ids.remove(&key);
                self.evict.textures += 1;
                let name = key.rsplit_once('|').map_or(key.as_str(), |x| x.0).to_string();
                let still = self.tex_ids.keys().any(|k| k.rsplit_once('|').is_some_and(|x| x.0 == name));
                if !still {
                    self.forgets.push_back(Forget::Texture(name));
                }
            } else {
                self.evict.textures_kept += 1;
            }
        }
        self.evict.forgets_pending = self.forgets.len() as u32;
    }

    /// B-287: drops queued CPU copies from `AssetDb`, as many as fit in `budget`.
    pub fn drain_forgets(&mut self, budget: std::time::Duration) {
        if self.forgets.is_empty() {
            return;
        }
        let t0 = std::time::Instant::now();
        while let Some(f) = self.forgets.pop_front() {
            match f {
                Forget::Mesh(p) => self.db.forget_mesh(&p),
                Forget::Material(p) => self.db.forget_material(&p),
                Forget::Texture(p) => self.db.forget_texture(&p),
            }
            if t0.elapsed() >= budget {
                break;
            }
        }
        self.evict.forgets_pending = self.forgets.len() as u32;
    }

    fn ensure_light(&mut self, r: &mut impl Backend, h: u32) -> u32 {
        let frame = self.frame;
        let Some(slot) = self.lights.get_mut(h as usize) else {
            return NONE;
        };
        slot.last_used = frame;
        match slot.state {
            SlotState::Ready(id) => return id,
            SlotState::Missing => return NONE,
            SlotState::Pending if slot.requested => return NONE,
            SlotState::Pending => {}
        }
        slot.requested = true;
        let path = slot.path.clone();
        // Components of actors spawned at run time (`Transient.*`) never have baked lighting.
        if path.split('.').next().is_some_and(|pkg| pkg == "Transient" || self.lights_complete.contains(pkg)) {
            slot.state = SlotState::Missing;
            return NONE;
        }
        if let Some(tx) = &self.tx {
            self.ctr.lazy_lights += 1;
            lazy_log("light", &path);
            let _ = tx.send(Request::Light(h, path));
            return NONE;
        }
        if let Loaded::Light { light, lightmaps, .. } = load_light(&self.db, &path) {
            self.apply(r, Loaded::Light { handle: h, light, lightmaps });
        }
        match self.lights[h as usize].state {
            SlotState::Ready(id) => id,
            _ => NONE,
        }
    }

    /// Handles -> renderer ids for one frame. Input instances carry handles (mesh, light, and material handles in
    /// `overrides`); returns the renderer-ready instances and override ids (borrowed until the next call).
    pub fn translate(
        &mut self,
        r: &mut impl Backend,
        instances: &[Instance],
        overrides: &[u32],
    ) -> (&[Instance], &[u32]) {
        self.frame += 1;
        let mut out_i = std::mem::take(&mut self.out_instances);
        let mut out_o = std::mem::take(&mut self.out_overrides);
        out_i.clear();
        out_o.clear();
        for inst in instances {
            let Some(mesh) = self.ensure_mesh(r, inst.mesh) else {
                continue;
            };
            let light = if inst.light == NONE {
                NONE
            } else {
                self.ensure_light(r, inst.light)
            };
            let off = out_o.len() as u32;
            for k in 0..inst.mat_count {
                let h = overrides
                    .get((inst.mat_offset + k) as usize)
                    .copied()
                    .unwrap_or(NONE);
                out_o.push(if h == NONE {
                    NONE
                } else {
                    self.ensure_material(r, h)
                });
            }
            out_i.push(Instance {
                transform: inst.transform,
                mesh,
                light,
                mat_offset: off,
                mat_count: inst.mat_count,
                env: inst.env,
            });
        }
        self.out_instances = out_i;
        self.out_overrides = out_o;
        (&self.out_instances, &self.out_overrides)
    }
}

// ---- the scene feed's JSON (spike format; see skullburglar src/scene_feed.h)

#[derive(Deserialize, Clone)]
pub struct FeedCamera {
    pub location: [f32; 3],
    pub rotation: [i32; 3],
    pub fov: f32,
    pub view: [f32; 16],
    pub proj: [f32; 16],
}

#[derive(Deserialize, Clone)]
pub struct FeedPrimitive {
    pub kind: String,
    #[serde(default)]
    pub class: String,
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub mesh: String,
    pub transform: Option<[f32; 16]>,
    #[serde(default)]
    pub materials: Vec<String>,
    #[serde(default)]
    pub hidden: bool,
    #[serde(default)]
    pub rendered: bool,
}

#[derive(Deserialize, Clone)]
pub struct FeedFrame {
    pub frame: u64,
    #[serde(default)]
    pub time: f64,
    #[serde(default)]
    pub host_us: u64,
    pub camera: FeedCamera,
    pub primitives: Vec<FeedPrimitive>,
}

impl FeedFrame {
    pub fn load(path: &std::path::Path) -> Result<Self> {
        // The feed writes NaN for un-updated values in a few fields; be lenient by patching bare NaN to null.
        let b = std::fs::read(path).with_context(|| format!("read {}", path.display()))?;
        match serde_json::from_slice(&b) {
            Ok(f) => Ok(f),
            Err(_) => {
                let s = String::from_utf8_lossy(&b)
                    .replace("-nan", "null")
                    .replace("nan", "null")
                    .replace("NaN", "null");
                serde_json::from_str(&s).with_context(|| format!("parse {}", path.display()))
            }
        }
    }

    /// Level packages named by the primitives' component paths (the packages the running level loaded).
    pub fn level_packages(&self) -> Vec<String> {
        let mut v: Vec<String> = self
            .primitives
            .iter()
            .filter_map(|p| p.name.split('.').next().map(str::to_string))
            .collect();
        v.sort();
        v.dedup();
        v
    }
}

#[cfg(test)]
mod evict_tests {
    use super::*;

    #[test]
    fn cutoff_waits_for_keep_levels() {
        assert_eq!(eviction_cutoff(&[], 2), None);
        assert_eq!(eviction_cutoff(&[10], 2), None);
        assert_eq!(eviction_cutoff(&[10, 20], 2), Some(10));
        assert_eq!(eviction_cutoff(&[10, 20, 30], 2), Some(20));
        assert_eq!(eviction_cutoff(&[10, 20, 30], 1), Some(30));
        assert_eq!(eviction_cutoff(&[10], 0), Some(10));
    }

    #[test]
    fn live_textures_union() {
        let a = vec![1, 2];
        let b = vec![2, 3];
        let l = [7, NONE, 9];
        let s = live_textures([&a, &b].into_iter(), [&l].into_iter());
        let mut v: Vec<u32> = s.into_iter().collect();
        v.sort_unstable();
        assert_eq!(v, vec![1, 2, 3, 7, 9]);
    }
}
