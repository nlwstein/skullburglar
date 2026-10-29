//! W2: the thread-safe store of shader modules and pipelines, and the workers that fill it.
//!
//! Before W2 the render thread compiled every shader (naga) and pipeline the moment a draw needed one, a few per
//! frame, and skipped the draws that had to wait. Now:
//! - **Everything that takes time runs on worker threads.** The render thread only looks things up (`pipe_lookup`) and,
//!   when something is missing, asks for it (`want_*`) and draws a fallback this frame.
//! - **Pipeline keys are small.** Cull mode and front face are dynamic state (Vulkan 1.3 core), so a scene pipeline is
//!   (shader, blend state, skinned). Two-sided and mirrored draws share a pipeline with their one-sided twins.
//! - **SPIR-V is cached on disk** by the hash of the WGSL it came from, and **the driver's `VkPipelineCache` is
//!   persisted**, so a warm start neither runs naga nor asks the driver to recompile anything it has seen.
//! - The startup step (`precompile.rs`) enumerates every shader and pipeline the disc can ask for and builds them here
//!   before the first level; whatever still turns up missing in play is counted (`late_*`) and logged.

use crate::shaders;
use anyhow::{Result, anyhow};
use ash::vk;
use matdef::codegen::Compiled;
use matdef::shader::ShaderFlags;
use std::collections::{HashMap, VecDeque};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering::Relaxed};
use std::sync::{Arc, Condvar, Mutex};

pub use crate::vkr::{DEPTH_FORMAT, HDR_FORMAT};
/// Bump when anything that changes SPIR-V without changing the WGSL text (naga upgrade, writer options) changes.
const SPV_REV: &str = "naga30-r1";
pub const NO_SHADER: u32 = u32::MAX;

/// A scene pipeline: cull mode and front face are dynamic where the device allows, so two-sided and mirrored draws
/// share it.
#[derive(Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Debug)]
pub struct SceneKey {
    pub shader: u32,
    /// `vkr::blend_of`: 0 opaque, 1 masked, 2 alpha, 3 additive, 4 modulate.
    pub blend: u8,
    pub skin: bool,
    /// Static cull state when the device can't switch it per draw: bit 0 two-sided, bit 1 mirrored. Always 0 with
    /// dynamic cull state.
    pub cull: u8,
}

/// A particle pipeline: sprite or mesh module, its blend, and the vertex layout (variant 0 sprite, 1 velocity, 2 mesh).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct PartKey {
    pub module: u32,
    pub blend: u8,
    pub two_sided: bool,
    pub variant: u8,
}

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
enum AnyKey {
    Scene(SceneKey),
    Part(PartKey),
}

#[derive(Clone, Copy)]
enum PState {
    Building,
    Ready(vk::Pipeline),
    Failed,
}

#[derive(Clone, Copy)]
enum MState {
    Building,
    Ready(u32),
    Failed,
}

pub struct ModEntry {
    pub module: vk::ShaderModule,
    pub ntex: usize,
    /// How to find it again: scene modules by shader identity (`skey`), particle modules by the hash of their words.
    pub key: u64,
    /// The SPIR-V's name in the disk cache: the hash of the WGSL (scene) or of the words (particles).
    pub spv: u64,
    pub particle: bool,
}

/// Who asked: the startup step or the render thread in play.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Origin {
    Startup,
    Live,
}

#[derive(Default)]
struct Inner {
    layouts: HashMap<usize, (vk::DescriptorSetLayout, vk::PipelineLayout)>,
    mods: Vec<ModEntry>,
    by_key: HashMap<u64, MState>,
    pipes: HashMap<AnyKey, PState>,
}

/// Counters for the report: everything is cumulative since the store was made.
#[derive(Default)]
pub struct Counters {
    pub shaders_startup: AtomicU32,
    pub shaders_live: AtomicU32,
    pub pipes_startup: AtomicU32,
    pub pipes_live: AtomicU32,
    /// Live requests made after the startup step ended: the ones the precompile missed.
    pub late_shaders: AtomicU32,
    pub late_pipes: AtomicU32,
    pub failed: AtomicU32,
    pub spv_hits: AtomicU32,
    pub spv_misses: AtomicU32,
    pub naga_us: AtomicU64,
    pub pipe_us: AtomicU64,
    pub pipe_max_us: AtomicU64,
    pub saved_pipes: AtomicU32,
}

pub type Task = Box<dyn FnOnce(&Arc<Store>) + Send>;

#[derive(Default)]
struct Queue {
    urgent: VecDeque<Task>,
    normal: VecDeque<Task>,
    stop: bool,
    running: usize,
}

