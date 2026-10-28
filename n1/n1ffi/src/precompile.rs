//! W2: the startup step. Builds every shader module and pipeline the disc can ask for before the first level, so play
//! never compiles anything (what's missing is drawn with a stand-in and reported).
//!
//! Two sources, tried in this order:
//! 1. **The manifest** (`manifest.txt` in the n1 GPU cache): what the last boot built, by persistent key. Reading it
//!    touches no package; each entry is a SPIR-V file read, a module and a pipeline that the driver's cache
//!    (`pipeline_cache.bin`) already holds. This is the warm path.
//! 2. **Enumeration** from the asset index: every Material/MaterialInstance on the disc planned through the same code
//!    the live path uses (`plan`), the skinned variants of the materials skeletal meshes name, every particle material
//!    the emitters name, and the fixed fallbacks. This is the cold path (first boot, new disc, or code changed).
//!
//! Either way the work is a list of jobs on the store's worker pool; `Progress` counts them for the progress UI.

use crate::particles;
use crate::plan::{self, fnv};
use crate::store::{Origin, PartKey, SceneKey, Snapshot, Store};
use crate::vkr::blend_of;
use assetdb::AssetDb;
use std::collections::{BTreeSet, HashMap};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering::Relaxed};
use std::sync::{Arc, Mutex};
use std::time::Instant;

#[derive(Default)]
pub struct Progress {
    pub done: AtomicU32,
    pub total: AtomicU32,
    pub finished: AtomicBool,
    /// Wall time of the whole step, ms, once finished.
    pub ms: AtomicU32,
    pub phase: Mutex<String>,
}

pub struct Opts {
    /// B-248 (b): the grade pass (`spawn_grade_pass`); the step is not done before it is.
    pub pregrade: Option<std::thread::JoinHandle<()>>,
    pub features: u32,
    pub bc5_ok: bool,
    pub threads: usize,
}

/// The pieces a manifest depends on; any change makes it stale.
fn header(db: &AssetDb, o: &Opts) -> String {
    // The templates and codegen that turn a material into WGSL: editing any of them changes shader identities.
    let code = fnv(include_str!("../../matdef/src/codegen.rs").as_bytes())
        ^ fnv(include_str!("../../matdef/src/shader.rs").as_bytes()).rotate_left(1)
        ^ fnv(include_str!("shaders.rs").as_bytes()).rotate_left(2)
        ^ fnv(include_str!("plan.rs").as_bytes()).rotate_left(3)
        // The WGSL templates the scene shaders are made from (B-189: a scene.wgsl edit kept the old SPIR-V).
        ^ fnv(matdef::shader::SCENE_WGSL.as_bytes()).rotate_left(4)
        ^ fnv(matdef::shader::ENVLIGHT_WGSL.as_bytes()).rotate_left(5)
        // The material IR builder: its node shapes are the shader keys (B-203).
        ^ fnv(include_str!("../../ue3/src/matir.rs").as_bytes()).rotate_left(6)
        // The particle material compiler and its templates: their SPIR-V hashes are the particle pipeline keys (B-229).
        ^ fnv(include_str!("../../scenefmt/src/particles/matprog.rs").as_bytes()).rotate_left(7)
        ^ fnv(include_str!("../../scenefmt/src/particles/shader.rs").as_bytes()).rotate_left(8);
    // B-235: the PWL switches change which shaders the materials ask for.
    format!(
        "v1 index={:016x} features={} bc5={} pwl={}{} code={code:016x}",
        db.index_stamp(),
        o.features,
        o.bc5_ok as u8,
        scenekit::scene_pwl() as u8,
        scenekit::lm_linear() as u8
    )
}

fn manifest_text(h: &str, sn: &Snapshot) -> String {
    let mut s = format!("H {h}\n");
    for (k, spv, ntex) in &sn.scene_mods {
        s += &format!("S {k:016x} {spv:016x} {ntex}\n");
    }
    for (k, b, skin) in &sn.scene_pipes {
        s += &format!("s {k:016x} {b} {}\n", *skin as u8);
    }
    for k in &sn.part_mods {
        s += &format!("M {k:016x}\n");
    }
    for (k, b, two, v) in &sn.part_pipes {
        s += &format!("q {k:016x} {b} {} {v}\n", *two as u8);
    }
    s
}

