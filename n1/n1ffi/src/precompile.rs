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
    /// Workers during the step; `play_threads` of them stay active after it (B-248).
    pub threads: usize,
    pub play_threads: usize,
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

/// A manifest entry: the shader's stored SPIR-V and the pipelines to build from it.
struct SceneJob {
    spv: u64,
    ntex: usize,
    pipes: BTreeSet<(u8, bool)>,
}

struct PartJob {
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
            j.scene.insert(
                k,
                SceneJob {
                    spv,
                    ntex,
                    pipes: BTreeSet::new(),
                },
            );
        }
        for &(k, b, skin) in &sn.scene_pipes {
            if let Some(s) = j.scene.get_mut(&k) {
                s.pipes.insert((b, skin));
            }
        }
        for &k in &sn.part_mods {
            j.part.insert(
                k,
                PartJob {
                    pipes: BTreeSet::new(),
                },
            );
        }
        for &(k, b, two, v) in &sn.part_pipes {
            if let Some(p) = j.part.get_mut(&k) {
                p.pipes.insert((b, two, v));
            }
        }
        j
    }
}

struct SceneEntry {
    plan: plan::Plan,
    pipes: Mutex<BTreeSet<(u8, bool)>>,
    /// The shader's progress unit and failure are counted once, by whichever of its jobs gets there first.
    counted: AtomicBool,
}

struct PartEntry {
    words: Vec<u32>,
    pipes: Mutex<BTreeSet<(u8, bool, u8)>>,
    counted: AtomicBool,
}

/// B-248: the cold path, streamed. The same jobs as `Jobs` (every shader and pipeline the disc can ask for), but each
/// is queued the moment its material is planned, so the compile workers run while the enumeration is still reading
/// packages (it used to finish first: 95 of 294 s on the T480 with a cold driver cache, workers idle).
struct Stream {
    store: Arc<Store>,
    prog: Arc<Progress>,
    missing: Arc<AtomicU32>,
    /// Units queued so far; the bar's total is the larger of this and `est` until the enumeration is done.
    queued: AtomicU32,
    est: u32,
    scene: Mutex<HashMap<u64, Arc<SceneEntry>>>,
    part: Mutex<HashMap<u64, Arc<PartEntry>>>,
    /// `BOTS_N1_STREAM=0`: hold the jobs back until the enumeration is done, as before B-248.
    held_back: Option<Mutex<Vec<crate::store::Task>>>,
}

impl Stream {
    fn queue(&self, t: crate::store::Task) {
        match &self.held_back {
            Some(h) => h.lock().unwrap().push(t),
            None => self.store.spawn(false, t),
        }
    }

    fn release(&self) {
        if let Some(h) = &self.held_back {
            for t in h.lock().unwrap().drain(..) {
                self.store.spawn(false, t);
            }
        }
    }

    fn add_units(&self, n: u32) {
        let q = self.queued.fetch_add(n, Relaxed) + n;
        self.prog.total.store(q.max(self.est), Relaxed);
    }

    /// Queues the pipeline `(blend, skin)` of the scene shader `key` (and the shader itself, the first time).
    fn want_scene(&self, key: u64, p: &plan::Plan, blend: u8, skin: bool) {
        let (e, fresh) = {
            let mut m = self.scene.lock().unwrap();
            match m.get(&key) {
                Some(e) => (e.clone(), false),
                None => {
                    let e = Arc::new(SceneEntry {
                        plan: plan_clone(p),
                        pipes: Mutex::new(BTreeSet::new()),
                        counted: AtomicBool::new(false),
                    });
                    m.insert(key, e.clone());
                    (e, true)
                }
            }
        };
        if fresh {
            self.add_units(1);
        }
        if !e.pipes.lock().unwrap().insert((blend, skin)) {
            return;
        }
        self.add_units(1);
        let (prog, missing) = (self.prog.clone(), self.missing.clone());
        self.queue(Box::new(move |s| {
            let id = s.build_scene_shader(key, &e.plan.c, e.plan.flags, Origin::Startup);
            if !e.counted.swap(true, Relaxed) {
                prog.done.fetch_add(1, Relaxed);
                if id.is_none() {
                    missing.fetch_add(1, Relaxed);
                }
            }
            if let Some(shader) = id {
                // Without dynamic cull state every combination is its own pipeline.
                let culls: &[u8] = if s.dyn_cull { &[0] } else { &[0, 1, 2, 3] };
                for &cull in culls {
                    s.build_scene_pipe(
                        SceneKey {
                            shader,
                            blend,
                            skin,
                            cull,
                        },
                        Origin::Startup,
                    );
                }
            }
            prog.done.fetch_add(1, Relaxed);
        }));
    }