pub struct Store {
    pub device: ash::Device,
    pub cache: vk::PipelineCache,
    /// B-248 (b): `vkMergePipelineCaches` needs `cache` externally synchronized, but the compile workers may now run
    /// while the persisted cache loads. Every pipeline creation with `cache` holds this for reading (creation is
    /// internally synchronized, so they stay parallel); the merge holds it for writing, a few ms.
    pub cache_use: std::sync::RwLock<()>,
    set0_layout: vk::DescriptorSetLayout,
    lm_layout: vk::DescriptorSetLayout,
    particle_layout: vk::PipelineLayout,
    inner: Mutex<Inner>,
    queue: Mutex<Queue>,
    wake: Condvar,
    /// Where workers above `max_active` wait (B-248): off `wake`, so a `notify_one` is never spent on a parked worker.
    park: Condvar,
    /// B-248: only workers with an index below this take tasks. The startup step may use more workers while the
    /// game is held; `set_max_active` lowers it to the play-time count when the step ends.
    max_active: std::sync::atomic::AtomicUsize,
    idle: Condvar,
    threads: Mutex<Vec<std::thread::JoinHandle<()>>>,
    pub ctr: Counters,
    /// Set when the startup step is over: live builds after this are misses.
    pub startup_done: AtomicBool,
    dir: Option<PathBuf>,
    cache_dirty_at: AtomicU32,
    /// Log every miss (`BOTS_N1_COMPILE_LOG=1`).
    pub log_misses: bool,
    /// Cull mode and front face are set per draw (Vulkan 1.3 dynamic state); else they are part of the pipeline key.
    pub dyn_cull: bool,
    /// For `precompile::maybe_save`.
    pub manifest_header: Mutex<String>,
    pub manifest_saved: AtomicU32,
    pub last_save: Mutex<std::time::Instant>,
    /// B-223: guards `cache` against the threads that outlive the renderer. The startup thread is never joined: when
    /// the host remakes the renderer (hybrid dual mode does, twice, at startup) it can still be merging the persisted
    /// cache into `cache`, or saving it, after `destroy` ran. Every merge into and every save of `cache` happens under
    /// this lock and only while the store is alive, and `destroy` takes it before destroying `cache`.
    gate: Mutex<Gate>,
    /// `shutdown` ran: the startup thread stops at its next step and starts no workers.
    stopping: AtomicBool,
}

#[derive(Default)]
struct Gate {
    /// `destroy` ran: `cache` is gone.
    dead: bool,
    /// The persisted cache has been merged into `cache` (or there was none to merge). Until then a save would replace
    /// the file with a nearly empty cache (B-223: each dual-mode remake cut it to 0.2 MB).
    loaded: bool,
}

fn blend_state(blend_id: u8) -> Option<vk::PipelineColorBlendAttachmentState> {
    let over = |c: vk::PipelineColorBlendAttachmentState| {
        c.blend_enable(true)
            .src_alpha_blend_factor(vk::BlendFactor::ONE)
            .dst_alpha_blend_factor(vk::BlendFactor::ONE_MINUS_SRC_ALPHA)
            .alpha_blend_op(vk::BlendOp::ADD)
            .color_blend_op(vk::BlendOp::ADD)
            .color_write_mask(vk::ColorComponentFlags::RGBA)
    };
    let base = vk::PipelineColorBlendAttachmentState::default();
    match blend_id {
        2 => Some(over(
            base.src_color_blend_factor(vk::BlendFactor::SRC_ALPHA)
                .dst_color_blend_factor(vk::BlendFactor::ONE_MINUS_SRC_ALPHA),
        )),
        3 => Some(over(
            base.src_color_blend_factor(vk::BlendFactor::SRC_ALPHA).dst_color_blend_factor(vk::BlendFactor::ONE),
        )),
        4 => Some(over(
            base.src_color_blend_factor(vk::BlendFactor::DST_COLOR).dst_color_blend_factor(vk::BlendFactor::ZERO),
        )),
        _ => None,
    }
}

fn fnv(b: &[u8]) -> u64 {
    crate::plan::fnv(b)
}

impl Store {
    /// `dir`: the n1 cache directory (SPIR-V under `spv-*`, the pipeline cache as `pipeline_cache.bin`); `None` keeps
    /// everything in memory. `particle_layout` is the particle pipeline layout (made by the renderer).
    pub fn new(
        device: ash::Device,
        set0_layout: vk::DescriptorSetLayout,
        lm_layout: vk::DescriptorSetLayout,
        particle_layout: vk::PipelineLayout,
        dir: Option<PathBuf>,
        dyn_cull: bool,
    ) -> Result<Arc<Store>> {
        // Empty now; the persisted data is merged in by `load_persisted_cache` on the startup thread, because some
        // drivers (MoltenVK recompiles every cached shader library) take seconds to read it.
        let cache = unsafe { device.create_pipeline_cache(&vk::PipelineCacheCreateInfo::default(), None)? };
        if let Some(d) = &dir {
            let _ = std::fs::create_dir_all(d.join(format!("spv-{SPV_REV}")));
        }
        Ok(Arc::new(Store {
            device,
            cache,
            cache_use: std::sync::RwLock::new(()),
            set0_layout,
            lm_layout,
            particle_layout,
            inner: Mutex::new(Inner::default()),
            queue: Mutex::new(Queue::default()),
            wake: Condvar::new(),
            park: Condvar::new(),
            max_active: std::sync::atomic::AtomicUsize::new(usize::MAX),
            idle: Condvar::new(),
            threads: Mutex::new(Vec::new()),
            ctr: Counters::default(),
            startup_done: AtomicBool::new(false),
            dir,
            cache_dirty_at: AtomicU32::new(0),
            log_misses: std::env::var("BOTS_N1_COMPILE_LOG").is_ok(),
            dyn_cull,
            manifest_header: Mutex::new(String::new()),
            manifest_saved: AtomicU32::new(0),
            last_save: Mutex::new(std::time::Instant::now()),
            gate: Mutex::new(Gate::default()),
            stopping: AtomicBool::new(false),
        }))
    }