fn parse_manifest(text: &str, h: &str) -> Option<Snapshot> {
    let mut lines = text.lines();
    if lines.next()?.strip_prefix("H ")? != h {
        return None;
    }
    let mut sn = Snapshot::default();
    let hx = |s: &str| u64::from_str_radix(s, 16).ok();
    for l in lines {
        let f: Vec<&str> = l.split(' ').collect();
        match f.as_slice() {
            ["S", k, spv, ntex] => sn.scene_mods.push((hx(k)?, hx(spv)?, ntex.parse().ok()?)),
            ["s", k, b, skin] => sn.scene_pipes.push((hx(k)?, b.parse().ok()?, *skin == "1")),
            ["M", k] => sn.part_mods.push(hx(k)?),
            ["q", k, b, two, v] => sn.part_pipes.push((hx(k)?, b.parse().ok()?, *two == "1", v.parse().ok()?)),
            _ => return None,
        }
    }
    Some(sn)
}

/// Writes the manifest of everything built so far (atomically). `h`: the header it was started with.
pub fn save_manifest(store: &Store, h: &str) {
    // One writer at a time: the startup thread, the background save and `flush` share the temporary file name.
    static ONE: Mutex<()> = Mutex::new(());
    let Some(d) = store.dir() else { return };
    let _one = ONE.lock().unwrap_or_else(|p| p.into_inner());
    let text = manifest_text(h, &store.snapshot());
    let f = d.join("manifest.txt");
    let tmp = f.with_extension(format!("tmp{}", std::process::id()));
    if std::fs::write(&tmp, text).is_ok() {
        let _ = std::fs::rename(&tmp, &f);
    }
}

/// What to build: scene shaders (with the pipelines each needs), then particle modules likewise.
#[derive(Default)]
struct Jobs {
    scene: HashMap<u64, SceneJob>,
    part: HashMap<u64, PartJob>,
}

struct SceneJob {
    src: SceneSrc,
    pipes: BTreeSet<(u8, bool)>,
}

enum SceneSrc {
    Plan(Box<plan::Plan>),
    Cached { spv: u64, ntex: usize },
}

struct PartJob {
    words: Option<Vec<u32>>,
    pipes: BTreeSet<(u8, bool, u8)>,
}

impl Jobs {
    fn units(&self) -> u32 {
        let n = |p: usize| p as u32 + 1;
        self.scene.values().map(|j| n(j.pipes.len())).sum::<u32>() + self.part.values().map(|j| n(j.pipes.len())).sum::<u32>()
    }

    fn from_snapshot(sn: &Snapshot) -> Jobs {
        let mut j = Jobs::default();
        for &(k, spv, ntex) in &sn.scene_mods {
            j.scene.insert(k, SceneJob { src: SceneSrc::Cached { spv, ntex }, pipes: BTreeSet::new() });
        }
        for &(k, b, skin) in &sn.scene_pipes {
            if let Some(s) = j.scene.get_mut(&k) {
                s.pipes.insert((b, skin));
            }
        }
        for &k in &sn.part_mods {
            j.part.insert(k, PartJob { words: None, pipes: BTreeSet::new() });
        }
        for &(k, b, two, v) in &sn.part_pipes {
            if let Some(p) = j.part.get_mut(&k) {
                p.pipes.insert((b, two, v));
            }
        }
        j
    }
}