    /// Queues a particle module (the first time) and, when given, one of its pipelines.
    fn want_part(&self, key: u64, words: &[u32], pipe: Option<(u8, bool, u8)>) {
        let (e, fresh) = {
            let mut m = self.part.lock().unwrap();
            match m.get(&key) {
                Some(e) => (e.clone(), false),
                None => {
                    let e = Arc::new(PartEntry {
                        words: words.to_vec(),
                        pipes: Mutex::new(BTreeSet::new()),
                        counted: AtomicBool::new(false),
                    });
                    m.insert(key, e.clone());
                    (e, true)
                }
            }
        };
        let new_pipe = pipe.is_some_and(|p| e.pipes.lock().unwrap().insert(p));
        if !fresh && !new_pipe {
            return;
        }
        self.add_units(fresh as u32 + new_pipe as u32);
        let (prog, missing) = (self.prog.clone(), self.missing.clone());
        self.queue(Box::new(move |s| {
            let id = s.module_from_spv(key, &e.words, Origin::Startup);
            if !e.counted.swap(true, Relaxed) {
                prog.done.fetch_add(1, Relaxed);
                if id.is_none() {
                    missing.fetch_add(1, Relaxed);
                }
            }
            if let (Some((blend, two_sided, variant)), true) = (pipe, new_pipe) {
                if let Some(module) = id {
                    s.build_part_pipe(
                        PartKey {
                            module,
                            blend,
                            two_sided,
                            variant,
                        },
                        Origin::Startup,
                    );
                }
                // Counted even when the module failed, or the step would wait for it forever.
                prog.done.fetch_add(1, Relaxed);
            }
        }));
    }
}

/// The particle materials the emitters name, their shaders and pipelines, queued as found.
fn enumerate_particles(db: &AssetDb, st: &Stream) {
    let emitters = db.paths_of_classes(&["ParticleSpriteEmitter", "ParticleMeshEmitter"]);
    let mut mats: HashMap<String, (bool, bool, bool)> = HashMap::new(); // sprite, velocity, mesh
    for ep in &emitters {
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
    }
    let mut mats: Vec<(String, (bool, bool, bool))> = mats.into_iter().collect();
    mats.sort();
    for (path, (sprite, velocity, mesh)) in &mats {
        if let Ok(sh) = particles::material_shaders(db, path) {
            let b = blend_of(sh.blend);
            let sk = fnv(bytemuck::cast_slice(&sh.sprite_spv));
            let mk = fnv(bytemuck::cast_slice(&sh.mesh_spv));
            // Sprites never cull, so their key's two_sided is always true (see `Renderer::part_key`).
            st.want_part(sk, &sh.sprite_spv, None);
            if *sprite {
                st.want_part(sk, &sh.sprite_spv, Some((b, true, 0)));
            }
            if *velocity {
                st.want_part(sk, &sh.sprite_spv, Some((b, true, 1)));
            }
            st.want_part(mk, &sh.mesh_spv, None);
            if *mesh {
                st.want_part(mk, &sh.mesh_spv, Some((b, sh.two_sided, 2)));
            }
        }
    }
}