    /// Reads the driver's pipeline cache saved by an earlier run and merges it into the live one. Slow on some
    /// drivers: call from a background thread. Returns (bytes, seconds).
    ///
    /// `vkMergePipelineCaches` needs its destination externally synchronized: the merge holds `cache_use` for writing, so
    /// no pipeline is being created with `cache` during it (the compile workers may already run, B-248 (b)), and
    /// `gate`, so a `destroy` from a renderer remake can't free `cache` under it (B-223).
    pub fn load_persisted_cache(&self) -> (usize, f64) {
        let t = std::time::Instant::now();
        let mark_loaded = || self.gate.lock().unwrap_or_else(|p| p.into_inner()).loaded = true;
        let Some(d) = &self.dir else {
            mark_loaded();
            return (0, 0.0);
        };
        if std::env::var("BOTS_N1_PIPELINE_CACHE").map(|v| v == "0").unwrap_or(false) {
            mark_loaded();
            return (0, 0.0);
        }
        let Ok(data) = std::fs::read(d.join("pipeline_cache.bin")) else {
            mark_loaded();
            return (0, 0.0);
        };
        if self.stopping() {
            return (0, 0.0);
        }
        unsafe {
            // Reading the data is the slow part on some drivers; it makes a cache of its own, so it needs no lock.
            match self.device.create_pipeline_cache(&vk::PipelineCacheCreateInfo::default().initial_data(&data), None) {
                Ok(c) => {
                    {
                        let mut g = self.gate.lock().unwrap_or_else(|p| p.into_inner());
                        let _w = self.cache_use.write().unwrap_or_else(|p| p.into_inner());
                        if !g.dead {
                            let _ = self.device.merge_pipeline_caches(self.cache, &[c]);
                            g.loaded = true;
                        }
                    }
                    self.device.destroy_pipeline_cache(c, None);
                }
                Err(e) => {
                    eprintln!("n1ffi: pipeline cache not loaded: {e}");
                    mark_loaded();
                }
            }
        }
        (data.len(), t.elapsed().as_secs_f64())
    }

    /// `shutdown` ran (the renderer is going away): the startup step gives up.
    pub fn stopping(&self) -> bool {
        self.stopping.load(Relaxed)
    }

    pub fn start_workers(self: &Arc<Self>, n: usize) {
        let mut t = self.threads.lock().unwrap();
        // After `shutdown` (a remake while the startup step ran) no workers: `shutdown` has already joined its own.
        if self.stopping() {
            return;
        }
        for k in t.len()..n {
            let s = self.clone();
            t.push(
                std::thread::Builder::new()
                    .name(format!("n1-compile-{k}"))
                    .spawn(move || s.worker(k))
                    .expect("spawn n1 compile worker"),
            );
        }
    }

    /// B-248: workers with an index at or above `n` stop taking tasks (they finish the one they run).
    pub fn set_max_active(&self, n: usize) {
        let _q = self.queue.lock().unwrap();
        self.max_active.store(n, Relaxed);
        self.wake.notify_all();
    }