/// Every shader and pipeline the disc can ask for, from the asset index.
fn enumerate(db: &AssetDb, o: &Opts, prog: &Progress) -> Jobs {
    let set = |p: &str| *prog.phase.lock().unwrap() = p.to_string();
    let mut jobs = Jobs::default();
    set("materials");
    let bar = |d: usize, t: usize| {
        prog.total.store(t as u32, Relaxed);
        prog.done.store(d as u32, Relaxed);
    };
    let plans = plan::enumerate(db, o.features, o.bc5_ok, o.threads, &bar);
    // Materials that skeletal meshes name get the skinned pipelines too.
    set("skeletal meshes");
    let mut skin_mats: BTreeSet<String> = BTreeSet::new();
    let sk_paths = db.paths_of_classes(&["SkeletalMesh"]);
    for (i, sp) in sk_paths.iter().enumerate() {
        if let Some(m) = db.skeletal(sp) {
            skin_mats.extend(m.materials.iter().filter(|x| !x.is_empty()).cloned());
        }
        bar(i + 1, sk_paths.len());
    }
    for (path, p) in plans {
        let b = blend_of(p.blend);
        let job = jobs.scene.entry(p.key).or_insert_with(|| SceneJob { src: SceneSrc::Plan(Box::new(plan_clone(&p))), pipes: BTreeSet::new() });
        job.pipes.insert((b, false));
        if skin_mats.contains(&path) {
            job.pipes.insert((b, true));
        }
    }
    // The grey stand-in (a material with no graph and no texture), opaque, static and skinned.
    {
        let g = plan::plan(o.features, &assetdb::MaterialAsset::default(), false, &|_| Default::default());
        let job = jobs.scene.entry(g.key).or_insert_with(|| SceneJob { src: SceneSrc::Plan(Box::new(plan_clone(&g))), pipes: BTreeSet::new() });
        job.pipes.insert((0, false));
        job.pipes.insert((0, true));
    }
    // Particle materials, found through the emitters that name them.
    set("particles");
    let emitters = db.paths_of_classes(&["ParticleSpriteEmitter", "ParticleMeshEmitter"]);
    let mut mats: HashMap<String, (bool, bool, bool)> = HashMap::new(); // sprite, velocity, mesh
    for (i, ep) in emitters.iter().enumerate() {
        if let Some(e) = db.emitter_info(ep)
            && !e.material.is_empty()
        {
            let m = mats.entry(e.material.clone()).or_default();
            if e.type_data.contains("Mesh") || !e.mesh.is_empty() {
                m.2 = true;
            } else if e.screen_alignment.contains("Velocity") {
                m.1 = true;
            } else {
                m.0 = true;
            }
        }
        bar(i + 1, emitters.len());
    }
    set("particle shaders");
    let mut mats: Vec<(String, (bool, bool, bool))> = mats.into_iter().collect();
    mats.sort();
    for (i, (path, (sprite, velocity, mesh))) in mats.iter().enumerate() {
        if let Ok(sh) = particles::material_shaders(db, path) {
            let b = blend_of(sh.blend);
            let sk = fnv(bytemuck::cast_slice(&sh.sprite_spv));
            let mk = fnv(bytemuck::cast_slice(&sh.mesh_spv));
            let sj = jobs.part.entry(sk).or_insert_with(|| PartJob { words: Some(sh.sprite_spv.clone()), pipes: BTreeSet::new() });
            // Sprites never cull, so their key's two_sided is always true (see `Renderer::part_key`).
            if *sprite {
                sj.pipes.insert((b, true, 0));
            }
            if *velocity {
                sj.pipes.insert((b, true, 1));
            }
            let mj = jobs.part.entry(mk).or_insert_with(|| PartJob { words: Some(sh.mesh_spv.clone()), pipes: BTreeSet::new() });
            if *mesh {
                mj.pipes.insert((b, sh.two_sided, 2));
            }
        }
        bar(i + 1, mats.len());
    }
    jobs
}

fn plan_clone(p: &plan::Plan) -> plan::Plan {
    plan::Plan { c: p.c.clone(), flags: p.flags, blend: p.blend, two_sided: p.two_sided, key: p.key }
}