/// Every shader and pipeline the disc can ask for, from the asset index, queued as it is found (see `Stream`). Waits
/// for the workers to finish them. Phases, in order: materials (the bulk), skeletal meshes (which materials need the
/// skinned pipelines), the grey stand-in, particle materials.
fn enumerate_and_build(store: &Arc<Store>, db: &AssetDb, o: &Opts, prog: &Arc<Progress>) {
    let t0 = Instant::now();
    let set = |p: &str| {
        eprintln!(
            "n1ffi: enumerate phase '{p}' at +{:.1} s",
            t0.elapsed().as_secs_f64()
        );
        *prog.phase.lock().unwrap() = p.to_string();
    };
    let bar = |d: usize, t: usize| {
        // Until the queue is complete the bar shows the materials pass; `Stream::add_units` takes over the total.
        let _ = (d, t);
    };
    prog.done.store(0, Relaxed);
    // About 1.1 units per material: measured 2,152 materials -> 2,370 units.
    let n_paths = db
        .paths_of_classes(&[
            "Material",
            "MaterialInstanceConstant",
            "MaterialInstanceTimeVarying",
            "DecalMaterial",
        ])
        .len();
    let st = Stream {
        store: store.clone(),
        prog: prog.clone(),
        missing: Arc::new(AtomicU32::new(0)),
        queued: AtomicU32::new(0),
        est: (n_paths as f32 * 1.1) as u32,
        scene: Mutex::new(HashMap::new()),
        part: Mutex::new(HashMap::new()),
        held_back: std::env::var("BOTS_N1_STREAM")
            .is_ok_and(|v| v == "0")
            .then(|| Mutex::new(Vec::new())),
    };
    prog.total.store(st.est, Relaxed);
    set("materials");
    let seen: Mutex<Vec<(String, u64, u8)>> = Mutex::new(Vec::new());
    let skin_mats: Mutex<BTreeSet<String>> = Mutex::new(BTreeSet::new());
    // The materials pass is the bulk. Beside it, on their own threads: the skeletal meshes (which materials need the
    // skinned pipelines) and the particle materials (found through the emitters), each queued as found.
    let side = |name: &str| {
        let t = Instant::now();
        let r = std::thread::scope(|sc| {
            let h1 = sc.spawn(|| {
                for sp in db.paths_of_classes(&["SkeletalMesh"]) {
                    if let Some(m) = db.skeletal(&sp) {
                        skin_mats
                            .lock()
                            .unwrap()
                            .extend(m.materials.iter().filter(|x| !x.is_empty()).cloned());
                    }
                }
            });
            let h2 = sc.spawn(|| enumerate_particles(db, &st));
            plan::enumerate(db, o.features, o.bc5_ok, o.threads, &bar, &|path, p| {
                let b = blend_of(p.blend);
                st.want_scene(p.key, p, b, false);
                seen.lock().unwrap().push((path.to_string(), p.key, b));
            });
            eprintln!(
                "n1ffi: enumerate: materials done at +{:.1} s",
                t.elapsed().as_secs_f64()
            );
            let _ = h1.join();
            let _ = h2.join();
        });
        eprintln!(
            "n1ffi: enumerate: {name} done at +{:.1} s",
            t.elapsed().as_secs_f64()
        );
        r
    };
    // Only while the game is held (`threads` above `play_threads`): with the game running behind the step the CPU is
    // not free (the reason the step uses 2 workers there), so the passes run one after the other. BOTS_N1_ENUM_SERIAL=1
    // forces that.
    let serial =
        o.threads <= o.play_threads || std::env::var("BOTS_N1_ENUM_SERIAL").is_ok_and(|v| v != "0");
    if serial {
        plan::enumerate(db, o.features, o.bc5_ok, o.threads, &bar, &|path, p| {
            let b = blend_of(p.blend);
            st.want_scene(p.key, p, b, false);
            seen.lock().unwrap().push((path.to_string(), p.key, b));
        });
        set("skeletal meshes");
        for sp in db.paths_of_classes(&["SkeletalMesh"]) {
            if let Some(m) = db.skeletal(&sp) {
                skin_mats
                    .lock()
                    .unwrap()
                    .extend(m.materials.iter().filter(|x| !x.is_empty()).cloned());
            }
        }
        set("particles");
        enumerate_particles(db, &st);
    } else {
        side("all passes");
    }
    // Materials that skeletal meshes name get the skinned pipelines too.
    let skin_mats = skin_mats.into_inner().unwrap();
    for (path, key, b) in seen.into_inner().unwrap() {
        if skin_mats.contains(&path) {
            let e = st.scene.lock().unwrap().get(&key).cloned();
            if let Some(e) = e {
                st.want_scene(key, &e.plan, b, true);
            }
        }
    }
    // The grey stand-in (a material with no graph and no texture), opaque, static and skinned.
    {
        let g = plan::plan(
            o.features,
            &assetdb::MaterialAsset::default(),
            false,
            &|_| Default::default(),
        );
        st.want_scene(g.key, &g, 0, false);
        st.want_scene(g.key, &g, 0, true);
    }
    set("compiling");
    // The queue is complete: the bar's total is exact from here.
    prog.total.store(st.queued.load(Relaxed), Relaxed);
    st.release();
    wait_done(store, prog);
    let missing = st.missing.load(Relaxed);
    if missing > 0 {
        eprintln!("n1ffi: precompile: {missing} shaders failed to build");
    }
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
                let id = s.build_scene_shader_cached(skey, job.spv, job.ntex, Origin::Startup);
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
                let id = s.build_particle_module_cached(key, Origin::Startup);
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
    wait_done(store, prog);
    missing.load(Relaxed)
}

/// Waits until every queued unit is done.
fn wait_done(store: &Arc<Store>, prog: &Arc<Progress>) {
    while prog.done.load(Relaxed) < prog.total.load(Relaxed) {
        // The renderer is going away (B-223): its workers are stopped and the queued jobs dropped.
        if store.stopping() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
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
            // B-248: each phase's start time, so a slow first launch says where it went.
            let set = |s: &str| {
                eprintln!("n1ffi: precompile phase '{s}' at {:.1} s", t0.elapsed().as_secs_f64());
                *p.phase.lock().unwrap() = s.to_string();
            };
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
                enumerate_and_build(&store, &db, &o, &p);
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
                set("grades done");
            }
            if gone() {
                return;
            }
            store.startup_done.store(true, Relaxed);
            store.set_max_active(o.play_threads.max(1));
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
