//! n1ffi: the live UE3 scene renderer behind a C ABI, for the port (see include/n1ffi.h for the contract).
//!
//! The host (the port's Vulkan backend) owns the instance, device, queue and every command buffer. At the scene
//! marker it calls `n1_record` with its open command buffer (outside any rendering), a colour target, the frame's
//! camera and the primitives as plain structs; we record uploads and the scene into that command buffer and return.
//! The host submits as usual. Nothing here submits, waits or touches a queue.
//!
//! Primitives name assets by handle: `n1_mesh("MIS_Helicopter.C1StaticMeshes.Canyon04A")` etc. hand out a stable
//! handle per path at once (no I/O); loading happens on loader threads and uploads are recorded at the next
//! `n1_record`, a few per frame. Until a mesh is in, its primitives are skipped.
//!
//! All entry points may be called from any thread; they serialise on one mutex (handle lookups are cheap; the host
//! should cache handles per component).

pub mod bcdec;
pub mod decals;
pub mod light;
pub mod particles;
pub mod plan;
pub mod postmat;
pub mod precompile;
pub mod shaders;
pub mod skin;
pub mod store;
pub mod vkr;
pub mod trees;
pub mod xenos_vs;

use ash::vk;
use assetdb::AssetDb;
use scenekit::{Instance, Resolver};
use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char};
use std::path::Path;
use std::sync::atomic::Ordering::Relaxed;
use std::sync::{Arc, Mutex};

pub use vkr::{Renderer, Target, VkContext};

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn set_error(e: impl std::fmt::Display) {
    let s = CString::new(e.to_string().replace('\0', " ")).unwrap_or_default();
    LAST_ERROR.with(|l| *l.borrow_mut() = s);
}

#[repr(C)]
pub struct N1Init {
    pub get_instance_proc_addr: vk::PFN_vkGetInstanceProcAddr,
    pub instance: vk::Instance,
    pub physical_device: vk::PhysicalDevice,
    pub device: vk::Device,
    /// VkFormat of the colour targets passed to `n1_record`. A UNORM format gets sRGB encoding in the shader.
    pub color_format: vk::Format,
    /// `game/C1Game/CookedXenon`.
    pub cooked_dir: *const c_char,
    /// Writable cache directory (index, extracted meshes/textures/light).
    pub cache_dir: *const c_char,
    /// Background loader threads; 0 loads synchronously inside `n1_record` (offline tools only: it stalls).
    pub loader_threads: u32,
}

#[repr(C)]
pub struct N1Camera {
    /// UE3 FSceneView ViewMatrix and ProjectionMatrix as stored (row-major, row vectors).
    pub view: [f32; 16],
    pub proj: [f32; 16],
    pub location: [f32; 3],
    /// Near plane (UE3: 5 in this game's FSceneView).
    pub near_plane: f32,
}

#[repr(C)]
pub struct N1Target {
    pub view: vk::ImageView,
    /// The layout the image is in (and stays in), e.g. VK_IMAGE_LAYOUT_GENERAL.
    pub layout: vk::ImageLayout,
    pub width: u32,
    pub height: u32,
    /// The view rectangle in the target, pixels.
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
    /// Non-zero: clear the rectangle to `clear_color` first (the host scene must cover the whole view).
    pub clear: u32,
    pub clear_color: [f32; 4],
}

/// One primitive this frame: handles from `n1_mesh` / `n1_light` (or N1_NONE), and a range in the frame's material
/// handle array (`n1_material`; N1_NONE entries keep the mesh's own material for that section).
pub type N1Instance = Instance;

#[repr(C)]
#[derive(Default)]
pub struct N1Stats {
    pub instances: u32,
    pub culled: u32,
    pub draws: u32,
    /// Primitives skipped this frame because their mesh isn't loaded (yet, or at all).
    pub skipped: u32,
    pub triangles: u64,
    pub record_ms: f32,
    pub uploads: u32,
    pub meshes_ready: u32,
    pub meshes_pending: u32,
    pub meshes_missing: u32,
    /// Skinned primitives this frame: drawn, culled, skipped (mesh not in yet, or unknown).
    pub skel_drawn: u32,
    pub skel_culled: u32,
    pub skel_skipped: u32,
    pub skel_meshes_ready: u32,
    /// Assets requested from the loader threads and not uploaded yet.
    pub loading: u32,
    pub textures: u32,
    pub textures_unsupported: u32,
    /// N12: material shaders and pipelines made so far; draws waiting for a pipeline this frame (per-frame budget).
    pub shaders: u32,
    pub pipelines: u32,
    pub pipelines_pending: u32,
    /// N12: feed lighting in use: level dynamic lights, light environments, feed frames received.
    pub dyn_lights: u32,
    pub light_envs: u32,
    pub feed_frames: u32,
    /// Instances drawn this frame with a light environment, and env handles known (`n1_env`).
    pub env_bound: u32,
    pub env_handles: u32,
    /// B-186 particles this frame: emitters drawn, sprites and mesh instances drawn, instances dropped (sprite, fill and
    /// mesh caps, plus non-finite rows),
    /// materials ready / failed to build, emitters skipped (material or mesh not loaded yet).
    pub particle_emitters: u32,
    pub particle_sprites: u32,
    pub particle_meshes: u32,
    pub particle_dropped: u32,
    pub particle_mats_ready: u32,
    pub particle_mats_failed: u32,
    pub particle_skipped: u32,
    /// B-187 trees: placements in the feed, instances and draws this frame, assets ready / still to capture / failed,
    /// uncaptured assets the feed needs now, assets captured this run.
    pub tree_placements: u32,
    pub tree_instances: u32,
    pub tree_draws: u32,
    pub tree_assets_ready: u32,
    pub tree_assets_uncaptured: u32,
    pub tree_assets_failed: u32,
    pub tree_wanted: u32,
    pub tree_captured: u32,
    /// W2: shaders and pipelines built since start, by who asked (the startup step, or play), the ones asked for in
    /// play after the startup step was over (`late_*`: what the precompile missed), failures, SPIR-V disk-cache hits
    /// and misses, time spent in `vkCreateGraphicsPipelines` on all threads (worst one separate), naga time, the
    /// compile queue depth, and what the render thread had to do without: draws skipped, drawn with the grey stand-in,
    /// frames with any skipped.
    pub shaders_startup: u32,
    pub shaders_live: u32,
    pub pipelines_startup: u32,
    pub pipelines_live: u32,
    pub late_shaders: u32,
    pub late_pipelines: u32,
    pub build_failures: u32,
    pub spv_hits: u32,
    pub spv_misses: u32,
    pub pipeline_ms: f32,
    pub pipeline_max_ms: f32,
    pub naga_ms: f32,
    pub compile_backlog: u32,
    pub partial_frames: u32,
    pub draws_no_pipe: u64,
    pub fallback_draws: u64,
    /// B-199 load counters, cumulative: loads first asked for by a draw (`lazy_*`: what the level preload missed),
    /// draws skipped because a preloaded asset wasn't in yet, preload requests, loaded assets applied (count, total and
    /// worst render-thread time), bytes copied into GPU memory, textures and meshes created, assets still to come in
    /// from preloads, texture copies queued for a command buffer and their staging bytes.
    pub lazy_meshes: u64,
    pub lazy_materials: u64,
    pub lazy_lights: u64,
    pub lazy_skel: u64,
    pub lazy_particles: u64,
    pub lazy_trees: u64,
    pub late_skips: u64,
    pub preload_requests: u64,
    pub applied: u64,
    pub apply_ms: f32,
    pub apply_max_ms: f32,
    pub upload_bytes: u64,
    pub textures_added: u64,
    pub meshes_added: u64,
    pub preload_pending: u32,
    pub copies_pending: u32,
    pub staging_bytes: u64,
    /// Package scans not finished yet (`n1_preload_package`).
    pub preload_scanning: u32,
    /// Run-time material parameter variants built from a resident base (drawn with the base meanwhile).
    pub variant_materials: u64,
    /// B-228: decals this frame (sent, drawn as receiver pieces), (decal, receiver) meshes cached, this frame's decal
    /// work in ms; meshes built and their triangles since start.
    pub decals: u32,
    pub decal_draws: u32,
    pub decal_cached: u32,
    pub decal_build_ms: f32,
    pub decal_builds: u64,
    pub decal_triangles: u64,
}