    fn worker(self: Arc<Self>, k: usize) {
        loop {
            let task = {
                let mut q = self.queue.lock().unwrap();
                loop {
                    if q.stop {
                        return;
                    }
                    if k >= self.max_active.load(Relaxed) {
                        q = self
                            .park
                            .wait_timeout(q, std::time::Duration::from_millis(250))
                            .unwrap()
                            .0;
                        continue;
                    }
                    if let Some(t) = q.urgent.pop_front().or_else(|| q.normal.pop_front()) {
                        q.running += 1;
                        break t;
                    }
                    q = self.wake.wait(q).unwrap();
                }
            };
            let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| task(&self)));
            if r.is_err() {
                self.ctr.failed.fetch_add(1, Relaxed);
                eprintln!("n1ffi: compile worker task panicked");
            }
            let mut q = self.queue.lock().unwrap();
            q.running -= 1;
            if q.running == 0 && q.urgent.is_empty() && q.normal.is_empty() {
                self.idle.notify_all();
            }
        }
    }

    pub fn spawn(&self, urgent: bool, t: Task) {
        let mut q = self.queue.lock().unwrap();
        if urgent {
            q.urgent.push_back(t);
        } else {
            q.normal.push_back(t);
        }
        self.wake.notify_one();
    }

    /// (shader modules, pipelines) built.
    pub fn counts(&self) -> (usize, usize) {
        let i = self.inner.lock().unwrap();
        (i.mods.len(), i.pipes.values().filter(|p| matches!(p, PState::Ready(_))).count())
    }

    /// Queue depth (waiting + running).
    pub fn backlog(&self) -> usize {
        let q = self.queue.lock().unwrap();
        q.urgent.len() + q.normal.len() + q.running
    }

    /// Stops the workers (queued tasks are dropped) and waits for them. Call before destroying the device objects.
    pub fn shutdown(&self) {
        {
            let mut q = self.queue.lock().unwrap();
            q.stop = true;
            self.stopping.store(true, Relaxed);
            q.urgent.clear();
            q.normal.clear();
            self.wake.notify_all();
            self.park.notify_all();
        }
        let ts: Vec<_> = self.threads.lock().unwrap().drain(..).collect();
        for t in ts {
            let _ = t.join();
        }
    }

    /// Destroys every object the store made. The workers must be stopped and the GPU idle.
    pub unsafe fn destroy(&self) {
        self.save_cache();
        // B-223: wait out a merge or save on the startup thread, then mark `cache` gone before destroying it.
        let mut g = self.gate.lock().unwrap_or_else(|p| p.into_inner());
        g.dead = true;
        let mut i = self.inner.lock().unwrap();
        unsafe {
            for (_, s) in i.pipes.drain() {
                if let PState::Ready(p) = s {
                    self.device.destroy_pipeline(p, None);
                }
            }
            for m in i.mods.drain(..) {
                self.device.destroy_shader_module(m.module, None);
            }
            for (_, (sl, pl)) in i.layouts.drain() {
                self.device.destroy_pipeline_layout(pl, None);
                self.device.destroy_descriptor_set_layout(sl, None);
            }
            self.device.destroy_pipeline_cache(self.cache, None);
        }
        drop(g);
    }

    // ---- layouts ----

    pub fn mat_layout(&self, ntex: usize) -> Result<(vk::DescriptorSetLayout, vk::PipelineLayout)> {
        let mut i = self.inner.lock().unwrap();
        if let Some(&l) = i.layouts.get(&ntex) {
            return Ok(l);
        }
        unsafe {
            let fr = vk::ShaderStageFlags::FRAGMENT;
            let mut bindings = vec![
                vk::DescriptorSetLayoutBinding::default()
                    .binding(0)
                    .descriptor_type(vk::DescriptorType::SAMPLER)
                    .descriptor_count(1)
                    .stage_flags(fr),
                vk::DescriptorSetLayoutBinding::default()
                    .binding(1)
                    .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER)
                    .descriptor_count(1)
                    .stage_flags(fr),
            ];
            for k in 0..ntex {
                bindings.push(
                    vk::DescriptorSetLayoutBinding::default()
                        .binding(2 + k as u32)
                        .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                        .descriptor_count(1)
                        .stage_flags(fr),
                );
            }
            let sl = self
                .device
                .create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings), None)?;
            let layouts = [self.set0_layout, self.lm_layout, sl];
            let pl = self
                .device
                .create_pipeline_layout(&vk::PipelineLayoutCreateInfo::default().set_layouts(&layouts), None)?;
            i.layouts.insert(ntex, (sl, pl));
            Ok((sl, pl))
        }
    }

    // ---- shader modules ----

    /// SPIR-V for WGSL source, from the disk cache when it has it.
    pub fn spirv_cached(&self, src: &str) -> Result<(Arc<Vec<u32>>, u64)> {
        let h = fnv(src.as_bytes());
        let file = self.dir.as_ref().map(|d| d.join(format!("spv-{SPV_REV}")).join(format!("{h:016x}.spv")));
        if let Some(f) = &file
            && let Ok(b) = std::fs::read(f)
            && b.len() % 4 == 0
            && b.len() >= 20
        {
            let words: Vec<u32> = b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
            if words[0] == 0x0723_0203 {
                self.ctr.spv_hits.fetch_add(1, Relaxed);
                return Ok((Arc::new(words), h));
            }
        }
        let t = std::time::Instant::now();
        let words = shaders::spirv(src)?;
        self.ctr.naga_us.fetch_add(t.elapsed().as_micros() as u64, Relaxed);
        self.ctr.spv_misses.fetch_add(1, Relaxed);
        if let Some(f) = &file {
            let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
            let tmp = f.with_extension(format!("tmp{}", std::process::id()));
            if std::fs::write(&tmp, bytes).is_ok() {
                let _ = std::fs::rename(&tmp, f);
            }
        }
        Ok((Arc::new(words), h))
    }

    fn spv_path(&self, name: &str) -> Option<PathBuf> {
        self.dir.as_ref().map(|d| d.join(format!("spv-{SPV_REV}")).join(format!("{name}.spv")))
    }

    /// SPIR-V by its name in the disk cache (`{hash:016x}` for scene shaders, `p{hash:016x}` for particles).
    pub fn read_spv(&self, name: &str) -> Option<Vec<u32>> {
        let b = std::fs::read(self.spv_path(name)?).ok()?;
        if b.len() < 20 || b.len() % 4 != 0 {
            return None;
        }
        let w: Vec<u32> = b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
        (w[0] == 0x0723_0203).then_some(w)
    }

    /// The module id for a key, when it is built.
    pub fn module_lookup(&self, key: u64) -> Option<u32> {
        match self.inner.lock().unwrap().by_key.get(&key) {
            Some(MState::Ready(id)) => Some(*id),
            _ => None,
        }
    }

    pub fn module(&self, id: u32) -> (vk::ShaderModule, usize) {
        let i = self.inner.lock().unwrap();
        let m = &i.mods[id as usize];
        (m.module, m.ntex)
    }

    pub fn ntex_of(&self, id: u32) -> usize {
        self.inner.lock().unwrap().mods[id as usize].ntex
    }

    /// The module for `key`, waiting (briefly) for whoever claimed it to finish.
    fn wait_module(&self, key: u64, origin: Origin) -> Option<u32> {
        // B-248: the startup step's jobs must not give up on a module another worker is still building (a cold naga
        // build can take seconds under load): the pipelines after it would be skipped and built late, in play.
        let limit = if origin == Origin::Startup {
            120_000
        } else {
            2000
        };
        for _ in 0..limit {
            match self.inner.lock().unwrap().by_key.get(&key) {
                Some(MState::Ready(id)) => return Some(*id),
                Some(MState::Building) => {}
                _ => return None,
            }
            std::thread::sleep(std::time::Duration::from_millis(1));
        }
        None
    }

    /// Claims `key` for building: true when the caller must build it (nobody has).
    fn claim_module(&self, key: u64) -> bool {
        let mut i = self.inner.lock().unwrap();
        if i.by_key.contains_key(&key) {
            return false;
        }
        i.by_key.insert(key, MState::Building);
        true
    }

    fn finish_module(&self, key: u64, r: Result<(vk::ShaderModule, usize, u64)>, particle: bool, origin: Origin, ms: f64) -> Option<u32> {
        let mut i = self.inner.lock().unwrap();
        match r {
            Ok((module, ntex, spv)) => {
                i.mods.push(ModEntry { module, ntex, key, spv, particle });
                let id = i.mods.len() as u32 - 1;
                i.by_key.insert(key, MState::Ready(id));
                match origin {
                    Origin::Startup => self.ctr.shaders_startup.fetch_add(1, Relaxed),
                    Origin::Live => self.ctr.shaders_live.fetch_add(1, Relaxed),
                };
                if self.log_misses && origin == Origin::Live {
                    eprintln!("n1ffi: live shader {key:016x} built in {ms:.1} ms (startup done: {})", self.startup_done.load(Relaxed));
                }
                Some(id)
            }
            Err(e) => {
                i.by_key.insert(key, MState::Failed);
                self.ctr.failed.fetch_add(1, Relaxed);
                eprintln!("n1ffi: shader {key:016x}: {e:#}");
                None
            }
        }
    }

    fn make_module(&self, words: &[u32]) -> Result<vk::ShaderModule> {
        Ok(unsafe { self.device.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(words), None)? })
    }

    /// Builds a scene material shader (if nobody has): WGSL assembly, SPIR-V (disk cache or naga), module. Returns the
    /// module id when it is, or becomes, ready; `None` when another thread is still building it, or it failed.
    pub fn build_scene_shader(&self, key: u64, c: &Compiled, flags: ShaderFlags, origin: Origin) -> Option<u32> {
        if !self.claim_module(key) {
            return self.wait_module(key, origin);
        }
        self.build_scene_shader_claimed(key, c, flags, origin)
    }

    fn build_scene_shader_claimed(&self, key: u64, c: &Compiled, flags: ShaderFlags, origin: Origin) -> Option<u32> {
        let t = std::time::Instant::now();
        let r = (|| {
            let src = shaders::scene_source(c, flags);
            let (words, h) = self.spirv_cached(&src)?;
            Ok((self.make_module(&words)?, c.ntex, h))
        })();
        self.finish_module(key, r, false, origin, t.elapsed().as_secs_f64() * 1e3)
    }

    /// Builds a scene shader from the disk cache alone (a manifest entry): no WGSL, no packages. `None` if the SPIR-V
    /// isn't there (the caller falls back to enumeration) or it failed.
    pub fn build_scene_shader_cached(&self, key: u64, spv: u64, ntex: usize, origin: Origin) -> Option<u32> {
        if !self.claim_module(key) {
            return self.wait_module(key, origin);
        }
        let t = std::time::Instant::now();
        let r = match self.read_spv(&format!("{spv:016x}")) {
            Some(w) => {
                self.ctr.spv_hits.fetch_add(1, Relaxed);
                self.make_module(&w).map(|m| (m, ntex, spv)).map_err(Into::into)
            }
            None => {
                // Not a failure: the caller will enumerate instead, which builds it from its WGSL.
                self.inner.lock().unwrap().by_key.remove(&key);
                return None;
            }
        };
        self.finish_module(key, r, false, origin, t.elapsed().as_secs_f64() * 1e3)
    }

    /// A particle module from the disk cache (a manifest entry).
    pub fn build_particle_module_cached(&self, key: u64, origin: Origin) -> Option<u32> {
        if !self.claim_module(key) {
            return self.wait_module(key, origin);
        }
        let r = match self.read_spv(&format!("p{key:016x}")) {
            Some(w) => self.make_module(&w).map(|m| (m, 0, key)).map_err(Into::into),
            None => {
                self.inner.lock().unwrap().by_key.remove(&key);
                return None;
            }
        };
        self.finish_module(key, r, true, origin, 0.0)
    }

    /// A module from SPIR-V words already in hand (particle materials), keyed by `key` (hash of its WGSL).
    pub fn module_from_spv(&self, key: u64, words: &[u32], origin: Origin) -> Option<u32> {
        if !self.claim_module(key) {
            return self.wait_module(key, origin);
        }
        if let Some(f) = self.spv_path(&format!("p{key:016x}"))
            && !f.exists()
        {
            let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
            let tmp = f.with_extension(format!("tmp{}", std::process::id()));
            if std::fs::write(&tmp, bytes).is_ok() {
                let _ = std::fs::rename(&tmp, &f);
            }
        }
        if self.startup_done.load(Relaxed) && origin == Origin::Live {
            self.ctr.late_shaders.fetch_add(1, Relaxed);
        }
        let r = self.make_module(words).map(|m| (m, 0, key)).map_err(Into::into);
        self.finish_module(key, r, true, origin, 0.0)
    }

    /// Asks for a scene shader without waiting: the render thread's call when a material arrives whose shader isn't
    /// built. `after`: pipelines to make once it is.
    pub fn want_scene_shader(self: &Arc<Self>, key: u64, c: Compiled, flags: ShaderFlags, after: Vec<(u8, bool)>) {
        if !self.claim_module(key) {
            return;
        }
        if self.startup_done.load(Relaxed) {
            self.ctr.late_shaders.fetch_add(1, Relaxed);
        }
        self.spawn(
            true,
            Box::new(move |s| {
                if let Some(id) = s.build_scene_shader_claimed(key, &c, flags, Origin::Live) {
                    for (blend, skin) in after {
                        s.build_scene_pipe(SceneKey { shader: id, blend, skin, cull: 0 }, Origin::Live);
                    }
                }
            }),
        );
    }

    // ---- pipelines ----

    pub fn pipe_lookup(&self, k: AnyKeyRef) -> Option<vk::Pipeline> {
        let k = match k {
            AnyKeyRef::Scene(s) => AnyKey::Scene(s),
            AnyKeyRef::Part(p) => AnyKey::Part(p),
        };
        match self.inner.lock().unwrap().pipes.get(&k) {
            Some(PState::Ready(p)) => Some(*p),
            _ => None,
        }
    }

    fn claim_pipe(&self, k: AnyKey) -> bool {
        let mut i = self.inner.lock().unwrap();
        if i.pipes.contains_key(&k) {
            return false;
        }
        i.pipes.insert(k, PState::Building);
        true
    }

    fn finish_pipe(&self, k: AnyKey, r: Result<vk::Pipeline>, origin: Origin, us: u64) -> Option<vk::Pipeline> {
        let mut i = self.inner.lock().unwrap();
        self.ctr.pipe_us.fetch_add(us, Relaxed);
        self.ctr.pipe_max_us.fetch_max(us, Relaxed);
        match r {
            Ok(p) => {
                i.pipes.insert(k, PState::Ready(p));
                match origin {
                    Origin::Startup => self.ctr.pipes_startup.fetch_add(1, Relaxed),
                    Origin::Live => self.ctr.pipes_live.fetch_add(1, Relaxed),
                };
                if self.log_misses && origin == Origin::Live {
                    eprintln!("n1ffi: live pipeline {k:?} built in {:.1} ms (startup done: {})", us as f64 / 1e3, self.startup_done.load(Relaxed));
                }
                Some(p)
            }
            Err(e) => {
                i.pipes.insert(k, PState::Failed);
                self.ctr.failed.fetch_add(1, Relaxed);
                eprintln!("n1ffi: pipeline {k:?}: {e:#}");
                None
            }
        }
    }

    /// Builds a scene pipeline (if nobody has) on the calling thread.
    pub fn build_scene_pipe(&self, k: SceneKey, origin: Origin) -> Option<vk::Pipeline> {
        if !self.claim_pipe(AnyKey::Scene(k)) {
            return self.pipe_lookup(AnyKeyRef::Scene(k));
        }
        self.build_scene_pipe_claimed(k, origin)
    }

    fn build_scene_pipe_claimed(&self, k: SceneKey, origin: Origin) -> Option<vk::Pipeline> {
        let t = std::time::Instant::now();
        let r = self.create_scene_pipe(k);
        self.finish_pipe(AnyKey::Scene(k), r, origin, t.elapsed().as_micros() as u64)
    }

    /// The render thread's request: build it on a worker, urgently, and report it as a miss if startup is over.
    pub fn want_scene_pipe(self: &Arc<Self>, k: SceneKey) {
        if !self.claim_pipe(AnyKey::Scene(k)) {
            return;
        }
        if self.startup_done.load(Relaxed) {
            self.ctr.late_pipes.fetch_add(1, Relaxed);
        }
        self.spawn(
            true,
            Box::new(move |s| {
                s.build_scene_pipe_claimed(k, Origin::Live);
            }),
        );
    }

    /// B-199: a pipeline the level preload expects to need: built on a worker behind urgent work, and not counted as
    /// late (nothing is waiting for it yet).
    pub fn prewarm_scene_pipe(self: &Arc<Self>, k: SceneKey) {
        if !self.claim_pipe(AnyKey::Scene(k)) {
            return;
        }
        self.spawn(
            false,
            Box::new(move |s| {
                s.build_scene_pipe_claimed(k, Origin::Live);
            }),
        );
    }

    pub fn build_part_pipe(&self, k: PartKey, origin: Origin) -> Option<vk::Pipeline> {
        if !self.claim_pipe(AnyKey::Part(k)) {
            return self.pipe_lookup(AnyKeyRef::Part(k));
        }
        self.build_part_pipe_claimed(k, origin)
    }

    fn build_part_pipe_claimed(&self, k: PartKey, origin: Origin) -> Option<vk::Pipeline> {
        let t = std::time::Instant::now();
        let r = self.create_part_pipe(k);
        self.finish_pipe(AnyKey::Part(k), r, origin, t.elapsed().as_micros() as u64)
    }

    pub fn want_part_pipe(self: &Arc<Self>, k: PartKey) {
        if !self.claim_pipe(AnyKey::Part(k)) {
            return;
        }
        if self.startup_done.load(Relaxed) {
            self.ctr.late_pipes.fetch_add(1, Relaxed);
        }
        self.spawn(
            true,
            Box::new(move |s| {
                s.build_part_pipe_claimed(k, Origin::Live);
            }),
        );
    }

    fn create_scene_pipe(&self, key: SceneKey) -> Result<vk::Pipeline> {
        let (module, ntex) = self.module(key.shader);
        let (_, layout) = self.mat_layout(ntex)?;
        unsafe {
            let blend = blend_state(key.blend);
            let write = key.blend < 2;
            let stages = [
                vk::PipelineShaderStageCreateInfo::default()
                    .stage(vk::ShaderStageFlags::VERTEX)
                    .module(module)
                    .name(if key.skin { c"vs_skin" } else { c"vs" }),
                vk::PipelineShaderStageCreateInfo::default()
                    .stage(vk::ShaderStageFlags::FRAGMENT)
                    .module(module)
                    .name(c"fs"),
            ];
            let stride = if key.skin {
                std::mem::size_of::<assetdb::SkinVertex>()
            } else {
                std::mem::size_of::<crate::vkr::Vertex>()
            } as u32;
            let bindings =
                [vk::VertexInputBindingDescription { binding: 0, stride, input_rate: vk::VertexInputRate::VERTEX }];
            let attr = |location, format, offset| vk::VertexInputAttributeDescription {
                location,
                binding: 0,
                format,
                offset,
            };
            let attrs: Vec<vk::VertexInputAttributeDescription> = if key.skin {
                vec![
                    attr(0, vk::Format::R32G32B32_SFLOAT, 0),
                    attr(1, vk::Format::R32G32B32_SFLOAT, 12),
                    attr(2, vk::Format::R32G32_SFLOAT, 24),
                    attr(3, vk::Format::R8G8B8A8_UINT, 32),
                    attr(4, vk::Format::R8G8B8A8_UNORM, 36),
                ]
            } else {
                vec![
                    attr(0, vk::Format::R32G32B32_SFLOAT, 0),
                    attr(1, vk::Format::R32G32B32_SFLOAT, 12),
                    attr(2, vk::Format::R32G32B32A32_SFLOAT, 24),
                    attr(3, vk::Format::R32G32_SFLOAT, 40),
                    attr(4, vk::Format::R32G32_SFLOAT, 48),
                    attr(5, vk::Format::R8G8B8A8_UNORM, 56),
                ]
            };
            let vi = vk::PipelineVertexInputStateCreateInfo::default()
                .vertex_binding_descriptions(&bindings)
                .vertex_attribute_descriptions(&attrs);
            let ia = vk::PipelineInputAssemblyStateCreateInfo::default().topology(vk::PrimitiveTopology::TRIANGLE_LIST);
            let vp = vk::PipelineViewportStateCreateInfo::default().viewport_count(1).scissor_count(1);
            // naga flips clip-space Y, so wgpu's Ccw is COUNTER_CLOCKWISE here; a mirrored transform flips it.
            let rs = vk::PipelineRasterizationStateCreateInfo::default()
                .polygon_mode(vk::PolygonMode::FILL)
                .cull_mode(if key.cull & 1 != 0 { vk::CullModeFlags::NONE } else { vk::CullModeFlags::BACK })
                .front_face(if key.cull & 2 != 0 { vk::FrontFace::CLOCKWISE } else { vk::FrontFace::COUNTER_CLOCKWISE })
                .line_width(1.0);
            let ms = vk::PipelineMultisampleStateCreateInfo::default().rasterization_samples(vk::SampleCountFlags::TYPE_1);
            let ds = vk::PipelineDepthStencilStateCreateInfo::default()
                .depth_test_enable(true)
                .depth_write_enable(write)
                .depth_compare_op(vk::CompareOp::GREATER); // reverse Z
            let att = [blend
                .unwrap_or(vk::PipelineColorBlendAttachmentState::default().color_write_mask(vk::ColorComponentFlags::RGBA))];
            let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&att);
            let dyns = [
                vk::DynamicState::VIEWPORT,
                vk::DynamicState::SCISSOR,
                vk::DynamicState::CULL_MODE,
                vk::DynamicState::FRONT_FACE,
            ];
            let n_dyn = if self.dyn_cull { 4 } else { 2 };
            let dy = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyns[..n_dyn]);
            let formats = [HDR_FORMAT];
            let mut rendering = vk::PipelineRenderingCreateInfo::default()
                .color_attachment_formats(&formats)
                .depth_attachment_format(DEPTH_FORMAT);
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
                .layout(layout)
                .push_next(&mut rendering);
            let _use = self.cache_use.read().unwrap_or_else(|p| p.into_inner());
            Ok(self
                .device
                .create_graphics_pipelines(self.cache, &[info], None)
                .map_err(|(_, e)| anyhow!("pipeline {key:?}: {e}"))?[0])
        }
    }

    fn create_part_pipe(&self, key: PartKey) -> Result<vk::Pipeline> {
        let module = self.module(key.module).0;
        let mesh = key.variant == 2;
        unsafe {
            let blend = blend_state(key.blend);
            let entry = match key.variant {
                0 => c"vs_sprite",
                1 => c"vs_velocity",
                _ => c"vs_mesh",
            };
            let stages = [
                vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::VERTEX).module(module).name(entry),
                vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::FRAGMENT).module(module).name(c"fs"),
            ];
            let f4 = vk::Format::R32G32B32A32_SFLOAT;
            let sprite_binds =
                [vk::VertexInputBindingDescription { binding: 0, stride: 80, input_rate: vk::VertexInputRate::INSTANCE }];
            let sprite_attrs: Vec<_> = (0..5)
                .map(|k| vk::VertexInputAttributeDescription { location: k, binding: 0, format: f4, offset: k * 16 })
                .collect();
            let mesh_binds = [
                vk::VertexInputBindingDescription { binding: 0, stride: 32, input_rate: vk::VertexInputRate::VERTEX },
                vk::VertexInputBindingDescription { binding: 1, stride: 80, input_rate: vk::VertexInputRate::INSTANCE },
            ];
            let mut mesh_attrs = vec![
                vk::VertexInputAttributeDescription { location: 0, binding: 0, format: vk::Format::R32G32B32_SFLOAT, offset: 0 },
                vk::VertexInputAttributeDescription { location: 1, binding: 0, format: vk::Format::R32G32B32_SFLOAT, offset: 12 },
                vk::VertexInputAttributeDescription { location: 2, binding: 0, format: vk::Format::R32G32_SFLOAT, offset: 24 },
            ];
            for k in 0..5u32 {
                mesh_attrs.push(vk::VertexInputAttributeDescription { location: 3 + k, binding: 1, format: f4, offset: k * 16 });
            }
            let vi = if mesh {
                vk::PipelineVertexInputStateCreateInfo::default()
                    .vertex_binding_descriptions(&mesh_binds)
                    .vertex_attribute_descriptions(&mesh_attrs)
            } else {
                vk::PipelineVertexInputStateCreateInfo::default()
                    .vertex_binding_descriptions(&sprite_binds)
                    .vertex_attribute_descriptions(&sprite_attrs)
            };
            let ia = vk::PipelineInputAssemblyStateCreateInfo::default().topology(vk::PrimitiveTopology::TRIANGLE_LIST);
            let vp = vk::PipelineViewportStateCreateInfo::default().viewport_count(1).scissor_count(1);
            let rs = vk::PipelineRasterizationStateCreateInfo::default()
                .polygon_mode(vk::PolygonMode::FILL)
                .cull_mode(if mesh && !key.two_sided { vk::CullModeFlags::BACK } else { vk::CullModeFlags::NONE })
                .front_face(vk::FrontFace::COUNTER_CLOCKWISE)
                .line_width(1.0);
            let ms = vk::PipelineMultisampleStateCreateInfo::default().rasterization_samples(vk::SampleCountFlags::TYPE_1);
            let ds = vk::PipelineDepthStencilStateCreateInfo::default()
                .depth_test_enable(true)
                .depth_write_enable(mesh && key.blend < 2)
                .depth_compare_op(vk::CompareOp::GREATER);
            let att = [blend
                .unwrap_or(vk::PipelineColorBlendAttachmentState::default().color_write_mask(vk::ColorComponentFlags::RGBA))];
            let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&att);
            let dyns = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
            let dy = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyns);
            let formats = [HDR_FORMAT];
            let mut rendering = vk::PipelineRenderingCreateInfo::default()
                .color_attachment_formats(&formats)
                .depth_attachment_format(DEPTH_FORMAT);
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
                .layout(self.particle_layout)
                .push_next(&mut rendering);
            let _use = self.cache_use.read().unwrap_or_else(|p| p.into_inner());
            Ok(self
                .device
                .create_graphics_pipelines(self.cache, &[info], None)
                .map_err(|(_, e)| anyhow!("particle pipeline {key:?}: {e}"))?[0])
        }
    }

    /// A pipeline that the post passes use (any layout, fixed function): not keyed, owned by the renderer.
    pub fn create_post_pipe(
        &self,
        module: vk::ShaderModule,
        layout: vk::PipelineLayout,
        fs: &std::ffi::CStr,
        out: vk::Format,
    ) -> Result<vk::Pipeline> {
        unsafe {
            let stages = [
                vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::VERTEX).module(module).name(c"vs"),
                vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::FRAGMENT).module(module).name(fs),
            ];
            let vi = vk::PipelineVertexInputStateCreateInfo::default();
            let ia = vk::PipelineInputAssemblyStateCreateInfo::default().topology(vk::PrimitiveTopology::TRIANGLE_LIST);
            let vp = vk::PipelineViewportStateCreateInfo::default().viewport_count(1).scissor_count(1);
            let rs = vk::PipelineRasterizationStateCreateInfo::default()
                .polygon_mode(vk::PolygonMode::FILL)
                .cull_mode(vk::CullModeFlags::NONE)
                .line_width(1.0);
            let ms = vk::PipelineMultisampleStateCreateInfo::default().rasterization_samples(vk::SampleCountFlags::TYPE_1);
            let ds = vk::PipelineDepthStencilStateCreateInfo::default();
            let att = [vk::PipelineColorBlendAttachmentState::default().color_write_mask(vk::ColorComponentFlags::RGBA)];
            let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&att);
            let dyns = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
            let dy = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyns);
            let formats = [out];
            let mut rendering = vk::PipelineRenderingCreateInfo::default().color_attachment_formats(&formats);
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
                .layout(layout)
                .push_next(&mut rendering);
            let _use = self.cache_use.read().unwrap_or_else(|p| p.into_inner());
            Ok(self
                .device
                .create_graphics_pipelines(self.cache, &[info], None)
                .map_err(|(_, e)| anyhow!("post pipeline {fs:?}: {e}"))?[0])
        }
    }

    // ---- persistence ----

    /// Writes the driver's pipeline cache next to the SPIR-V (atomically). Cheap when nothing new was built.
    /// Saves run under `gate` (one at a time, never after `destroy`, never before the persisted cache was merged).
    pub fn save_cache(&self) {
        let Some(d) = &self.dir else { return };
        let g = self.gate.lock().unwrap_or_else(|p| p.into_inner());
        if g.dead || !g.loaded {
            return;
        }
        let built = self.ctr.pipes_startup.load(Relaxed) + self.ctr.pipes_live.load(Relaxed);
        if self.cache_dirty_at.swap(built, Relaxed) == built && built != 0 {
            return;
        }
        let Ok(data) = (unsafe { self.device.get_pipeline_cache_data(self.cache) }) else { return };
        let f = d.join("pipeline_cache.bin");
        let tmp = f.with_extension(format!("tmp{}", std::process::id()));
        if std::fs::write(&tmp, &data).is_ok() {
            let _ = std::fs::rename(&tmp, &f);
            self.ctr.saved_pipes.store(built, Relaxed);
        }
    }
}