/// Queues `jobs` on the store's workers and waits for them. Returns how many cached entries couldn't be built.
fn run(store: &Arc<Store>, jobs: Jobs, prog: &Arc<Progress>) -> u32 {
    prog.done.store(0, Relaxed);
    prog.total.store(jobs.units(), Relaxed);
    let missing = Arc::new(AtomicU32::new(0));
    let dyn_cull = store.dyn_cull;
    for (skey, job) in jobs.scene {
        let (prog, missing) = (prog.clone(), missing.clone());
        store.spawn(
            false,
            Box::new(move |s| {
                let id = match &job.src {
                    SceneSrc::Plan(p) => s.build_scene_shader(skey, &p.c, p.flags, Origin::Startup),
                    SceneSrc::Cached { spv, ntex } => s.build_scene_shader_cached(skey, *spv, *ntex, Origin::Startup),
                };
                prog.done.fetch_add(1, Relaxed);
                for (blend, skin) in job.pipes {
                    if let Some(shader) = id {
                        // Without dynamic cull state every combination is its own pipeline.
                        let culls: &[u8] = if dyn_cull { &[0] } else { &[0, 1, 2, 3] };
                        for &cull in culls {
                            s.build_scene_pipe(SceneKey { shader, blend, skin, cull }, Origin::Startup);
                        }
                    }
                    prog.done.fetch_add(1, Relaxed);
                }
                if id.is_none() {
                    missing.fetch_add(1, Relaxed);
                }
            }),
        );
    }
    for (key, job) in jobs.part {
        let (prog, missing) = (prog.clone(), missing.clone());
        store.spawn(
            false,
            Box::new(move |s| {
                let id = match &job.words {
                    Some(w) => s.module_from_spv(key, w, Origin::Startup),
                    None => s.build_particle_module_cached(key, Origin::Startup),
                };
                prog.done.fetch_add(1, Relaxed);
                for (blend, two_sided, variant) in job.pipes {
                    if let Some(module) = id {
                        s.build_part_pipe(PartKey { module, blend, two_sided, variant }, Origin::Startup);
                    }
                    prog.done.fetch_add(1, Relaxed);
                }
                if id.is_none() {
                    missing.fetch_add(1, Relaxed);
                }
            }),
        );
    }
    while prog.done.load(Relaxed) < prog.total.load(Relaxed) {
        // The renderer is going away (B-223): its workers are stopped and the queued jobs dropped.
        if store.stopping() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
    missing.load(Relaxed)
}

/// Starts the startup step on its own thread; the returned `Progress` is for the progress UI. The step is over when
/// `finished` is set (and `store.startup_done`).
pub fn start(store: Arc<Store>, db: Arc<AssetDb>, mut o: Opts) -> Arc<Progress> {
    let prog = Arc::new(Progress::default());
    let p = prog.clone();
    std::thread::Builder::new()
        .name("n1-precompile".into())
        .spawn(move || {
            let t0 = Instant::now();
            let h = header(&db, &o);
            let set = |s: &str| *p.phase.lock().unwrap() = s.to_string();
            // B-248 (b): the compile workers start first and the persisted cache loads on its own thread beside them. On
            // MoltenVK the load recompiles every cached library: 211.7 s for a 62 MB cache with a cold OS shader cache
            // (int-2f7d-s8), and the workers used to wait for all of it. Off by default until a truly cold load has been
            // tested: if MoltenVK's slow part is the merge itself, holding `cache_use` for writing would block pipeline
            // creation for that long. BOTS_N1_LOAD_OVERLAP=1 turns it on.
            let overlap = std::env::var("BOTS_N1_LOAD_OVERLAP").is_ok_and(|v| v != "0");
            let loader = overlap.then(|| {
                let s = store.clone();
                let before = s.ctr.pipes_startup.load(Relaxed);
                std::thread::Builder::new()
                    .name("n1-cache-load".into())
                    .spawn(move || {
                        let r = s.load_persisted_cache();
                        (r, s.ctr.pipes_startup.load(Relaxed) - before)
                    })
                    .expect("spawn n1 cache load")
            });
            let (bytes, secs) = if overlap {
                // A warm load takes well under a second and the manifest's pipelines then come out of the cache it
                // fills, so give it a moment to finish alone; only a slow one (cold OS shader cache) is overlapped.
                let wait_ms: u64 = std::env::var("BOTS_N1_LOAD_WAIT_MS").ok().and_then(|v| v.parse().ok()).unwrap_or(2000);
                let t = Instant::now();
                while loader.as_ref().is_some_and(|h| !h.is_finished()) && t.elapsed().as_millis() < wait_ms as u128 {
                    std::thread::sleep(std::time::Duration::from_millis(5));
                }
                store.start_workers(o.threads.max(1));
                (0, 0.0)
            } else {
                store.load_persisted_cache()
            };
            // B-223: the host may destroy the renderer while this thread runs (hybrid dual mode remakes it at startup).
            // The store's Vulkan calls are safe after that (see `Store::gate`); here we just stop early.
            let gone = || {
                if store.stopping() {
                    eprintln!("n1ffi: precompile abandoned: the renderer was destroyed");
                }
                store.stopping()
            };
            if gone() {
                return;
            }
            if !overlap {
                store.start_workers(o.threads.max(1));
            }
            let mut from_manifest = false;
            let mut missing = 0;
            if let Some(d) = store.dir()
                && std::env::var("BOTS_N1_MANIFEST").map(|v| v != "0").unwrap_or(true)
                && let Ok(text) = std::fs::read_to_string(d.join("manifest.txt"))
                && let Some(sn) = parse_manifest(&text, &h)
            {
                set("compiling");
                missing = run(&store, Jobs::from_snapshot(&sn), &p);
                from_manifest = true;
            }
            if !from_manifest || missing > 0 {
                if missing > 0 {
                    eprintln!("n1ffi: precompile: {missing} manifest entries missing from the cache; enumerating");
                }
                if gone() {
                    return;
                }
                let jobs = enumerate(&db, &o, &p);
                set("compiling");
                run(&store, jobs, &p);
            }
            let (bytes, secs) = match loader.map(|h| h.join()) {
                Some(Ok(((b, s), built))) => {
                    eprintln!("n1ffi: pipeline cache load overlapped with the compiles: {built} pipelines built during the load");
                    (b, s)
                }
                _ => (bytes, secs),
            };
            if let Some(h) = o.pregrade.take() {
                set("grades");
                let _ = h.join();
            }
            if gone() {
                return;
            }
            store.startup_done.store(true, Relaxed);
            save_manifest(&store, &h);
            store.save_cache();
            store.set_manifest_header(h);
            p.ms.store(t0.elapsed().as_millis() as u32, Relaxed);
            set("done");
            p.finished.store(true, Relaxed);
            let c = &store.ctr;
            eprintln!("n1ffi: pipeline cache: {:.1} MB loaded in {secs:.1} s", bytes as f64 / 1e6);
            eprintln!(
                "n1ffi: precompile done in {:.1} s ({}): {} shaders, {} pipelines ({} spv cache hits, {} naga builds, {:.1} s naga, {:.1} s in the driver, worst pipeline {:.0} ms), {} failed",
                t0.elapsed().as_secs_f64(),
                if from_manifest { "manifest" } else { "enumerated" },
                c.shaders_startup.load(Relaxed),
                c.pipes_startup.load(Relaxed),
                c.spv_hits.load(Relaxed),
                c.spv_misses.load(Relaxed),
                c.naga_us.load(Relaxed) as f64 / 1e6,
                c.pipe_us.load(Relaxed) as f64 / 1e6,
                c.pipe_max_us.load(Relaxed) as f64 / 1e3,
                c.failed.load(Relaxed),
            );
        })
        .expect("spawn n1 precompile");
    prog
}

/// A background save (`maybe_save`) is running.
static SAVING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// From the render thread, every so often: persists what play had to build after the startup step, so the next boot's
/// manifest has it. The write runs on its own thread (B-199): on a laptop, the driver's pipeline-cache read plus the
/// two file writes took 20-35 ms of the render thread every 10 s while play still built pipelines.
pub fn maybe_save(store: &Arc<Store>) {
    if !store.startup_done.load(Relaxed) {
        return;
    }
    let c = &store.ctr;
    let built = c.pipes_live.load(Relaxed) + c.shaders_live.load(Relaxed);
    if built == store.manifest_saved.load(Relaxed) || store.last_save.lock().unwrap().elapsed().as_secs() < 10 {
        return;
    }
    store.manifest_saved.store(built, Relaxed);
    *store.last_save.lock().unwrap() = Instant::now();
    let h = store.manifest_header.lock().unwrap().clone();
    if h.is_empty() || SAVING.swap(true, Relaxed) {
        return;
    }
    let st = store.clone();
    let spawned = std::thread::Builder::new().name("n1 manifest save".into()).spawn(move || {
        save_manifest(&st, &h);
        st.save_cache();
        SAVING.store(false, Relaxed);
    });
    if spawned.is_err() {
        SAVING.store(false, Relaxed);
    }
}

/// Persists the manifest and the driver's pipeline cache now (renderer shutdown).
pub fn flush(store: &Store) {
    if !store.startup_done.load(Relaxed) {
        return;
    }
    // Let a background save finish first (both write the same temporary files).
    for _ in 0..200 {
        if !SAVING.load(Relaxed) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
    let h = store.manifest_header.lock().unwrap().clone();
    if !h.is_empty() {
        save_manifest(store, &h);
    }
    store.save_cache();
}