/// B-199: one package's worth of things to load, found on a scan thread.
#[derive(Default)]
struct PreloadList {
    package: String,
    meshes: Vec<String>,
    skel: Vec<String>,
    materials: Vec<String>,
    lights: Vec<String>,
    pmaterials: Vec<String>,
    pmeshes: Vec<String>,
    tree_assets: Vec<String>,
    tree_components: Vec<String>,
    /// `lights` lists every lit component the package places (its light map was read whole).
    level: bool,
    scan_ms: f32,
}

fn scan_package(db: &AssetDb, pkg: &str, particles: bool, trees: bool) -> PreloadList {
    let t0 = std::time::Instant::now();
    let mut l = PreloadList { package: pkg.to_string(), ..Default::default() };
    for (path, class) in db.package_objects(pkg) {
        match class.as_str() {
            "StaticMesh" => l.meshes.push(path),
            "SkeletalMesh" => l.skel.push(path),
            "Material" | "MaterialInstanceConstant" | "MaterialInstanceTimeVarying" | "DecalMaterial" => l.materials.push(path),
            "SpeedTree" if trees => l.tree_assets.push(path),
            "ParticleSpriteEmitter" | "ParticleMeshEmitter" if particles => {
                if let Some(e) = db.emitter_info(&path) {
                    if !e.material.is_empty() {
                        l.pmaterials.push(e.material);
                    }
                    if !e.mesh.is_empty() {
                        l.pmeshes.push(e.mesh);
                    }
                }
            }
            _ => {}
        }
    }
    // Level packages: every placed component's baked light, and the trees'.
    l.lights = db.level_light_components(pkg);
    // `level_light_components` loaded the package's whole light map: a component not in it has no baked light.
    l.level = true;
    if trees {
        l.tree_components = db.level_tree_components(pkg);
    }
    l.pmaterials.sort();
    l.pmaterials.dedup();
    l.pmeshes.sort();
    l.pmeshes.dedup();
    l.scan_ms = t0.elapsed().as_secs_f32() * 1000.0;
    l
}

/// Preload bookkeeping kept in `Inner`.
struct Preload {
    seen: std::collections::HashSet<String>,
    tx: std::sync::mpsc::Sender<PreloadList>,
    rx: std::sync::mpsc::Receiver<PreloadList>,
    scanning: Arc<std::sync::atomic::AtomicU32>,
    log: bool,
    /// Render-thread time budget per frame for applying loads in play (`BOTS_N1_UPLOAD_MS`, 0: count only).
    play_budget: std::time::Duration,
}

impl Inner {
    /// Queues what finished scans found.
    fn take_preloads(&mut self) {
        while let Ok(l) = self.preload.rx.try_recv() {
            let mut q = 0usize;
            // Its copies win over other packages' for paths in several (cooked levels carry their own copies).
            self.db.prefer_packages([l.package.clone()]);
            for p in &l.materials {
                q += self.resolver.preload(scenekit::PreloadKind::Material, p) as usize;
            }
            for p in &l.meshes {
                q += self.resolver.preload(scenekit::PreloadKind::Mesh, p) as usize;
            }
            for p in &l.lights {
                q += self.resolver.preload(scenekit::PreloadKind::Light, p) as usize;
            }
            if l.level {
                self.resolver.set_lights_complete(&l.package);
            }
            for p in &l.skel {
                q += self.skins.preload(p) as usize;
            }
            for p in &l.pmaterials {
                q += self.ps.preload_material(p) as usize;
            }
            for p in &l.pmeshes {
                q += self.ps.preload_mesh(p) as usize;
            }
            for p in &l.tree_assets {
                q += self.trees.preload_asset(p) as usize;
            }
            for p in &l.tree_components {
                q += self.trees.preload_component(p) as usize;
            }
            if self.preload.log {
                eprintln!(
                    "n1ffi: preload {}: {} meshes, {} skeletal, {} materials, {} lights, {} particle materials, {} particle meshes, {} trees, {} tree components; {q} new requests; scan {:.0} ms",
                    l.package,
                    l.meshes.len(),
                    l.skel.len(),
                    l.materials.len(),
                    l.lights.len(),
                    l.pmaterials.len(),
                    l.pmeshes.len(),
                    l.tree_assets.len(),
                    l.tree_components.len(),
                    l.scan_ms
                );
            }
        }
    }

    /// Applies finished loads (all kinds) until `deadline`; returns how many.
    fn apply_loads(&mut self, max: usize, deadline: Option<std::time::Instant>) -> usize {
        let t0 = std::time::Instant::now();
        let mut n = self.resolver.pump_until(&mut self.renderer, max, deadline);
        if n < max && !deadline.is_some_and(|d| std::time::Instant::now() >= d) {
            n += self.skins.pump_until(&mut self.renderer, &mut self.resolver, max - n, deadline);
        }
        if n < max && !deadline.is_some_and(|d| std::time::Instant::now() >= d) {
            n += self.ps.pump_until(&mut self.renderer, max - n, deadline);
        }
        self.trees.pump(&mut self.renderer);
        let _ = t0;
        n
    }

    fn preload_pending(&self) -> u32 {
        self.resolver.stats().preload_in_flight as u32 + self.skins.preload_pending() + self.ps.pending() + self.trees.pending_jobs() as u32
    }

    fn fill_load_stats(&self, out: &mut N1Stats) {
        let c = &self.resolver.ctr;
        let rc = &self.renderer.ctr;
        let (copies, staging) = self.renderer.pending_uploads();
        out.lazy_meshes = c.lazy_meshes;
        out.lazy_materials = c.lazy_materials;
        out.variant_materials = c.variant_materials;
        out.lazy_lights = c.lazy_lights;
        out.lazy_skel = self.skins.lazy;
        out.lazy_particles = self.ps.lazy;
        out.lazy_trees = self.trees.lazy;
        out.late_skips = c.late_skips + self.skins.late;
        out.preload_requests = c.preload_requests + self.skins.preloads + self.ps.preloads + self.trees.preloads;
        out.applied = c.uploads;
        out.apply_ms = c.upload_us as f32 / 1e3;
        out.apply_max_ms = c.upload_max_us as f32 / 1e3;
        out.upload_bytes = rc.upload_bytes;
        out.textures_added = rc.textures_added;
        out.meshes_added = rc.meshes_added;
        out.preload_pending = self.preload_pending();
        out.copies_pending = copies;
        out.staging_bytes = staging;
        out.preload_scanning = self.preload.scanning.load(Relaxed);
    }
}

type Grades = Arc<Mutex<std::collections::HashMap<String, Option<lightfeed::post::Grade>>>>;

/// Version of the on-disk grade files; bump it when `Grade::from_params` or `material_params` change what they give.
const GRADE_FILE_VERSION: &str = "g1";

fn grade_file(db: &AssetDb, m: &str) -> std::path::PathBuf {
    let name: String = m.chars().map(|c| if c.is_ascii_alphanumeric() || c == '_' || c == '.' { c } else { '_' }).collect();
    db.cache_dir().join("grade").join(format!("{name}.txt"))
}

fn grade_floats(g: &lightfeed::post::Grade) -> [f32; 14] {
    [
        g.fg_desaturate, g.fg_contrast, g.fg_tint[0], g.fg_tint[1], g.fg_tint[2], g.bk_brightness, g.bk_desaturate,
        g.bk_contrast, g.bk_tint[0], g.bk_tint[1], g.bk_tint[2], g.blend, g.blend_offset, g.sky_clamp,
    ]
}