/// Everything built so far, by persistent key (not store ids): what the next boot should build before play.
#[derive(Default, Debug, Clone, PartialEq)]
pub struct Snapshot {
    /// (skey, spv name, ntex)
    pub scene_mods: Vec<(u64, u64, usize)>,
    /// (skey, blend, skin)
    pub scene_pipes: Vec<(u64, u8, bool)>,
    pub part_mods: Vec<u64>,
    /// (module key, blend, two_sided, variant)
    pub part_pipes: Vec<(u64, u8, bool, u8)>,
}

impl Store {
    pub fn snapshot(&self) -> Snapshot {
        let i = self.inner.lock().unwrap();
        let mut sn = Snapshot::default();
        for m in &i.mods {
            if m.particle {
                sn.part_mods.push(m.key);
            } else {
                sn.scene_mods.push((m.key, m.spv, m.ntex));
            }
        }
        for (k, st) in &i.pipes {
            if !matches!(st, PState::Ready(_)) {
                continue;
            }
            match k {
                AnyKey::Scene(k) => sn.scene_pipes.push((i.mods[k.shader as usize].key, k.blend, k.skin)),
                AnyKey::Part(k) => sn.part_pipes.push((i.mods[k.module as usize].key, k.blend, k.two_sided, k.variant)),
            }
        }
        sn.scene_mods.sort();
        sn.scene_pipes.sort();
        sn.part_mods.sort();
        sn.part_pipes.sort();
        sn
    }

    pub fn set_manifest_header(&self, h: String) {
        *self.manifest_header.lock().unwrap() = h;
    }

    pub fn dir(&self) -> Option<&std::path::Path> {
        self.dir.as_deref()
    }
}

#[derive(Clone, Copy)]
pub enum AnyKeyRef {
    Scene(SceneKey),
    Part(PartKey),
}