/// `Some(None)`: the file says "not a GenericOutdoor grade"; `None`: no usable file.
fn read_grade_file(path: &Path, m: &str) -> Option<Option<lightfeed::post::Grade>> {
    let text = std::fs::read_to_string(path).ok()?;
    let mut lines = text.lines();
    if lines.next()? != GRADE_FILE_VERSION || lines.next()? != m {
        return None;
    }
    let body = lines.next()?;
    if body == "none" {
        return Some(None);
    }
    let v: Vec<f32> = body
        .split(' ')
        .map(|h| u32::from_str_radix(h, 16).map(f32::from_bits))
        .collect::<Result<_, _>>()
        .ok()?;
    if v.len() != 14 {
        return None;
    }
    Some(Some(lightfeed::post::Grade {
        fg_desaturate: v[0],
        fg_contrast: v[1],
        fg_tint: [v[2], v[3], v[4]],
        bk_brightness: v[5],
        bk_desaturate: v[6],
        bk_contrast: v[7],
        bk_tint: [v[8], v[9], v[10]],
        blend: v[11],
        blend_offset: v[12],
        sky_clamp: v[13],
    }))
}

fn write_grade_file(path: &Path, m: &str, g: &Option<lightfeed::post::Grade>) {
    let body = match g {
        None => "none".to_string(),
        Some(g) => grade_floats(g).iter().map(|f| format!("{:08x}", f.to_bits())).collect::<Vec<_>>().join(" "),
    };
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let _ = std::fs::write(path, format!("{GRADE_FILE_VERSION}\n{m}\n{body}\n"));
}

/// A plain Material is its own base, and only GenericOutdoor_MAT01 gives a grade (`Grade::from_params`), so no other
/// Material needs its package opened to know it has none.
fn never_a_grade(class: Option<&str>, path: &str) -> bool {
    class == Some("Material") && !path.ends_with("GenericOutdoor_MAT01")
}

/// A MaterialEffect's grade, from the in-memory cache, the cache directory's `grade/` file, or decoded from the
/// packages (which opens and parses one: about 47 ms on the Mac for a level's first; B-248). The decode runs outside the
/// lock, so a second caller for the same path at worst repeats it.
fn grade_of(db: &AssetDb, grades: &Grades, m: &str) -> Option<lightfeed::post::Grade> {
    if let Some(g) = grades.lock().unwrap_or_else(|p| p.into_inner()).get(m) {
        return g.clone();
    }
    // BOTS_N1_GRADE_PREWARM=0 (also the host's switch for the background decode) turns the grade files off too.
    static FILES: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    let files = *FILES.get_or_init(|| std::env::var("BOTS_N1_GRADE_PREWARM").map(|v| v != "0").unwrap_or(true));
    // A plain Material is its own base: only GenericOutdoor_MAT01 itself can give a grade, so every other one is
    // "none" without opening its package.
    if never_a_grade(db.class_of(m), m) {
        grades.lock().unwrap_or_else(|p| p.into_inner()).insert(m.to_string(), None);
        return None;
    }
    let file = grade_file(db, m);
    let g = match files.then(|| read_grade_file(&file, m)).flatten() {
        Some(g) => g,
        None => {
            let g = db.material_params(m).and_then(|(base, sc, ve)| lightfeed::post::Grade::from_params(&base, &sc, &ve));
            if files {
                write_grade_file(&file, m, &g);
            }
            g
        }
    };
    grades.lock().unwrap_or_else(|p| p.into_inner()).insert(m.to_string(), g.clone());
    g
}

/// B-248 (b): resolves the grade of every MaterialInstanceConstant on the disc (the only kind that gives one besides
/// GenericOutdoor_MAT01 itself), so no level's first lighting feed finds its MaterialEffect undecoded: with the grade
/// files written (or read back, on a warm boot) the render thread never opens a package for it. Runs on its own thread
/// beside the startup step's compiles, which wait for it. `BOTS_N1_GRADE_ALL=0` (or `BOTS_N1_GRADE_PREWARM=0`): off.
fn spawn_grade_pass(db: Arc<AssetDb>, grades: Grades) -> Option<std::thread::JoinHandle<()>> {
    let on = |k: &str| std::env::var(k).map(|v| v != "0").unwrap_or(true);
    if !on("BOTS_N1_GRADE_ALL") || !on("BOTS_N1_GRADE_PREWARM") {
        return None;
    }
    std::thread::Builder::new()
        .name("n1-grades".into())
        .spawn(move || {
            let t0 = std::time::Instant::now();
            let all = db.paths_of_classes(&["MaterialInstanceConstant"]);
            let known = |m: &String| grades.lock().unwrap_or_else(|p| p.into_inner()).contains_key(m);
            let todo: Vec<&String> = all.iter().filter(|m| !known(m)).collect();
            let found = todo.iter().filter(|m| grade_of(&db, &grades, m).is_some()).count();
            eprintln!(
                "n1ffi: grades: {} materials resolved ({} grades) in {:.0} ms",
                todo.len(),
                found,
                t0.elapsed().as_secs_f64() * 1e3
            );
        })
        .ok()
}

struct Inner {
    db: Arc<AssetDb>,
    renderer: Renderer,
    resolver: Resolver,
    skins: skin::Skins,
    skel_instances: Vec<skin::SkelInstance>,
    skel_bones: Vec<f32>,
    skel_materials: Vec<u32>,
    instances: Vec<Instance>,
    overrides: Vec<u32>,
    env_paths: Vec<String>,
    env_by_path: std::collections::HashMap<String, u32>,
    env_map: Vec<u32>,
    /// Fog and post from the last feed that carried them (the game sends them less often than lights).
    last_fog: Option<lightfeed::Fog>,
    last_post: Option<lightfeed::PostFeed>,
    /// W4: MaterialEffect material path -> its decoded grade (None: not a GenericOutdoor instance or not found).
    /// Shared with the B-248 prewarm thread (`n1_prewarm_grade`), so it is behind its own lock, not `Inner`'s.
    grades: Grades,
    ps: particles::ParticleSystem,
    trees: trees::TreeSystem,
    decals: decals::Decals,
    /// `n1_record` calls, and run-time material variants released so far (`Resolver::release_idle_variants`).
    record_calls: u64,
    variants_released: u64,
    preload: Preload,
    /// B-266: post-material textures loaded on a background thread (started by the first post feed), uploaded by
    /// `n1_record`.
    post_tex_rx: Option<std::sync::mpsc::Receiver<(String, Arc<assetdb::TextureAsset>)>>,
}

pub struct N1Renderer {
    inner: Mutex<Inner>,
    /// The startup step, once started (kept outside `inner` so progress can be read while it works).
    pre: Mutex<Option<Arc<precompile::Progress>>>,
}

fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

/// # Safety
/// `init` must point to a valid `N1Init` whose handles outlive the renderer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_create(init: *const N1Init) -> *mut N1Renderer {
    let r = std::panic::catch_unwind(|| -> anyhow::Result<Box<N1Renderer>> {
        let init = unsafe { init.as_ref() }.ok_or_else(|| anyhow::anyhow!("null init"))?;
        let cooked = cstr(init.cooked_dir).ok_or_else(|| anyhow::anyhow!("cooked_dir"))?;
        let cache = cstr(init.cache_dir).ok_or_else(|| anyhow::anyhow!("cache_dir"))?;
        let db = Arc::new(AssetDb::open(Path::new(cooked), Path::new(cache))?);
        let ctx = VkContext {
            get_instance_proc_addr: init.get_instance_proc_addr,
            instance: init.instance,
            physical_device: init.physical_device,
            device: init.device,
        };
        let renderer = unsafe { Renderer::new(&ctx, init.color_format, Some(Path::new(cache).to_path_buf()))? };
        let db_ps = db.clone();
        let db_tr = db.clone();
        let particles_on = renderer.knobs.particles;
        let mut resolver = Resolver::new(db.clone());
        if init.loader_threads > 0 {
            resolver.start_loader(init.loader_threads as usize);
        }
        Ok(Box::new(N1Renderer {
            pre: Mutex::new(None),
            inner: Mutex::new(Inner {
                skins: skin::Skins::new(db.clone()),
                skel_instances: Vec::new(),
                skel_bones: Vec::new(),
                skel_materials: Vec::new(),
                db,
                renderer,
                resolver,
                instances: Vec::new(),
                overrides: Vec::new(),
                env_paths: Vec::new(),
                env_by_path: Default::default(),
                env_map: Vec::new(),
                last_fog: None,
                last_post: None,
                grades: Default::default(),
                ps: particles::ParticleSystem::new(db_ps, particles_on),
                trees: trees::TreeSystem::new(db_tr),
                decals: decals::Decals::new(),
                record_calls: 0,
                variants_released: 0,
                post_tex_rx: None,
                preload: {
                    let (tx, rx) = std::sync::mpsc::channel();
                    let ms: f32 = std::env::var("BOTS_N1_UPLOAD_MS").ok().and_then(|v| v.parse().ok()).unwrap_or(2.0);
                    Preload {
                        seen: Default::default(),
                        tx,
                        rx,
                        scanning: Default::default(),
                        log: std::env::var("BOTS_N1_PRELOAD_LOG").map(|v| v != "0").unwrap_or(true),
                        play_budget: std::time::Duration::from_micros((ms.max(0.0) * 1000.0) as u64),
                    }
                },
            }),
        }))
    });
    match r {
        Ok(Ok(b)) => Box::into_raw(b),
        Ok(Err(e)) => {
            set_error(format!("{e:#}"));
            std::ptr::null_mut()
        }
        Err(_) => {
            set_error("panic in n1_create");
            std::ptr::null_mut()
        }
    }
}

/// Starts the startup step: builds every shader and pipeline the disc can ask for on worker threads (from the manifest
/// of the last boot when there is one, else by enumerating the disc's materials). Returns at once. Poll
/// `n1_precompile_progress`. Calling it again does nothing.
///
/// # Safety
/// `r` must be a live renderer from `n1_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_precompile_start(r: *const N1Renderer) -> i32 {
    let Some(rr) = (unsafe { r.as_ref() }) else { return -1 };
    let mut pre = match rr.pre.lock() {
        Ok(g) => g,
        Err(p) => p.into_inner(),
    };
    if pre.is_some() {
        return 0;
    }
    let Some((store, db, features, bc5_ok, grades)) = with(r, None, |i| {
        Some((i.renderer.store.clone(), i.db.clone(), i.renderer.knobs.features, i.renderer.bc5_ok(), i.grades.clone()))
    }) else {
        return -1;
    };
    let cores = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    let threads = std::env::var("BOTS_N1_COMPILE_THREADS")
        .ok()
        .and_then(|v| v.parse().ok())
        // Up to 8 logical CPUs (a 4-core laptop) the startup step gets 2 workers, not 4: on a laptop 4 workers left the
        // game's audio short of CPU (670-1468 underruns over the 85 s cold-cache step, against 288-567 with 2, for a
        // 92-98 s step).
        .unwrap_or(if cores <= 8 { 2 } else { (cores / 2).clamp(2, 8) });
    let pregrade = spawn_grade_pass(db.clone(), grades);
    *pre = Some(precompile::start(store, db, precompile::Opts { features, bc5_ok, threads, pregrade }));
    0
}

/// Progress of the startup step: 0 not started, 1 running, 2 done. `done`/`total` count work items (they restart
/// when the step moves from its first phase to its second); either may be NULL.
///
/// # Safety
/// `r` must be a live renderer; `done` and `total` NULL or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_precompile_progress(r: *const N1Renderer, done: *mut u32, total: *mut u32) -> u32 {
    let Some(rr) = (unsafe { r.as_ref() }) else { return 0 };
    let pre = match rr.pre.lock() {
        Ok(g) => g,
        Err(p) => p.into_inner(),
    };
    let Some(p) = pre.as_ref() else { return 0 };
    if let Some(d) = unsafe { done.as_mut() } {
        *d = p.done.load(Relaxed);
    }
    if let Some(t) = unsafe { total.as_mut() } {
        *t = p.total.load(Relaxed);
    }
    if p.finished.load(Relaxed) { 2 } else { 1 }
}

/// The last error on this thread (empty if none). Valid until the next n1 call on this thread.
#[unsafe(no_mangle)]
pub extern "C" fn n1_last_error() -> *const c_char {
    LAST_ERROR.with(|l| l.borrow().as_ptr())
}

/// # Safety
/// The GPU must be done with everything recorded (the host waited for its submissions).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_destroy(r: *mut N1Renderer) {
    if !r.is_null() {
        drop(unsafe { Box::from_raw(r) });
    }
}

impl Inner {
    /// W4: the MaterialEffect's colour grade, decoded once per material from the packages (both feed paths).
    fn resolve_grade(&mut self, post: &mut lightfeed::PostFeed) {
        let last = self.last_post.as_ref().map(|p| &p.material_effects);
        if last != Some(&post.material_effects) {
            eprintln!("n1ffi: post materials {:?}", post.material_effects);
        }
        // B-266: the ported post materials' textures, loaded once in the background (a few small textures) so the
        // first Gangsta Fire doesn't wait on a package open.
        if post.present && self.post_tex_rx.is_none() && self.renderer.knobs.post_mats {
            let (tx, rx) = std::sync::mpsc::channel();
            let db = self.db.clone();
            let _ = std::thread::Builder::new().name("n1-postmat".into()).spawn(move || {
                for t in postmat::all_textures() {
                    match db.texture(t) {
                        Some(a) => {
                            let _ = tx.send((t.to_string(), a));
                        }
                        None => eprintln!("n1ffi: post material texture {t} not found"),
                    }
                }
            });
            self.post_tex_rx = Some(rx);
        }
        let db = &self.db;
        let grades = &self.grades;
        post.grade = post.material_effects.iter().find_map(|m| grade_of(db, grades, m));
    }
}

/// B-248: decodes the colour grades of these MaterialEffect paths on a background thread, so the first lighting feed
/// that names them (`n1_set_feed_bin`, on the render thread) finds them cached instead of taking ~47 ms to open the
/// package. Paths already cached cost nothing; returns the number started (0 or 1 threads), -1 on a bad renderer.
///
/// # Safety
/// `paths` points to `count` valid NUL-terminated strings (or `count` is 0).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_prewarm_grade(r: *const N1Renderer, paths: *const *const c_char, count: u32) -> i32 {
    if paths.is_null() || count == 0 {
        return 0;
    }
    let names: Vec<String> = unsafe { std::slice::from_raw_parts(paths, count as usize) }
        .iter()
        .filter_map(|&p| cstr(p).map(str::to_string))
        .collect();
    let Some((db, grades)) = with(r, None, |i| Some((i.db.clone(), i.grades.clone()))) else {
        return -1;
    };
    let todo: Vec<String> = {
        let g = grades.lock().unwrap_or_else(|p| p.into_inner());
        names.into_iter().filter(|m| !g.contains_key(m)).collect()
    };
    if todo.is_empty() {
        return 0;
    }
    let spawned = std::thread::Builder::new().name("n1-grade".into()).spawn(move || {
        for m in &todo {
            let _ = grade_of(&db, &grades, m);
        }
    });
    i32::from(spawned.is_ok())
}

fn with<T>(r: *const N1Renderer, default: T, f: impl FnOnce(&mut Inner) -> T) -> T {
    let Some(r) = (unsafe { r.as_ref() }) else {
        return default;
    };
    let mut g = match r.inner.lock() {
        Ok(g) => g,
        Err(p) => p.into_inner(),
    };
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| f(&mut g))) {
        Ok(v) => v,
        Err(_) => {
            set_error("panic");
            default
        }
    }
}

/// Handle for a StaticMesh path (`Package.Group.Name`). Never fails; a path that doesn't resolve is simply never drawn.
#[unsafe(no_mangle)]
pub extern "C" fn n1_mesh(r: *const N1Renderer, path: *const c_char) -> u32 {
    let Some(p) = cstr(path) else {
        return scenekit::NONE;
    };
    with(r, scenekit::NONE, |i| i.resolver.mesh_handle(p))
}

/// Handle for a material path; "" or NULL gives N1_NONE.
#[unsafe(no_mangle)]
pub extern "C" fn n1_material(r: *const N1Renderer, path: *const c_char) -> u32 {
    let Some(p) = cstr(path) else {
        return scenekit::NONE;
    };
    with(r, scenekit::NONE, |i| i.resolver.material_handle(p))
}

/// Handle for a placed component's baked vertex light, by the component's full path
/// (`Level.TheWorld.PersistentLevel.Actor.Component`). The level package it names is also preferred from then on
/// when a mesh or material path exists in several packages (cooked levels carry their own copies).
#[unsafe(no_mangle)]
pub extern "C" fn n1_light(r: *const N1Renderer, component: *const c_char) -> u32 {
    let Some(p) = cstr(component) else {
        return scenekit::NONE;
    };
    with(r, scenekit::NONE, |i| {
        if let Some(level) = p.split('.').next() {
            i.db.prefer_packages([level.to_string()]);
        }
        i.resolver.light_handle(p)
    })
}

/// Handle for a SkeletalMesh path. Never fails; one that doesn't resolve is never drawn.
#[unsafe(no_mangle)]
pub extern "C" fn n1_skel_mesh(r: *const N1Renderer, path: *const c_char) -> u32 {
    let Some(p) = cstr(path) else {
        return scenekit::NONE;
    };
    with(r, scenekit::NONE, |i| i.skins.handle(p))
}

pub type N1SkelInstance = skin::SkelInstance;

/// The skinned primitives for the next `n1_record` (copied; used once). `bones`: `bone_floats` floats, SpaceBases
/// matrices (16 floats each, UE3 row-major; NaN = not set by the game) that instances index; `materials`: handles.
///
/// # Safety
/// Pointers valid for the given counts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_set_skeletal(
    r: *const N1Renderer,
    instances: *const N1SkelInstance,
    count: u32,
    bones: *const f32,
    bone_floats: u32,
    materials: *const u32,
    material_count: u32,
) {
    let slice = |p, n: u32| {
        if n == 0 || p == std::ptr::null() {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(p, n as usize) }
        }
    };
    let inst: &[N1SkelInstance] = if count == 0 || instances.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(instances, count as usize) }
    };
    let b: &[f32] = slice(bones, bone_floats);
    let m: &[u32] = if material_count == 0 || materials.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(materials, material_count as usize) }
    };
    with(r, (), |i| {
        i.skel_instances.clear();
        i.skel_instances.extend_from_slice(inst);
        i.skel_bones.clear();
        i.skel_bones.extend_from_slice(b);
        i.skel_materials.clear();
        i.skel_materials.extend_from_slice(m);
    })
}

/// Handle for a particle material by path (the emitter's material or material instance). Stable, no I/O; the shader
/// is built from the package's expression graph on a loader thread, and the emitter is skipped until it is ready.
#[unsafe(no_mangle)]
pub extern "C" fn n1_pmaterial(r: *const N1Renderer, path: *const c_char) -> u32 {
    let Some(p) = cstr(path) else {
        return scenekit::NONE;
    };
    with(r, scenekit::NONE, |i| i.ps.material_handle(p))
}

/// Handle for a mesh-particle mesh by StaticMesh path. Stable, no I/O.
#[unsafe(no_mangle)]
pub extern "C" fn n1_pmesh(r: *const N1Renderer, path: *const c_char) -> u32 {
    let Some(p) = cstr(path) else {
        return scenekit::NONE;
    };
    with(r, scenekit::NONE, |i| i.ps.mesh_handle(p))
}

pub type N1Emitter = particles::N1Emitter;

/// The next `n1_record`'s particle emitters (copied; used once). `rows`: `row_floats` floats per particle, emitters
/// index into it by `row_offset` (in floats).
///
/// # Safety
/// Pointers valid for the given counts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_set_particles(
    r: *const N1Renderer,
    emitters: *const N1Emitter,
    count: u32,
    rows: *const f32,
    row_floats_total: u32,
) {
    let e: &[N1Emitter] = if count == 0 || emitters.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(emitters, count as usize) }
    };
    let f: &[f32] = if row_floats_total == 0 || rows.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(rows, row_floats_total as usize) }
    };
    with(r, (), |i| i.ps.set_frame(e, f))
}

// ---------------------------------------------------------------------------------------------------- B-187 trees

pub type N1Tree = trees::N1Tree;
pub type N1TreeDraw = trees::N1TreeDraw;

/// Handle for a SpeedTreeComponent by its full path (`<package>.<component path>`, the scene feed's primitive name):
/// its baked vertex light loads on a loader thread. Stable, no I/O.
#[unsafe(no_mangle)]
pub extern "C" fn n1_tree_component(r: *const N1Renderer, path: *const c_char) -> u32 {
    let Some(p) = cstr(path) else {
        return scenekit::NONE;
    };
    with(r, scenekit::NONE, |i| i.trees.component_handle(p))
}

/// Handle for a SpeedTree asset by its object path (`AST_FoliageTree.Shrub04`): its geometry (cached, or captured from
/// the game's own draws) and materials. Stable, no I/O.
#[unsafe(no_mangle)]
pub extern "C" fn n1_tree_asset(r: *const N1Renderer, path: *const c_char) -> u32 {
    let Some(p) = cstr(path) else {
        return scenekit::NONE;
    };
    with(r, scenekit::NONE, |i| i.trees.asset_handle(p))
}

/// B-228: the next `n1_record`'s decals (copied; used once). `receivers`: indices into that call's instances.
///
/// # Safety
/// `decals` valid for `count` entries, `receivers` for `receiver_count`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_set_decals(
    r: *const N1Renderer,
    decals: *const decals::N1Decal,
    count: u32,
    receivers: *const u32,
    receiver_count: u32,
) {
    let d: &[decals::N1Decal] = if count == 0 || decals.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(decals, count as usize) }
    };
    let rv: &[u32] = if receiver_count == 0 || receivers.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(receivers, receiver_count as usize) }
    };
    with(r, (), |i| i.decals.set_frame(d, rv))
}

/// The next `n1_record`'s tree placements (copied; used once).
///
/// # Safety
/// `trees` valid for `count` entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_set_trees(r: *const N1Renderer, trees: *const N1Tree, count: u32) {
    let t: &[N1Tree] = if count == 0 || trees.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(trees, count as usize) }
    };
    with(r, (), |i| i.trees.set_frame(t))
}

/// Capture hook, step 1, for each colour-pass draw while a capture is active: bit 0 of the result asks the host to
/// call `n1_tree_capture` for this draw, bit 1 to call `n1_tree_ucode` first. `consts`: 256 vec4 vertex constants.
///
/// # Safety
/// `consts` valid for 1024 floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_tree_probe(r: *const N1Renderer, vs_hash: u64, consts: *const f32, index_count: u32) -> u32 {
    if consts.is_null() {
        return 0;
    }
    let c = unsafe { std::slice::from_raw_parts(consts, 1024) };
    with(r, 0, |i| i.trees.probe(vs_hash, c, index_count))
}

/// The disassembly text of a vertex shader `n1_tree_probe` asked for.
#[unsafe(no_mangle)]
pub extern "C" fn n1_tree_ucode(r: *const N1Renderer, vs_hash: u64, text: *const c_char) {
    if let Some(t) = cstr(text) {
        with(r, (), |i| i.trees.set_ucode(vs_hash, t))
    }
}

/// Capture hook, step 2: the draw's inputs; the game's vertex shader runs over them on the CPU.
///
/// # Safety
/// Every pointer in `draw` valid for its stated size, for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_tree_capture(r: *const N1Renderer, draw: *const N1TreeDraw) {
    if let Some(d) = unsafe { draw.as_ref() } {
        with(r, (), |i| unsafe { i.trees.capture(d) })
    }
}

/// Once per rendered frame: advances the capture session. Returns the number of frames the host should let the game
/// draw its own scene for (non-zero when a session starts: the geometry of trees without a cache file is captured from
/// those frames). The cache is generated on this machine from the player's disc and never shipped.
#[unsafe(no_mangle)]
pub extern "C" fn n1_tree_end_frame(r: *const N1Renderer) -> u32 {
    with(r, 0, |i| i.trees.end_frame())
}

/// Kept for ABI compatibility: ambient now comes from the feed (`n1_set_feed`) or `BOTS_N1_ENVP=ambk=..`.
#[unsafe(no_mangle)]
pub extern "C" fn n1_set_ambient(r: *const N1Renderer, ambient: f32) {
    let _ = (r, ambient);
}

/// Debug A/B switches that take effect on the next frame, without a new feed (paired shots of one held frame).
/// Bit 0 (B-230): level dynamic lights use the plain `LightColor / 255 * Brightness` colour instead of UE3's
/// `pow(LightColor / 255, 2.2) * Brightness` (what `BOTS_N1_LIGHT_GAMMA=0` does for a whole run). Bit 8 (256, B-245):
/// light environments in the other colour mode (`BOTS_N1_ENV_GAMMA`). Bit 9 (512, B-245): light-environment key
/// specular back to Blinn (`BOTS_N1_ENV_SPEC=0`). Bit 11 (2048, B-249): the level's sun, fill and sky light colours in the
/// other colour mode (`BOTS_N1_SKY_GAMMA`). Bit 12 (4096, B-261): world-space ReflectionVector (`BOTS_N1_REFL_TS=0`). 0: none.
#[unsafe(no_mangle)]
pub extern "C" fn n1_set_debug_bits(r: *const N1Renderer, bits: u32) {
    with(r, (), |i| i.renderer.debug_bits = bits);
}

/// Handle for a DynamicLightEnvironmentComponent by its full path (the `LightEnvironment` value the feed's lights
/// carry). Stable, no I/O; put it in `N1Instance::env` / `N1SkelInstance::env`.
#[unsafe(no_mangle)]
pub extern "C" fn n1_env(r: *const N1Renderer, component: *const c_char) -> u32 {
    let Some(p) = cstr(component) else {
        return scenekit::NONE;
    };
    with(r, scenekit::NONE, |i| {
        if let Some(&h) = i.env_by_path.get(p) {
            return h;
        }
        let h = i.env_paths.len() as u32;
        i.env_paths.push(p.to_string());
        i.env_by_path.insert(p.to_string(), h);
        h
    })
}

/// The game's world time in seconds (UE3 ViewFamily CurrentWorldTime), for the materials' Time expressions and the
/// particles'. Materials compare it with times the game wrote (e.g. a tracer's EffectStartTime), so it must be the
/// game's clock; until the host first calls this, n1 uses seconds since it started.
///
/// # Safety
/// `r` from `n1_create` (or null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_set_time(r: *const N1Renderer, seconds: f32) {
    with(r, (), |i| i.renderer.world_time = Some(seconds));
}

/// B-240: the view's screen fade (camera and cinematic fades) for the next frames, as the port's composite applies it:
/// out = pow(saturate(c * scale), 1 / 2.2) + overlay, with scale = ColorScale * (1 - OverlayColor.a) (the port's
/// composite constant c5.rgb) and overlay = OverlayColor.rgb * OverlayColor.a. Null pointers reset to no fade.
///
/// # Safety
/// `scale` and `overlay` null or valid for 3 floats each.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_set_fade(r: *const N1Renderer, scale: *const f32, overlay: *const f32) {
    let fade = if scale.is_null() || overlay.is_null() {
        [[1.0; 3], [0.0; 3]]
    } else {
        let (s, o) = unsafe { (std::slice::from_raw_parts(scale, 3), std::slice::from_raw_parts(overlay, 3)) };
        let f = |x: f32, d: f32| if x.is_finite() { x } else { d };
        [[f(s[0], 1.0), f(s[1], 1.0), f(s[2], 1.0)], [f(o[0], 0.0), f(o[1], 0.0), f(o[2], 0.0)]]
    };
    with(r, (), |i| i.renderer.fade = fade);
}

/// The frame's lights, fog and post chain as the scene feed's JSON (`{"lights":[..],"fog":[..],"post":{..}}`, the
/// format `lightfeed` parses). Sun, sky, fog, post and dynamic lights follow it until the next call. Returns 0, or -1
/// with `n1_last_error` set.
///
/// # Safety
/// `json` valid for `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_set_feed(r: *const N1Renderer, json: *const c_char, len: u32) -> i32 {
    if json.is_null() {
        return -1;
    }
    let bytes = unsafe { std::slice::from_raw_parts(json as *const u8, len as usize) };
    // BOTS_N1_FEED_DUMP=<dir>: keep every 256th feed as <dir>/feed_<n>.json (debugging).
    if let Ok(dir) = std::env::var("BOTS_N1_FEED_DUMP") {
        static N: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let n = N.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        if n % 256 == 0 {
            let _ = std::fs::write(format!("{dir}/feed_{n:06}.json"), bytes);
        }
    }
    with(r, -1, |i| {
        let v: serde_json::Value = match serde_json::from_slice(bytes) {
            Ok(v) => v,
            Err(e) => {
                set_error(format!("feed json: {e}"));
                return -1;
            }
        };
        match lightfeed::Frame::from_value(&v) {
            Ok(mut fr) => {
                // Absent "fog"/"post" keys mean unchanged: keep the last ones.
                if v.get("fog").is_some() {
                    i.last_fog = fr.fog.clone();
                } else {
                    fr.fog = i.last_fog.clone();
                }
                if v.get("post").is_some() {
                    i.resolve_grade(&mut fr.post);
                    i.last_post = Some(fr.post.clone());
                } else if let Some(p) = &i.last_post {
                    fr.post = p.clone();
                }
                let k = i.renderer.knobs;
                i.renderer.lighting.set_feed(&fr, &k);
                0
            }
            Err(e) => {
                set_error(format!("{e:#}"));
                -1
            }
        }
    })
}

pub type N1FeedBin = lightfeed::bin::FeedBin;

/// B-188: the frame's lights, fog and post as fixed-layout binary records (`N1FeedBin` in n1ffi.h, no JSON). Same
/// effect as `n1_set_feed`: sun, sky, fog, post and dynamic lights follow it until the next call; a feed without
/// `N1_FEED_FOG_POST` keeps the last fog and post. Returns 0, or -1 with `n1_last_error` set.
///
/// # Safety
/// `feed` and the pointers inside it valid for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_set_feed_bin(r: *const N1Renderer, feed: *const N1FeedBin) -> i32 {
    let Some(b) = (unsafe { feed.as_ref() }) else {
        set_error("null feed");
        return -1;
    };
    with(r, -1, |i| match unsafe { lightfeed::bin::decode(b) } {
        Ok(d) => {
            let mut fr = d.frame;
            if d.has_fog_post {
                i.last_fog = fr.fog.clone();
                // B-227: the binary feed names the MaterialEffect too; without this the level's grade was never
                // applied on binary-feed hosts (the default), e.g. Crib's blue screen tint in the shadows.
                i.resolve_grade(&mut fr.post);
                i.last_post = Some(fr.post.clone());
            } else {
                fr.fog = i.last_fog.clone();
                if let Some(p) = &i.last_post {
                    fr.post = p.clone();
                }
            }
            let k = i.renderer.knobs;
            i.renderer.lighting.set_feed(&fr, &k);
            0
        }
        Err(e) => {
            set_error(e);
            -1
        }
    })
}

/// Debugging: parse the JSON feed and the binary feed built from the same game state and compare what the renderer
/// would see (numbers within 1e-5, as the JSON has 7 digits). Returns the number of differences (the first ones are in
/// `n1_last_error`), or -1 if either cannot be read. Changes no renderer state.
///
/// # Safety
/// `json` valid for `len` bytes; `feed` and its pointers valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_check_feed(json: *const c_char, len: u32, feed: *const N1FeedBin) -> i32 {
    let (Some(b), false) = (unsafe { feed.as_ref() }, json.is_null()) else {
        set_error("null feed");
        return -1;
    };
    let bytes = unsafe { std::slice::from_raw_parts(json as *const u8, len as usize) };
    let jf = match lightfeed::Frame::from_json(&String::from_utf8_lossy(bytes)) {
        Ok(f) => f,
        Err(e) => {
            set_error(format!("{e:#}"));
            return -1;
        }
    };
    match unsafe { lightfeed::bin::decode(b) } {
        Ok(d) => {
            let diffs = lightfeed::bin::diff_frames(&jf, &d.frame, 1e-5);
            if !diffs.is_empty() {
                if std::env::var_os("BOTS_N1_FEED_VERIFY_DUMP").is_some() {
                    eprintln!("feed verify json: {:?}\nfeed verify bin:  {:?}", jf.post, d.frame.post);
                }
                set_error(diffs.join("; "));
            }
            diffs.len() as i32
        }
        Err(e) => {
            set_error(e);
            -1
        }
    }
}

// ------------------------------------------------------------------------------------------- B-199 level preload

/// Starts loading everything a cooked package holds (call it when the game opens `<name>.xxx`: a map, a streamed
/// sublevel, a shared package): static and skeletal meshes, materials, particle materials and meshes, SpeedTrees, and
/// for a level package every placed component's baked light. A scan thread lists them; the loads go to the loader
/// threads, and `n1_upload` / `n1_record` apply them. Each package once. Returns 1 when a scan started, 0 when the
/// package was seen before or isn't on the disc.
#[unsafe(no_mangle)]
pub extern "C" fn n1_preload_package(r: *const N1Renderer, name: *const c_char) -> i32 {
    let Some(name) = cstr(name) else {
        return 0;
    };
    let pkg = name.rsplit(['/', '\\']).next().unwrap_or(name);
    let pkg = pkg.strip_suffix(".xxx").or_else(|| pkg.strip_suffix(".XXX")).unwrap_or(pkg).to_string();
    with(r, 0, |i| {
        if !i.db.has_package(&pkg) || !i.preload.seen.insert(pkg.to_ascii_lowercase()) {
            return 0;
        }
        let db = i.db.clone();
        let tx = i.preload.tx.clone();
        let scanning = i.preload.scanning.clone();
        let (particles, trees) = (i.ps.enabled, i.trees.enabled);
        scanning.fetch_add(1, Relaxed);
        let spawned = std::thread::Builder::new().name("n1-preload-scan".into()).spawn(move || {
            let l = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| scan_package(&db, &pkg, particles, trees)));
            if let Ok(l) = l {
                let _ = tx.send(l);
            }
            scanning.fetch_sub(1, Relaxed);
        });
        if spawned.is_err() {
            i.preload.scanning.fetch_sub(1, Relaxed);
            return 0;
        }
        1
    })
}

/// Applies finished loads for up to `budget_us` microseconds and records their texture copies into `cmd`, for frames
/// without a scene (loading screen, the preload hold). Same `serial` / `completed` rules as `n1_record` (a frame may
/// call both with one serial). Fills the load counters of `stats` (the other fields are left alone). Returns the loads
/// applied, or -1 on error.
///
/// # Safety
/// `cmd` in the recording state outside any render pass, on the device given to `n1_create`; `stats` NULL or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_upload(
    r: *const N1Renderer,
    cmd: vk::CommandBuffer,
    serial: u64,
    completed: u64,
    budget_us: u32,
    stats: *mut N1Stats,
) -> i32 {
    with(r, -1, |i| {
        i.take_preloads();
        let deadline = std::time::Instant::now() + std::time::Duration::from_micros(budget_us as u64);
        let n = i.apply_loads(usize::MAX, Some(deadline));
        let res = i.renderer.upload(cmd, serial, completed);
        if let Some(out) = unsafe { stats.as_mut() } {
            i.fill_load_stats(out);
        }
        match res {
            Ok(_) => n as i32,
            Err(e) => {
                set_error(format!("{e:#}"));
                -1
            }
        }
    })
}

/// Reads the load counters into `stats` without recording anything (the other fields are left alone).
///
/// # Safety
/// `stats` valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_load_stats(r: *const N1Renderer, stats: *mut N1Stats) {
    let Some(out) = (unsafe { stats.as_mut() }) else {
        return;
    };
    with(r, (), |i| {
        i.take_preloads();
        i.fill_load_stats(out)
    })
}

/// Records the frame into `cmd` (outside any render pass or dynamic rendering). `serial`: the host's number for the
/// submission `cmd` will be part of, increasing; `completed`: the newest serial whose submission has finished.
/// Uploads at most `upload_budget` loaded assets first. Returns 0 on success, -1 on error (`n1_last_error`).
///
/// # Safety
/// All pointers valid for the given counts; `cmd` in the recording state on the device given to `n1_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn n1_record(
    r: *const N1Renderer,
    cmd: vk::CommandBuffer,
    target: *const N1Target,
    camera: *const N1Camera,
    instances: *const N1Instance,
    count: u32,
    materials: *const u32,
    material_count: u32,
    serial: u64,
    completed: u64,
    upload_budget: u32,
    stats: *mut N1Stats,
) -> i32 {
    let (Some(t), Some(c)) = (unsafe { target.as_ref() }, unsafe { camera.as_ref() }) else {
        set_error("null target or camera");
        return -1;
    };
    let inst: &[Instance] = if count == 0 || instances.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(instances, count as usize) }
    };
    let mats: &[u32] = if material_count == 0 || materials.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(materials, material_count as usize) }
    };
    with(r, -1, |i| {
        let t_all = std::time::Instant::now();
        let mut lp = vkr::Laps::new();
        lp.begin();
        i.take_preloads();
        if let Some(rx) = &i.post_tex_rx {
            while let Ok((name, t)) = rx.try_recv() {
                if let Err(e) = i.renderer.post_tex_add(&name, &t) {
                    eprintln!("n1ffi: post material texture {name}: {e:#}");
                }
            }
        }
        lp.lap("take_preloads");
        // B-199: a count cap (`upload_budget`) and a time cap (`BOTS_N1_UPLOAD_MS`) on loads applied per frame.
        let deadline = (!i.preload.play_budget.is_zero()).then(|| std::time::Instant::now() + i.preload.play_budget);
        let uploads = i.resolver.pump_until(&mut i.renderer, upload_budget as usize, deadline);
        lp.lap("pump_until");
        let skel_uploads = i.skins.pump_until(
            &mut i.renderer,
            &mut i.resolver,
            upload_budget.max(1) as usize,
            deadline,
        );
        lp.lap("skins_pump");
        let (ti, to) = i.resolver.translate(&mut i.renderer, inst, mats);
        i.instances.clear();
        i.instances.extend_from_slice(ti);
        i.overrides.clear();
        i.overrides.extend_from_slice(to);
        let translated = i.instances.len() as u32;
        // B-228: decals, as extra instances (clipped receiver geometry with the decal's material).
        lp.lap("translate");
        i.decals.append(&mut i.renderer, &mut i.resolver, &i.db, inst, &mut i.instances, &mut i.overrides);
        lp.lap("decals");
        // Run-time material variants (one per bullet tracer) nothing has named for ~5 s give their GPU material back.
        i.record_calls += 1;
        if i.record_calls % 64 == 0 {
            let n = i.resolver.release_idle_variants(&mut i.renderer, 300) as u64;
            if n > 0 && (i.variants_released + n) / 256 != i.variants_released / 256 {
                eprintln!(
                    "n1ffi: {} run-time material variants released so far ({} renderer materials)",
                    i.variants_released + n,
                    i.renderer.materials.len()
                );
            }
            i.variants_released += n;
        }
        lp.lap("release_variants");
        let vp = scenekit::view_proj(
            &c.view,
            &c.proj,
            if c.near_plane > 0.0 {
                c.near_plane
            } else {
                5.0
            },
        );
        let target = Target {
            view: t.view,
            layout: t.layout,
            width: t.width,
            height: t.height,
            rect: vk::Rect2D {
                offset: vk::Offset2D { x: t.x, y: t.y },
                extent: vk::Extent2D {
                    width: t.w,
                    height: t.h,
                },
            },
            clear: (t.clear != 0).then_some(t.clear_color),
        };
        i.env_map.clear();
        for p in &i.env_paths {
            let e = i.renderer.lighting.env_index.get(p).map(|&x| x as u32);
            i.env_map.push(e.unwrap_or(scenekit::NONE));
        }
        i.skins.build(
            &mut i.renderer,
            &mut i.resolver,
            &vp,
            &i.skel_instances,
            &i.skel_bones,
            &i.skel_materials,
            &i.env_map,
        );
        lp.lap("skins_build");
        // B-186: particles. Material and mesh loads finish on the loader thread; a few are taken per frame.
        let near = if c.near_plane > 0.0 { c.near_plane } else { 5.0 };
        let mut pf = i.ps.prepare(&mut i.renderer, &vp, c.location, 2);
        pf.near = near;
        lp.lap("ps_prepare");
        i.renderer.pframe = pf;
        i.renderer.tframe = i.trees.prepare(&mut i.renderer, &vp, c.location);
        lp.lap("trees_prepare");
        let tree_stats = i.trees.last;
        i.renderer.near = near;
        let skel_count = i.skel_instances.len() as u32;
        i.skel_instances.clear();
        let res = i.renderer.record(
            cmd,
            &target,
            &vp,
            c.location,
            &i.instances,
            &i.overrides,
            &i.skins.frame,
            &i.env_map,
            serial,
            completed,
        );
        lp.lap("renderer_record");
        match res {
            Ok(st) => {
                precompile::maybe_save(&i.renderer.store);
                lp.lap("maybe_save");
                lp.report(
                    "n1_record",
                    t_all.elapsed().as_secs_f32() * 1000.0,
                    &format!("(inst {count} record_ms {:.1})", st.record_ms),
                );
                if let Some(out) = unsafe { stats.as_mut() } {
                    let rs = i.resolver.stats();
                    let (_, _, textures, _, shaders, pipelines) = i.renderer.counts();
                    let lt = &i.renderer.lighting;
                    let sc = &i.renderer.store.ctr;
                    *out = N1Stats {
                        instances: count,
                        culled: st.culled,
                        draws: st.draws,
                        skipped: count - translated,
                        triangles: st.triangles,
                        record_ms: st.record_ms,
                        uploads: (uploads + skel_uploads) as u32 + st.uploads,
                        skel_drawn: i.skins.frame.insts.len() as u32,
                        skel_culled: i.skins.culled,
                        skel_skipped: skel_count
                            - i.skins.frame.insts.len() as u32
                            - i.skins.culled,
                        skel_meshes_ready: i.skins.counts().0,
                        meshes_ready: rs.meshes_ready as u32,
                        meshes_pending: rs.meshes_pending as u32,
                        meshes_missing: rs.meshes_missing as u32,
                        loading: rs.in_flight as u32 + i.skins.counts().1,
                        textures: textures as u32,
                        textures_unsupported: i.renderer.unsupported_textures,
                        shaders: shaders as u32,
                        pipelines: pipelines as u32,
                        pipelines_pending: i.renderer.pipe_pending,
                        dyn_lights: lt.dyn_frame.lights.len() as u32,
                        light_envs: lt.dyn_frame.envs.len() as u32,
                        feed_frames: lt.frames as u32,
                        env_bound: i.renderer.env_bound(),
                        env_handles: i.env_paths.len() as u32,
                        particle_emitters: i.ps.last.emitters,
                        particle_sprites: i.ps.last.sprites,
                        particle_meshes: i.ps.last.mesh_instances,
                        particle_dropped: i.ps.last.dropped_sprites + i.ps.last.dropped_meshes + i.ps.last.bad_rows,
                        particle_mats_ready: i.ps.last.mats_ready,
                        particle_mats_failed: i.ps.last.mats_failed,
                        particle_skipped: i.ps.last.skipped_emitters,
                        tree_placements: tree_stats.placements,
                        tree_instances: tree_stats.instances,
                        tree_draws: tree_stats.draws,
                        tree_assets_ready: tree_stats.assets_ready,
                        tree_assets_uncaptured: tree_stats.assets_uncaptured,
                        tree_assets_failed: tree_stats.assets_failed,
                        tree_wanted: tree_stats.wanted,
                        tree_captured: i.trees.captured,
                        shaders_startup: sc.shaders_startup.load(Relaxed),
                        shaders_live: sc.shaders_live.load(Relaxed),
                        pipelines_startup: sc.pipes_startup.load(Relaxed),
                        pipelines_live: sc.pipes_live.load(Relaxed),
                        late_shaders: sc.late_shaders.load(Relaxed),
                        late_pipelines: sc.late_pipes.load(Relaxed),
                        build_failures: sc.failed.load(Relaxed),
                        spv_hits: sc.spv_hits.load(Relaxed),
                        spv_misses: sc.spv_misses.load(Relaxed),
                        pipeline_ms: sc.pipe_us.load(Relaxed) as f32 / 1e3,
                        pipeline_max_ms: sc.pipe_max_us.load(Relaxed) as f32 / 1e3,
                        naga_ms: sc.naga_us.load(Relaxed) as f32 / 1e3,
                        compile_backlog: i.renderer.store.backlog() as u32,
                        partial_frames: i.renderer.ctr.partial_frames,
                        draws_no_pipe: i.renderer.ctr.draws_no_pipe,
                        fallback_draws: i.renderer.ctr.fallback_draws,
                        decals: i.decals.stats.decals,
                        decal_draws: i.decals.stats.drawn,
                        decal_cached: i.decals.stats.cached,
                        decal_build_ms: i.decals.stats.build_ms,
                        decal_builds: i.decals.stats.builds,
                        decal_triangles: i.decals.stats.triangles,
                        ..Default::default()
                    };
                    i.fill_load_stats(out);
                }
                0
            }
            Err(e) => {
                set_error(format!("{e:#}"));
                -1
            }
        }
    })
}

#[cfg(test)]
mod grade_file_tests {
    use super::*;

    fn sample() -> lightfeed::post::Grade {
        lightfeed::post::Grade {
            fg_desaturate: 0.1,
            fg_contrast: -0.25,
            fg_tint: [0.01, 0.02, 0.03],
            bk_brightness: 1.1,
            bk_desaturate: 0.3,
            bk_contrast: 0.2,
            bk_tint: [0.9, 0.95, 1.0],
            blend: 1e-4,
            blend_offset: -0.05,
            sky_clamp: 2.0,
        }
    }

    #[test]
    fn plain_materials_are_never_grades() {
        assert!(never_a_grade(Some("Material"), "FX.PostFX_v03"));
        assert!(!never_a_grade(Some("Material"), "Pkg.GenericOutdoor_MAT01"));
        assert!(!never_a_grade(Some("MaterialInstanceConstant"), "Pkg.Crib_MIC"), "an instance has to be decoded");
        assert!(!never_a_grade(None, "Pkg.Unknown"), "an unindexed path takes the old route");
    }

    /// B-248: a grade read back from its cache file is bit-for-bit the one written, and "none" survives too.
    #[test]
    fn grade_file_round_trips() {
        let dir = std::env::temp_dir().join(format!("n1-grade-test-{}", std::process::id()));
        let file = dir.join("grade").join("a.txt");
        write_grade_file(&file, "Pkg.Mat", &Some(sample()));
        assert_eq!(read_grade_file(&file, "Pkg.Mat"), Some(Some(sample())));
        assert_eq!(read_grade_file(&file, "Other.Mat"), None, "a file for another path is not used");
        write_grade_file(&file, "Pkg.Mat", &None);
        assert_eq!(read_grade_file(&file, "Pkg.Mat"), Some(None));
        std::fs::write(&file, "g0\nPkg.Mat\nnone\n").unwrap();
        assert_eq!(read_grade_file(&file, "Pkg.Mat"), None, "another version is not used");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
