//! B-187: SpeedTree trees in the live renderer. CPU side: handles, a loader thread (baked light, materials, textures,
//! the geometry cache), per-frame culling into instances, and the capture that builds each tree asset's geometry.
//!
//! The geometry is not in the disc's packages: SpeedTreeRT, compiled into the game, generates it at load. The port
//! therefore captures it once per asset: while the game draws a tree, the host hands over that draw's vertex shader
//! constants and vertex streams, and [`crate::xenos_vs`] runs the game's own vertex shader on the CPU. The result
//! (tree-local positions, UVs, normals, leaf-card corner offsets) is cached in the ignored cache dir
//! (`trees/<asset>.bin`), generated on the player's machine from the code on their disc; nothing here is shipped.
//! The Vulkan side is `vkr/tgpu.rs`.

use crate::vkr::Renderer;
use crate::xenos_vs::{Program, Stream, Vm};
use anyhow::{Context, Result, bail};
use assetdb::{AssetDb, TextureAsset, TreeLight};
use std::collections::{HashMap, HashSet};
use std::sync::{Arc, mpsc};

pub const NONE: u32 = u32::MAX;
/// Floats per vertex: local pos 3, uv 2, card offset 3, normal 3, light slot, light (VB) index.
pub const STRIDE: usize = 13;
const MAGIC: u32 = 0x3145_5254; // "TRE1"

/// One tree placement as the host passes it. 88 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct N1Tree {
    /// Component LocalToWorld (UE3, row-major, no rotation: scale and translation).
    pub transform: [f32; 16],
    /// `n1_tree_component` handle (baked light).
    pub component: u32,
    /// `n1_tree_asset` handle (geometry, materials).
    pub asset: u32,
    /// The actor's rotation (pitch, yaw, roll) in UE3 units.
    pub rotation: [i32; 3],
    pub pad: u32,
}

/// One tree draw of the game's colour pass, as the capture hook hands it over. Pointers are valid for the call only.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct N1TreeDraw {
    pub vs_hash: u64,
    /// 256 vec4 vertex shader constants.
    pub consts: *const f32,
    /// Vertex streams in the vertex shader's binding order (position, then packed attributes), guest byte order.
    pub stream: [*const u8; 4],
    pub stream_bytes: [u32; 4],
    pub stream_stride: [u32; 4],
    pub stream_addr: [u32; 4],
    pub stream_count: u32,
    pub pad: u32,
    /// Indices as fetched (host order), the draw's base index offset, and how many.
    pub indices: *const u32,
    pub index_count: u32,
    pub index_offset: u32,
    /// Texture fetch constant word 1 of the pixel shader's textures (base address in the high bits).
    pub tex_base: [u32; 4],
    pub tex_count: u32,
    /// Vertex fetch constant of each stream (stream 0 in the low byte, stream 1 in the next): the shader's `vfN`.
    pub fetch_consts: u32, // one byte per stream
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Kind {
    Branch,
    Frond,
    LeafCard,
    LeafMesh,
}

impl Kind {
    fn code(self) -> u32 {
        self as u32
    }
    fn from_code(c: u32) -> Option<Kind> {
        Some(match c {
            0 => Kind::Branch,
            1 => Kind::Frond,
            2 => Kind::LeafCard,
            3 => Kind::LeafMesh,
            _ => return None,
        })
    }
    /// Baked-light slot: branch and frond share slot 0.
    pub fn light_slot(self) -> f32 {
        match self {
            Kind::Branch | Kind::Frond => 0.0,
            Kind::LeafCard => 1.0,
            Kind::LeafMesh => 3.0,
        }
    }
    /// Material in `[branch, frond, leaf]`.
    pub fn material(self) -> usize {
        match self {
            Kind::Branch => 0,
            Kind::Frond => 1,
            Kind::LeafCard | Kind::LeafMesh => 2,
        }
    }
    pub fn is_leaf(self) -> bool {
        matches!(self, Kind::LeafCard | Kind::LeafMesh)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct GDraw {
    pub kind: Kind,
    pub first: u32,
    pub count: u32,
}

/// An asset's captured geometry.
#[derive(Clone, Debug, Default)]
pub struct Geometry {
    pub vertices: Vec<f32>,
    pub indices: Vec<u32>,
    pub draws: Vec<GDraw>,
}

impl Geometry {
    fn encode(&self) -> Vec<u8> {
        let mut o = Vec::with_capacity(16 + self.draws.len() * 12 + self.vertices.len() * 4 + self.indices.len() * 4);
        for v in [MAGIC, (self.vertices.len() / STRIDE) as u32, self.indices.len() as u32, self.draws.len() as u32] {
            o.extend_from_slice(&v.to_le_bytes());
        }
        for d in &self.draws {
            for v in [d.kind.code(), d.first, d.count] {
                o.extend_from_slice(&v.to_le_bytes());
            }
        }
        o.extend_from_slice(bytemuck::cast_slice(&self.vertices));
        o.extend_from_slice(bytemuck::cast_slice(&self.indices));
        o
    }

    fn decode(b: &[u8]) -> Result<Geometry> {
        let u = |i: usize| -> Result<u32> {
            let s = b.get(i * 4..i * 4 + 4).context("truncated")?;
            Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
        };
        if u(0)? != MAGIC {
            bail!("magic");
        }
        let (nv, ni, nd) = (u(1)? as usize, u(2)? as usize, u(3)? as usize);
        let mut at = 4 + nd * 3;
        let mut draws = Vec::with_capacity(nd);
        for k in 0..nd {
            draws.push(GDraw {
                kind: Kind::from_code(u(4 + k * 3)?).context("kind")?,
                first: u(5 + k * 3)?,
                count: u(6 + k * 3)?,
            });
        }
        let vb = b.get(at * 4..(at + nv * STRIDE) * 4).context("truncated vertices")?;
        at += nv * STRIDE;
        let ib = b.get(at * 4..(at + ni) * 4).context("truncated indices")?;
        let vertices: Vec<f32> = vb.chunks_exact(4).map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
        let indices: Vec<u32> = ib.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
        if indices.iter().any(|&i| i as usize >= nv) {
            bail!("index out of range");
        }
        Ok(Geometry { vertices, indices, draws })
    }

    /// AABB centre and bounding radius around it, card corners at their largest extent (UE axes, tree-local).
    fn bounds(&self) -> ([f32; 3], f32) {
        let (mut lo, mut hi) = ([f32::MAX; 3], [f32::MIN; 3]);
        for c in self.vertices.chunks_exact(STRIDE) {
            for k in 0..3 {
                lo[k] = lo[k].min(c[k]);
                hi[k] = hi[k].max(c[k]);
            }
        }
        if lo[0] > hi[0] {
            return ([0.0; 3], 0.0);
        }
        let ctr = [(lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5];
        let mut r = 0f32;
        for c in self.vertices.chunks_exact(STRIDE) {
            let d = ((c[0] - ctr[0]).powi(2) + (c[1] - ctr[1]).powi(2) + (c[2] - ctr[2]).powi(2)).sqrt();
            let card = (c[5] * c[5] + c[6] * c[6] + c[7] * c[7]).sqrt();
            r = r.max(d + card);
        }
        (ctr, r)
    }
}

/// A texture the tree's material samples, and whether it is alpha-tested.
#[derive(Clone)]
pub struct TreeTex {
    pub tex: Option<Arc<TextureAsset>>,
    pub masked: bool,
}

/// An asset ready for the GPU: geometry plus branch, frond and leaf materials.
pub struct LoadedTree {
    pub geom: Geometry,
    pub mats: [TreeTex; 3],
}

/// Tree quality: how far trees draw and how many leaves.
#[derive(Clone, Copy, Debug)]
pub struct TreeSettings {
    /// Trees whose bounding sphere is farther than this (UE units) are skipped.
    pub max_distance: f32,
    /// Fraction of leaf quads drawn.
    pub leaf_density: f32,
    pub frustum_cull: bool,
}

impl Default for TreeSettings {
    fn default() -> Self {
        TreeSettings { max_distance: f32::INFINITY, leaf_density: 1.0, frustum_cull: true }
    }
}

impl TreeSettings {
    /// `high` (everything), `medium` (20k units, 75% leaves), `low` (10k units, 50% leaves), `off` (no trees).
    pub fn preset(name: &str) -> Option<Self> {
        Some(match name {
            "high" | "" => Self::default(),
            "medium" => TreeSettings { max_distance: 20_000.0, leaf_density: 0.75, frustum_cull: true },
            "low" => TreeSettings { max_distance: 10_000.0, leaf_density: 0.5, frustum_cull: true },
            "off" => TreeSettings { max_distance: 0.0, leaf_density: 0.0, frustum_cull: true },
            _ => return None,
        })
    }

    /// Preset name optionally followed by `,dist=N,leaves=F,nocull`.
    pub fn parse(s: &str) -> Option<Self> {
        let mut it = s.split(',');
        let mut t = Self::preset(it.next().unwrap_or(""))?;
        for kv in it {
            let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
            match k {
                "dist" => t.max_distance = v.parse().ok()?,
                "leaves" => t.leaf_density = v.parse().ok()?,
                "nocull" => t.frustum_cull = false,
                _ => return None,
            }
        }
        Some(t)
    }
}

/// One instance as the GPU reads it. 288 bytes.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct TInst {
    /// Column-major, swizzled for the renderer's Y-up world (`swz_model` of the full UE3 transform).
    pub model: [f32; 16],
    /// x = tree scale (leaf card corners are in units of it).
    pub misc: [f32; 4],
    /// Light-buffer vertex base per slot, -1 = none.
    pub base: [i32; 4],
    /// Light scale per slot and coefficient (slot * 3 + k).
    pub sc: [[f32; 4]; 12],
}

#[derive(Clone, Copy, Debug)]
pub struct TCall {
    /// Renderer asset id.
    pub asset: u32,
    pub first: u32,
    pub count: u32,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct TStats {
    pub placements: u32,
    pub instances: u32,
    pub draws: u32,
    pub assets_ready: u32,
    pub assets_loading: u32,
    pub assets_uncaptured: u32,
    pub assets_failed: u32,
    /// Uncaptured assets some placement in the feed needs right now.
    pub wanted: u32,
    /// Of those, assets with a placement near the camera and in (a generous) view, so the game is likely drawing it:
    /// only these start a capture session (B-199).
    pub wanted_visible: u32,
}

/// Everything the renderer draws for trees this frame.
#[derive(Default)]
pub struct TFrame {
    pub insts: Vec<TInst>,
    pub calls: Vec<TCall>,
    pub leaf_density: f32,
    pub stats: TStats,
}

/// A cheap hasher for the placement grid's `(i32, i32, i32)` keys (std's SipHash costs more than the lookup's work).
#[derive(Default, Clone, Copy)]
pub struct CellHasher(u64);

impl std::hash::Hasher for CellHasher {
    fn finish(&self) -> u64 {
        self.0
    }
    fn write(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.write_u64(u64::from(b));
        }
    }
    fn write_i32(&mut self, v: i32) {
        self.write_u64(u64::from(v as u32));
    }
    fn write_u64(&mut self, v: u64) {
        self.0 = (self.0.rotate_left(5) ^ v).wrapping_mul(0x517c_c1b7_2722_0a95);
    }
}

pub type CellHash = std::hash::BuildHasherDefault<CellHasher>;

// ------------------------------------------------------------------------------------------------ matrices

type M4 = [[f64; 4]; 4];

fn rot3(rot: [i32; 3]) -> [[f64; 3]; 3] {
    let k = std::f64::consts::PI * 2.0 / 65536.0;
    let (sp, cp) = ((rot[0] as f64 * k).sin(), (rot[0] as f64 * k).cos());
    let (sy, cy) = ((rot[1] as f64 * k).sin(), (rot[1] as f64 * k).cos());
    let (sr, cr) = ((rot[2] as f64 * k).sin(), (rot[2] as f64 * k).cos());
    [
        [cp * cy, cp * sy, sp],
        [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp],
        [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp],
    ]
}

fn mul4(a: &M4, b: &M4) -> M4 {
    let mut r = [[0.0; 4]; 4];
    for i in 0..4 {
        for j in 0..4 {
            r[i][j] = (0..4).map(|k| a[i][k] * b[k][j]).sum();
        }
    }
    r
}

/// Gauss-Jordan inverse with partial pivoting; `None` when singular.
fn inv4(m: &M4) -> Option<M4> {
    let mut a = [[0.0f64; 8]; 4];
    for i in 0..4 {
        a[i][..4].copy_from_slice(&m[i]);
        a[i][4 + i] = 1.0;
    }
    for c in 0..4 {
        let p = (c..4).max_by(|&x, &y| a[x][c].abs().total_cmp(&a[y][c].abs()))?;
        if a[p][c].abs() < 1e-12 {
            return None;
        }
        a.swap(c, p);
        let d = a[c][c];
        for x in a[c].iter_mut() {
            *x /= d;
        }
        for r in 0..4 {
            if r != c {
                let f = a[r][c];
                if f != 0.0 {
                    for j in 0..8 {
                        a[r][j] -= f * a[c][j];
                    }
                }
            }
        }
    }
    let mut o = [[0.0; 4]; 4];
    for i in 0..4 {
        o[i].copy_from_slice(&a[i][4..]);
    }
    Some(o)
}

fn to_m4(t: &[f32; 16]) -> M4 {
    let mut m = [[0.0; 4]; 4];
    for i in 0..4 {
        for j in 0..4 {
            m[i][j] = t[i * 4 + j] as f64;
        }
    }
    m
}

/// `R4 x L2W` (row vectors, rotation first): the full transform of a tree placement, UE3 axes.
fn full_transform(t: &N1Tree) -> M4 {
    let r = rot3(t.rotation);
    let mut r4 = [[0.0; 4]; 4];
    for i in 0..3 {
        r4[i][..3].copy_from_slice(&r[i]);
    }
    r4[3][3] = 1.0;
    mul4(&r4, &to_m4(&t.transform))
}

/// `swz_model`: columns 1 and 2 swapped in every row, as the renderer's meshes use it.
fn swz_model(m: &M4) -> [f32; 16] {
    let mut o = [0f32; 16];
    for i in 0..4 {
        o[i * 4] = m[i][0] as f32;
        o[i * 4 + 1] = m[i][2] as f32;
        o[i * 4 + 2] = m[i][1] as f32;
        o[i * 4 + 3] = m[i][3] as f32;
    }
    o
}

// ------------------------------------------------------------------------------------------------ loader thread

enum Job {
    Light(u32, String),
    Asset(u32, String),
    /// B-220: one captured draw to run the game's vertex shader over (`cap_compute`), off the render thread.
    CapDraw(Box<CapJob>),
    /// B-220: a capture session ended: classify, write the cache and load each (asset, path, component path).
    /// The assets to commit, and geometry built on the render thread (BOTS_N1_TREE_ASYNC=0).
    CapCommit(Vec<(u32, String, String)>, HashMap<u32, Cap>),
}

/// A captured tree draw, copied out of guest memory so the loader thread can build its vertices (B-220). Before, the
/// vertex shader ran inside `n1_tree_capture`, on the GPU thread under the renderer's lock: 100-300 ms frames on a
/// UHD 620 in a level's first second with no tree cache.
struct CapJob {
    asset: u32,
    vs_kind: Kind,
    prog: Arc<Program>,
    consts: Vec<f32>,
    /// Per stream: bytes (only the indexed vertices filled in; the rest zero) and stride.
    streams: Vec<(Vec<u8>, usize)>,
    s0: usize,
    s1: usize,
    indices: Vec<u32>,
    index_offset: u32,
    vb: u32,
    min_index: u32,
    row: usize,
    rotation: [i32; 3],
    tex: Vec<u32>,
}

enum Done {
    Light(u32, Option<Arc<TreeLight>>),
    Asset(u32, AssetResult),
}

enum AssetResult {
    Ready(LoadedTree),
    /// No cached geometry: the asset has to be captured from the game's own draws.
    NeedsCapture,
    Failed(String),
}

fn cache_file(db: &AssetDb, asset: &str) -> std::path::PathBuf {
    let n: String = asset.chars().map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-' { c } else { '_' }).collect();
    db.cache_dir().join("trees").join(n + ".bin")
}

fn tree_tex(db: &AssetDb, material: &str) -> TreeTex {
    if material.is_empty() {
        return TreeTex { tex: None, masked: false };
    }
    let m = db.material(material);
    TreeTex { tex: m.diffuse.as_deref().and_then(|t| db.texture(t)), masked: m.blend == "BLEND_Masked" }
}

fn load_asset(db: &AssetDb, path: &str) -> AssetResult {
    let geom = match std::fs::read(cache_file(db, path)) {
        Ok(b) => match Geometry::decode(&b) {
            Ok(g) => g,
            Err(e) => {
                eprintln!("n1ffi: tree cache for {path} unusable ({e:#}); capturing again");
                return AssetResult::NeedsCapture;
            }
        },
        Err(_) => return AssetResult::NeedsCapture,
    };
    let Some(m) = db.tree_materials(path) else {
        return AssetResult::Failed("no such SpeedTree asset in its package".into());
    };
    AssetResult::Ready(LoadedTree {
        geom,
        mats: [tree_tex(db, &m.branch), tree_tex(db, &m.frond), tree_tex(db, &m.leaf)],
    })
}

/// Builds a captured draw's vertices by running the game's vertex shader over its indices (loader thread, B-220).
fn cap_compute(caps: &mut HashMap<u32, Cap>, j: &CapJob) {
    let cap = caps.entry(j.asset).or_default();
    let streams: Vec<Stream> = j.streams.iter().map(|(data, stride)| Stream { data, stride: *stride }).collect();
    let (s0, s1) = (j.s0, j.s1);
    let c: Vec<[f64; 4]> = (0..256).map(|r| [0, 1, 2, 3].map(|k| j.consts[r * 4 + k] as f64)).collect();
    let row = j.row;
    let l2w: M4 = [c[row - 3], c[row - 2], c[row - 1], c[row]];
    let (Some(inv_l2w), Some(vp_inv)) = (inv4(&l2w), inv4(&[c[0], c[1], c[2], c[3]])) else { return };
    let scale = (l2w[0][0].powi(2) + l2w[0][1].powi(2) + l2w[0][2].powi(2)).sqrt().max(1e-9);
    let rm = rot3(j.rotation);
    let vs_kind = j.vs_kind;
    let group = if vs_kind == Kind::Branch { 0u8 } else if vs_kind == Kind::LeafCard { 1 } else { 2 };
    let mut vm = Vm::new(&j.prog, &j.consts, &streams);
    let first = cap.indices.len() as u32;
    for &raw_vi in &j.indices {
        let vi = raw_vi.wrapping_add(j.index_offset);
        let key = (group, j.vb, vi);
        if let Some(&n) = cap.vmap.get(&key) {
            cap.indices.push(n);
            continue;
        }
        let o = match vm.run(vi as usize) {
            Ok(o) => o,
            Err(e) => {
                eprintln!("n1ffi: tree vertex shader run failed: {e:#} ({:?}, index_offset {})", vs_kind, j.index_offset);
                return;
            }
        };
        let (Some(clip), Some(o1)) = (o.get("oPos"), o.get("o1")) else {
            eprintln!("n1ffi: SpeedTree vertex shader ({vs_kind:?}) has no oPos/o1 output");
            return;
        };
        let w = [0, 1, 2].map(|j| (0..4).map(|i| clip[i] * vp_inv[i][j]).sum::<f64>());
        let ww: f64 = (0..4).map(|i| clip[i] * vp_inv[i][3]).sum();
        let world = [w[0] / ww, w[1] / ww, w[2] / ww];
        let pos = {
            let at = vi as usize * 12;
            let Some(b) = streams[s0].data.get(at..at + 12) else { return };
            [0, 1, 2].map(|k| f32::from_be_bytes([b[4 * k], b[4 * k + 1], b[4 * k + 2], b[4 * k + 3]]) as f64)
        };
        let mut card = [0.0f64; 3];
        let local: [f64; 3];
        if vs_kind == Kind::LeafCard {
            // centre of the card in world space: (p · R, 1) · L2W
            let pr = [0, 1, 2].map(|j| pos[0] * rm[0][j] + pos[1] * rm[1][j] + pos[2] * rm[2][j]);
            let ctr = [0, 1, 2].map(|j| pr[0] * l2w[0][j] + pr[1] * l2w[1][j] + pr[2] * l2w[2][j] + l2w[3][j]);
            let off = [world[0] - ctr[0], world[1] - ctr[1], world[2] - ctr[2]];
            for (k, row_c) in [7usize, 8, 9].iter().enumerate() {
                card[k] = (off[0] * c[*row_c][0] + off[1] * c[*row_c][1] + off[2] * c[*row_c][2]) / scale;
            }
            local = pos;
        } else {
            let lw = [0, 1, 2].map(|j| world[0] * inv_l2w[0][j] + world[1] * inv_l2w[1][j] + world[2] * inv_l2w[2][j] + inv_l2w[3][j]);
            // local = lw · Rᵀ (R is orthonormal)
            local = [0, 1, 2].map(|j| lw[0] * rm[j][0] + lw[1] * rm[j][1] + lw[2] * rm[j][2]);
        }
        // normal: first 8888 attribute of the second stream (0..255 -> -1..1, Xenos x = low byte)
        let nrm = {
            let at = vi as usize * streams[s1].stride;
            match streams[s1].data.get(at..at + 4) {
                Some(b) => {
                    let w0 = u32::from_be_bytes([b[0], b[1], b[2], b[3]]);
                    [0, 1, 2].map(|i| ((w0 >> (8 * i)) & 0xFF) as f64 * 2.0 / 255.0 - 1.0)
                }
                None => [0.0, 0.0, 1.0],
            }
        };
        let slot = match vs_kind {
            Kind::Branch => 0.0,
            Kind::LeafCard => 1.0,
            _ => 3.0,
        };
        let n = (cap.verts.len() / STRIDE) as u32;
        cap.verts.extend_from_slice(&[
            local[0] as f32, local[1] as f32, local[2] as f32,
            o1[0] as f32, o1[1] as f32,
            card[0] as f32, card[1] as f32, card[2] as f32,
            nrm[0] as f32, nrm[1] as f32, nrm[2] as f32,
            slot, vi as f32,
        ]);
        cap.vmap.insert(key, n);
        cap.indices.push(n);
    }
    let count = cap.indices.len() as u32 - first;
    cap.draws.push(CapDraw { vs_kind, vb: j.vb, min_index: j.min_index, tex: j.tex.clone(), first, count });
}

/// Ends one asset's capture: classifies fronds and writes the geometry cache. False when nothing was built.
fn cap_commit(db: &AssetDb, path: &str, comp_path: &str, cap: Cap, log: bool) -> bool {
    if cap.draws.is_empty() || cap.verts.is_empty() {
        return false;
    }
    let leaf_tex: HashSet<u32> = cap.draws.iter().filter(|d| d.vs_kind == Kind::LeafCard).flat_map(|d| d.tex.iter().copied()).collect();
    let mut draws = Vec::new();
    for d in &cap.draws {
        let mut kind = d.vs_kind;
        if kind == Kind::Branch {
            // Branches and fronds share one vertex buffer, branches first in it. With one draw only, the
            // frond is the one that samples the leaf texture.
            let same: Vec<&CapDraw> = cap.draws.iter().filter(|e| e.vs_kind == Kind::Branch && e.vb == d.vb).collect();
            let mins: HashSet<u32> = same.iter().map(|e| e.min_index).collect();
            if mins.len() > 1 {
                if d.min_index > same.iter().map(|e| e.min_index).min().unwrap_or(0) {
                    kind = Kind::Frond;
                }
            } else if !leaf_tex.is_empty() && d.tex.iter().any(|t| leaf_tex.contains(t)) {
                kind = Kind::Frond;
            }
        }
        draws.push(GDraw { kind, first: d.first, count: d.count });
    }
    let geom = Geometry { vertices: cap.verts, indices: cap.indices, draws };
    let file = cache_file(db, path);
    if let Some(dir) = file.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    if let Err(e) = std::fs::write(&file, geom.encode()) {
        eprintln!("n1ffi: write {}: {e}", file.display());
    }
    if log {
        eprintln!(
            "n1ffi: captured {path}: {} vertices, {} triangles, {} draws (from {})",
            geom.vertices.len() / STRIDE,
            geom.indices.len() / 3,
            geom.draws.len(),
            comp_path
        );
    }
    true
}

// ------------------------------------------------------------------------------------------------ state

enum CompSt {
    Idle,
    Loading,
    Ready { base: [i32; 4], scale: [[f32; 4]; 12] },
    Failed,
}

enum AssetSt {
    Idle,
    Loading,
    NeedCapture,
    Ready(u32),
    Failed,
}

struct AInfo {
    center: [f32; 3],
    radius: f32,
    /// The renderer's asset id.
    gpu: u32,
}

/// What one capture session records for an asset (on the loader thread): the geometry built so far.
#[derive(Default)]
struct Cap {
    verts: Vec<f32>,
    vmap: HashMap<(u8, u32, u32), u32>,
    indices: Vec<u32>,
    draws: Vec<CapDraw>,
}

struct CapDraw {
    vs_kind: Kind,
    vb: u32,
    min_index: u32,
    tex: Vec<u32>,
    first: u32,
    count: u32,
}

#[derive(PartialEq, Eq, Clone, Copy, Debug)]
enum Phase {
    Idle,
    /// Counting each placement's indices, to follow the most detailed one.
    Survey,
    /// Running the vertex shader for the chosen placements.
    Compute,
}

pub struct TreeSystem {
    tx: mpsc::Sender<Job>,
    rx: mpsc::Receiver<Done>,
    pub settings: TreeSettings,
    pub enabled: bool,
    pub log: bool,
    comp_paths: Vec<String>,
    comp_by_path: HashMap<String, u32>,
    comp: Vec<CompSt>,
    asset_paths: Vec<String>,
    asset_by_path: HashMap<String, u32>,
    asset: Vec<AssetSt>,
    info: HashMap<u32, AInfo>,
    raw: Vec<N1Tree>,
    pub last: TStats,
    // capture
    phase: Phase,
    frames_left: u32,
    survey_frames: u32,
    cooldown: u32,
    /// Sessions in a row that captured nothing (B-199): each doubles the cooldown before the next one.
    empty_sessions: u32,
    /// Frames the game drew its own scene for capture sessions, this run.
    pub capture_frames: u64,
    stall: u32,
    saw: bool,
    dumped: HashSet<u64>,
    dbg: std::collections::BTreeMap<&'static str, u32>,
    tries: HashMap<u32, u8>,
    /// Sessions in which an asset looked visible but the game drew none of its placements (B-199).
    misses: HashMap<u32, u8>,
    /// Assets that looked visible during the running session.
    session_wanted: HashSet<u32>,
    vs_kinds: HashMap<u64, Kind>,
    progs: HashMap<u64, Option<Arc<Program>>>,
    grid: HashMap<(i32, i32, i32), Vec<usize>, CellHash>,
    /// B-260: `find_placement` visits only the cells a match can be in (BOTS_N1_TREE_FASTMATCH=0: all 27 around).
    fast_match: bool,
    /// Time in `probe` this run, ns (BOTS_N1_TREE_LOG only).
    probe_ns: u64,
    counts: HashMap<u32, (u32, u64)>,
    target: HashMap<u32, u32>,
    /// B-220: draws handed to the loader thread this session (dedup), and the assets they were for (-> component).
    cap_seen: HashSet<(u32, (u32, u32, u32, u32, u32))>,
    sent: HashMap<u32, u32>,
    /// Assets whose capture was committed and is still being built or loaded.
    capturing: HashSet<u32>,
    /// BOTS_N1_TREE_ASYNC=0 (A/B): build the geometry in `capture` as before B-220, into this map.
    sync_caps: Option<HashMap<u32, Cap>>,
    unknown_logged: HashSet<u64>,
    pub captured: u32,
    pub capture_sessions: u32,
    /// B-199: loads first asked for by a draw (not preloaded), and preload requests.
    pub lazy: u64,
    pub preloads: u64,
}

fn env_num<T: std::str::FromStr>(name: &str, default: T) -> T {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

impl TreeSystem {
    pub fn new(db: Arc<AssetDb>) -> TreeSystem {
        let (tx, jobs) = mpsc::channel::<Job>();
        let (done_tx, rx) = mpsc::channel::<Done>();
        let d = db;
        std::thread::Builder::new()
            .name("n1-trees".into())
            .spawn(move || {
                // B-220: the capture sessions' geometry, built here from CapDraw jobs (in order) until CapCommit.
                let mut caps: HashMap<u32, Cap> = HashMap::new();
                let log = std::env::var("BOTS_N1_TREE_LOG").is_ok();
                while let Ok(job) = jobs.recv() {
                    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| match job {
                        Job::Light(h, p) => vec![Done::Light(h, d.tree_light(&p))],
                        Job::Asset(h, p) => vec![Done::Asset(h, load_asset(&d, &p))],
                        Job::CapDraw(j) => {
                            cap_compute(&mut caps, &j);
                            Vec::new()
                        }
                        Job::CapCommit(list, built) => {
                            caps.extend(built);
                            let done = list
                                .into_iter()
                                .map(|(asset, path, comp_path)| {
                                    let ok = caps.remove(&asset).is_some_and(|cap| cap_commit(&d, &path, &comp_path, cap, log));
                                    // Not captured after all (the shader run failed): back to needing a capture.
                                    Done::Asset(asset, if ok { load_asset(&d, &path) } else { AssetResult::NeedsCapture })
                                })
                                .collect();
                            // The session is over: geometry of assets not committed is never reused.
                            caps.clear();
                            done
                        }
                    }));
                    match r {
                        Ok(done) => {
                            if done.into_iter().any(|x| done_tx.send(x).is_err()) {
                                break;
                            }
                        }
                        Err(_) => eprintln!("n1ffi: tree loader panicked"),
                    }
                }
            })
            .expect("tree loader thread");
        let spec = std::env::var("BOTS_N1_TREES").unwrap_or_default();
        let settings = TreeSettings::parse(&spec).unwrap_or_else(|| {
            eprintln!("n1ffi: BOTS_N1_TREES={spec:?} not understood (off|low|medium|high[,dist=N,leaves=F,nocull]); using high");
            TreeSettings::default()
        });
        let enabled = settings.max_distance > 0.0;
        // Colour-pass vertex shaders of the three geometry kinds (mission 6); BOTS_N1_TREE_VS=hash:kind,... adds more
        // (kind: branch, leafcard, leafmesh).
        let mut vs_kinds = HashMap::from([
            (0x3D0B_A91C_92C0_8C8Fu64, Kind::Branch),
            (0x7A6F_D5B1_E3CC_363Fu64, Kind::LeafCard),
            (0x3E0A_1929_5CBB_862Cu64, Kind::LeafMesh),
        ]);
        if let Ok(extra) = std::env::var("BOTS_N1_TREE_VS") {
            for kv in extra.split(',') {
                let Some((h, k)) = kv.split_once(':') else { continue };
                let kind = match k {
                    "branch" => Kind::Branch,
                    "leafcard" => Kind::LeafCard,
                    "leafmesh" => Kind::LeafMesh,
                    _ => continue,
                };
                if let Ok(h) = u64::from_str_radix(h.trim_start_matches("0x"), 16) {
                    vs_kinds.insert(h, kind);
                }
            }
        }
        TreeSystem {
            tx,
            rx,
            settings,
            enabled,
            log: std::env::var("BOTS_N1_TREE_LOG").is_ok(),
            comp_paths: Vec::new(),
            comp_by_path: HashMap::new(),
            comp: Vec::new(),
            asset_paths: Vec::new(),
            asset_by_path: HashMap::new(),
            asset: Vec::new(),
            info: HashMap::new(),
            raw: Vec::new(),
            last: TStats::default(),
            phase: Phase::Idle,
            frames_left: 0,
            survey_frames: env_num("BOTS_N1_TREE_SURVEY", 6u32),
            cooldown: 0,
            empty_sessions: 0,
            capture_frames: 0,
            stall: 0,
            saw: false,
            dumped: HashSet::new(),
            dbg: Default::default(),
            tries: HashMap::new(),
            misses: HashMap::new(),
            session_wanted: HashSet::new(),
            vs_kinds,
            progs: HashMap::new(),
            grid: HashMap::default(),
            probe_ns: 0,
            fast_match: env_num("BOTS_N1_TREE_FASTMATCH", 1u32) != 0,
            counts: HashMap::new(),
            target: HashMap::new(),
            cap_seen: HashSet::new(),
            sent: HashMap::new(),
            capturing: HashSet::new(),
            sync_caps: (env_num("BOTS_N1_TREE_ASYNC", 1u32) == 0).then(HashMap::new),
            unknown_logged: HashSet::new(),
            captured: 0,
            capture_sessions: 0,
            lazy: 0,
            preloads: 0,
        }
    }

    pub fn component_handle(&mut self, path: &str) -> u32 {
        if let Some(&h) = self.comp_by_path.get(path) {
            return h;
        }
        let h = self.comp_paths.len() as u32;
        self.comp_paths.push(path.to_string());
        self.comp_by_path.insert(path.to_string(), h);
        self.comp.push(CompSt::Idle);
        h
    }

    pub fn asset_handle(&mut self, path: &str) -> u32 {
        if let Some(&h) = self.asset_by_path.get(path) {
            return h;
        }
        let h = self.asset_paths.len() as u32;
        self.asset_paths.push(path.to_string());
        self.asset_by_path.insert(path.to_string(), h);
        self.asset.push(AssetSt::Idle);
        h
    }

    /// Start loading a tree asset before any placement needs it (B-199 level preload).
    pub fn preload_asset(&mut self, path: &str) -> bool {
        let h = self.asset_handle(path);
        if !matches!(self.asset[h as usize], AssetSt::Idle) {
            return false;
        }
        self.asset[h as usize] = AssetSt::Loading;
        self.preloads += 1;
        let _ = self.tx.send(Job::Asset(h, path.to_string()));
        true
    }

    /// Start loading a tree component's baked light before it is drawn.
    pub fn preload_component(&mut self, path: &str) -> bool {
        let h = self.component_handle(path);
        if !matches!(self.comp[h as usize], CompSt::Idle) {
            return false;
        }
        self.comp[h as usize] = CompSt::Loading;
        self.preloads += 1;
        let _ = self.tx.send(Job::Light(h, path.to_string()));
        true
    }

    /// The next frame's placements (copied).
    pub fn set_frame(&mut self, trees: &[N1Tree]) {
        self.raw.clear();
        self.raw.extend_from_slice(trees);
    }

    /// Applies finished loads outside `prepare` (upload pump on frames without a scene).
    pub fn pump(&mut self, r: &mut Renderer) {
        self.take_done(r);
    }

    fn take_done(&mut self, r: &mut Renderer) {
        while let Ok(d) = self.rx.try_recv() {
            match d {
                Done::Light(h, l) => {
                    let st = match l {
                        Some(l) => {
                            let mut base = [-1i32; 4];
                            let mut scale = [[0f32; 4]; 12];
                            for (slot, s) in l.slots.iter().enumerate() {
                                let Some((words, sc)) = s else { continue };
                                base[slot] = r.tree_light_words(words) as i32;
                                for k in 0..3 {
                                    scale[slot * 3 + k] = [sc[k][0], sc[k][1], sc[k][2], 0.0];
                                }
                            }
                            CompSt::Ready { base, scale }
                        }
                        None => CompSt::Failed,
                    };
                    if let Some(c) = self.comp.get_mut(h as usize) {
                        *c = st;
                    }
                }
                Done::Asset(h, AssetResult::Ready(t)) => {
                    let (center, radius) = t.geom.bounds();
                    match r.tree_add(&t) {
                        Ok(gpu) => {
                            self.info.insert(h, AInfo { center, radius, gpu });
                            self.asset[h as usize] = AssetSt::Ready(gpu);
                        }
                        Err(e) => {
                            eprintln!("n1ffi: tree {}: {e:#}", self.asset_paths[h as usize]);
                            self.asset[h as usize] = AssetSt::Failed;
                        }
                    }
                }
                Done::Asset(h, AssetResult::NeedsCapture) => {
                    // A committed capture that built nothing uses up one of the asset's tries.
                    if self.capturing.remove(&h) {
                        *self.tries.entry(h).or_insert(0) += 1;
                    }
                    self.asset[h as usize] = AssetSt::NeedCapture
                }
                Done::Asset(h, AssetResult::Failed(e)) => {
                    if self.log {
                        eprintln!("n1ffi: tree {}: {e}", self.asset_paths[h as usize]);
                    }
                    self.asset[h as usize] = AssetSt::Failed;
                }
            }
        }
    }

    /// Per frame, before `Renderer::record`: takes finished loads, culls, and builds the instance list.
    pub fn prepare(&mut self, r: &mut Renderer, view_proj: &[f32; 16], eye: [f32; 3]) -> TFrame {
        self.take_done(r);
        let mut st = TStats { placements: self.raw.len() as u32, ..Default::default() };
        for a in &self.asset {
            match a {
                AssetSt::Ready(_) => st.assets_ready += 1,
                AssetSt::Loading => st.assets_loading += 1,
                AssetSt::NeedCapture => st.assets_uncaptured += 1,
                AssetSt::Failed => st.assets_failed += 1,
                AssetSt::Idle => {}
            }
        }
        let mut out = TFrame { leaf_density: self.settings.leaf_density, ..Default::default() };
        if !self.enabled || self.raw.is_empty() {
            self.last = st;
            return out;
        }
        let planes = scenekit::frustum_planes(view_proj);
        let mut wanted: HashSet<u32> = HashSet::new();
        let mut wanted_visible: HashSet<u32> = HashSet::new();
        let cap_dist = env_num("BOTS_N1_TREE_CAPTURE_DIST", 15_000f32);
        let cap_radius = env_num("BOTS_N1_TREE_CAPTURE_RADIUS", 2_000f32);
        let mut by_asset: Vec<(u32, Vec<TInst>)> = Vec::new();
        let mut slot_of: HashMap<u32, usize> = HashMap::new();
        for i in 0..self.raw.len() {
            let t = self.raw[i];
            let (Some(c), Some(a)) = (self.comp.get_mut(t.component as usize), self.asset.get_mut(t.asset as usize)) else {
                continue;
            };
            if matches!(c, CompSt::Idle) {
                *c = CompSt::Loading;
                self.lazy += 1;
                scenekit::lazy_log("tree component", &self.comp_paths[t.component as usize]);
                let _ = self.tx.send(Job::Light(t.component, self.comp_paths[t.component as usize].clone()));
            }
            if matches!(a, AssetSt::Idle) {
                *a = AssetSt::Loading;
                self.lazy += 1;
                scenekit::lazy_log("tree asset", &self.asset_paths[t.asset as usize]);
                let _ = self.tx.send(Job::Asset(t.asset, self.asset_paths[t.asset as usize].clone()));
            }
            if matches!(a, AssetSt::NeedCapture)
                && self.tries.get(&t.asset).copied().unwrap_or(0) < 3
                && self.misses.get(&t.asset).copied().unwrap_or(0) < 8
            {
                wanted.insert(t.asset);
                // Geometry (so its bounds) is unknown until captured: the placement's origin and a generous radius.
                let p = [t.transform[12], t.transform[13], t.transform[14]];
                let d = ((p[0] - eye[0]).powi(2) + (p[1] - eye[1]).powi(2) + (p[2] - eye[2]).powi(2)).sqrt();
                if d - cap_radius <= cap_dist && !planes.iter().any(|q| q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3] < -cap_radius) {
                    wanted_visible.insert(t.asset);
                }
            }
            let (AssetSt::Ready(_), CompSt::Ready { base, scale }) = (&*a, &*c) else { continue };
            let Some(info) = self.info.get(&t.asset) else { continue };
            let f = full_transform(&t);
            let scale_len = ((t.transform[0] as f64).powi(2) + (t.transform[1] as f64).powi(2) + (t.transform[2] as f64).powi(2)).sqrt() as f32;
            let ctr = info.center;
            let wc = [0, 1, 2].map(|j| (ctr[0] as f64 * f[0][j] + ctr[1] as f64 * f[1][j] + ctr[2] as f64 * f[2][j] + f[3][j]) as f32);
            let radius = info.radius * scale_len;
            let dist = ((wc[0] - eye[0]).powi(2) + (wc[1] - eye[1]).powi(2) + (wc[2] - eye[2]).powi(2)).sqrt();
            if dist - radius > self.settings.max_distance {
                continue;
            }
            if self.settings.frustum_cull && planes.iter().any(|p| p[0] * wc[0] + p[1] * wc[1] + p[2] * wc[2] + p[3] < -radius) {
                continue;
            }
            let inst = TInst { model: swz_model(&f), misc: [scale_len, 0.0, 0.0, 0.0], base: *base, sc: *scale };
            let k = *slot_of.entry(info.gpu).or_insert_with(|| {
                by_asset.push((info.gpu, Vec::new()));
                by_asset.len() - 1
            });
            by_asset[k].1.push(inst);
        }
        for (gpu, v) in by_asset {
            out.calls.push(TCall { asset: gpu, first: out.insts.len() as u32, count: v.len() as u32 });
            out.insts.extend(v);
        }
        st.instances = out.insts.len() as u32;
        st.draws = out.calls.len() as u32;
        st.wanted = wanted.len() as u32;
        st.wanted_visible = wanted_visible.len() as u32;
        if self.phase != Phase::Idle {
            self.session_wanted.extend(wanted_visible);
        }
        self.last = st;
        out
    }

    // -------------------------------------------------------------------------------------------- capture

    /// Called once per rendered frame (from the host's scene draw): advances a capture session and commits it when it
    /// ends. Returns the number of frames the host should still let the game draw its own scene for (the port normally
    /// suppresses it), 0 when no session is running. A frame counts towards the session only if the capture hook saw
    /// one of the wanted trees in it.
    pub fn end_frame(&mut self) -> u32 {
        let total = env_num("BOTS_N1_TREE_CAPTURE_FRAMES", 30u32).max(self.survey_frames + 2);
        if self.phase == Phase::Idle {
            if self.cooldown > 0 {
                self.cooldown -= 1;
                return 0;
            }
            // B-199: only when an uncaptured tree is probably on screen. Starting whenever any placement in the level
            // needed one let the game draw its own scene for up to ~400 frames per session with nothing to capture.
            if !self.enabled || self.last.wanted_visible == 0 || env_num("BOTS_N1_TREE_CAPTURE", 1u32) == 0 {
                return 0;
            }
            self.phase = Phase::Survey;
            self.frames_left = total;
            self.stall = 0;
            self.saw = false;
            self.counts.clear();
            self.target.clear();
            self.cap_seen.clear();
            self.sent.clear();
            self.session_wanted.clear();
            self.capture_sessions += 1;
            if self.log {
                eprintln!(
                    "n1ffi: tree capture session {} starts ({} assets wanted, {} in view)",
                    self.capture_sessions, self.last.wanted, self.last.wanted_visible
                );
            }
            return total;
        }
        self.capture_frames += 1;
        if self.saw {
            self.saw = false;
            self.stall = 0;
            self.frames_left = self.frames_left.saturating_sub(1);
        } else {
            self.stall += 1;
        }
        if self.phase == Phase::Survey && self.frames_left <= total - self.survey_frames {
            self.choose_targets();
            self.phase = Phase::Compute;
        }
        // B-199: a survey that has not seen a wanted tree drawn after a few frames gives up (BOTS_N1_TREE_SURVEY_STALL).
        // The in-view test is generous (origin plus a radius), so many sessions find nothing; each ran the full stall
        // limit with the game drawing its own scene, 35-200 ms a frame on a low-end laptop (mission 5: ~900 frames over 33 ms).
        let survey_empty = self.phase == Phase::Survey && self.counts.is_empty() && self.stall > env_num("BOTS_N1_TREE_SURVEY_STALL", 8u32);
        if self.frames_left == 0 || survey_empty || self.stall > env_num("BOTS_N1_TREE_STALL", 120u32) {
            if self.phase == Phase::Survey {
                self.choose_targets();
            }
            let before = self.captured;
            self.commit();
            self.phase = Phase::Idle;
            self.empty_sessions = if self.captured > before { 0 } else { (self.empty_sessions + 1).min(4) };
            self.cooldown = 120 << self.empty_sessions;
            return 0;
        }
        self.frames_left
    }

    fn choose_targets(&mut self) {
        // For each wanted asset, the placement that drew the most indices over the survey frames (most detailed LOD).
        let mut best: HashMap<u32, (u64, u32)> = HashMap::new();
        for (&comp, &(asset, n)) in &self.counts {
            let e = best.entry(asset).or_insert((0, comp));
            if n > e.0 {
                *e = (n, comp);
            }
        }
        self.target = best.into_iter().map(|(a, (_, c))| (a, c)).collect();
        if self.log {
            eprintln!("n1ffi: tree survey saw {} assets", self.target.len());
        }
    }

    fn rebuild_grid(&mut self) {
        self.grid.clear();
        for (i, t) in self.raw.iter().enumerate() {
            let k = (t.transform[12].floor() as i32, t.transform[13].floor() as i32, t.transform[14].floor() as i32);
            self.grid.entry(k).or_default().push(i);
        }
    }

    /// The placement whose translation equals a `w == 1` row of the constants (row 4 excluded), and that row.
    fn find_placement(&self, consts: &[f32]) -> Option<(usize, usize)> {
        find_in_grid(&self.grid, &self.raw, consts, self.fast_match)
    }

    /// Does the host need to hand this draw over? Bit 0: capture it. Bit 1: the vertex shader's text is needed first
    /// (`ucode`). Counts the survey as a side effect.
    pub fn probe(&mut self, vs_hash: u64, consts: &[f32], index_count: u32) -> u32 {
        if !self.log || self.phase == Phase::Idle {
            return self.probe_draw(vs_hash, consts, index_count);
        }
        let t0 = std::time::Instant::now();
        let r = self.probe_draw(vs_hash, consts, index_count);
        *self.dbg.entry("probe_calls").or_default() += 1;
        self.probe_ns += t0.elapsed().as_nanos() as u64;
        r
    }

    fn probe_draw(&mut self, vs_hash: u64, consts: &[f32], index_count: u32) -> u32 {
        if self.phase == Phase::Idle || consts.len() < 1024 {
            return 0;
        }
        if self.grid.is_empty() {
            self.rebuild_grid();
        }
        let Some((pi, _)) = self.find_placement(consts) else { return 0 };
        let t = self.raw[pi];
        if !matches!(self.asset.get(t.asset as usize), Some(AssetSt::NeedCapture)) {
            return 0;
        }
        if !self.vs_kinds.contains_key(&vs_hash) {
            // B-199: the capture cannot use a draw whose shader kind it does not know, so give the asset up for this run
            // (all 3 tries). Before, sessions kept starting for it and each ran ~400 frames of the game's own scene with
            // nothing to capture (mission 6's ExoticPlant01: 20+ hitches over 33 ms per run on a low-end laptop).
            self.tries.insert(t.asset, 3);
            if self.unknown_logged.insert(vs_hash) {
                eprintln!(
                    "n1ffi: a draw of tree {} uses vertex shader {vs_hash:016X}, which is not a known SpeedTree shader (BOTS_N1_TREE_VS=hash:branch|leafcard|leafmesh adds one)",
                    self.asset_paths[t.asset as usize]
                );
            }
            return if std::env::var_os("BOTS_N1_TREE_VSDUMP").is_some() && self.dumped.insert(vs_hash) { 2 } else { 0 };
        }
        self.saw = true;
        match self.phase {
            Phase::Survey => {
                let e = self.counts.entry(t.component).or_insert((t.asset, 0));
                e.1 += index_count as u64;
                0
            }
            Phase::Compute => {
                if self.target.get(&t.asset) != Some(&t.component) {
                    return 0;
                }
                *self.dbg.entry("probe_want").or_default() += 1;
                1 | if matches!(self.progs.get(&vs_hash), Some(Some(_))) { 0 } else { 2 }
            }
            Phase::Idle => 0,
        }
    }

    /// The vertex shader's disassembly text for a hash.
    pub fn set_ucode(&mut self, vs_hash: u64, text: &str) {
        if !self.vs_kinds.contains_key(&vs_hash) {
            if let Some(d) = std::env::var_os("BOTS_N1_TREE_VSDUMP") {
                let _ = std::fs::write(std::path::Path::new(&d).join(format!("vs_{vs_hash:016X}.txt")), text);
            }
            return;
        }
        let p = Program::parse(text);
        if let Err(e) = &p {
            eprintln!("n1ffi: SpeedTree vertex shader {vs_hash:016X}: {e:#}");
        }
        self.progs.insert(vs_hash, p.ok().map(Arc::new));
    }

    /// Checks a draw of a capture target and hands a copy of it to the loader thread, which runs the game's vertex
    /// shader over it (`cap_compute`, B-220).
    ///
    /// # Safety
    /// The pointers in `d` are valid for their stated sizes.
    pub unsafe fn capture(&mut self, d: &N1TreeDraw) {
        if self.phase != Phase::Compute || d.consts.is_null() || d.indices.is_null() {
            return;
        }
        let consts = unsafe { std::slice::from_raw_parts(d.consts, 1024) };
        let indices = unsafe { std::slice::from_raw_parts(d.indices, d.index_count as usize) };
        *self.dbg.entry("cap_calls").or_default() += 1;
        let Some(&vs_kind) = self.vs_kinds.get(&d.vs_hash) else { return };
        let Some(Some(prog)) = self.progs.get(&d.vs_hash).cloned() else {
            *self.dbg.entry("no_prog").or_default() += 1;
            return;
        };
        let Some((pi, row)) = self.find_placement(consts) else {
            *self.dbg.entry("no_place").or_default() += 1;
            return;
        };
        let t = self.raw[pi];
        if self.target.get(&t.asset) != Some(&t.component) || row < 3 {
            *self.dbg.entry("not_target").or_default() += 1;
            return;
        }
        let min_index = indices.iter().copied().min().unwrap_or(0);
        let key = (d.vs_hash as u32 ^ (d.vs_hash >> 32) as u32, d.stream_addr[0], min_index, d.index_count, indices.first().copied().unwrap_or(0));
        if !self.cap_seen.insert((t.asset, key)) {
            *self.dbg.entry("dup").or_default() += 1;
            return;
        }
        // The shader's vfN is the Nth binding of the translated shader (their fetch constants run 95, 94, ...).
        // Copy only the vertices the indices reach (zeroed allocations elsewhere cost nothing until touched).
        let nst = (d.stream_count as usize).clamp(1, 4);
        let (lo, hi) = indices.iter().fold((u32::MAX, 0u32), |(lo, hi), &i| {
            let v = i.wrapping_add(d.index_offset);
            (lo.min(v), hi.max(v))
        });
        let streams: Vec<(Vec<u8>, usize)> = (0..nst)
            .map(|k| {
                let stride = d.stream_stride[k] as usize;
                if d.stream[k].is_null() || indices.is_empty() {
                    return (Vec::new(), stride);
                }
                let src = unsafe { std::slice::from_raw_parts(d.stream[k], d.stream_bytes[k] as usize) };
                let from = (lo as usize).saturating_mul(stride).min(src.len());
                let to = (hi as usize + 1).saturating_mul(stride).min(src.len());
                let mut data = vec![0u8; to];
                data[from..to].copy_from_slice(&src[from..to]);
                (data, stride)
            })
            .collect();
        let tex: Vec<u32> = d.tex_base[..(d.tex_count as usize).min(4)].iter().map(|w| w & !0xFFF).collect();
        self.sent.entry(t.asset).or_insert(t.component);
        *self.dbg.entry("draws_sent").or_default() += 1;
        let job = CapJob {
            asset: t.asset,
            vs_kind,
            prog,
            consts: consts.to_vec(),
            streams,
            s0: 0,
            s1: nst.min(2) - 1,
            indices: indices.to_vec(),
            index_offset: d.index_offset,
            vb: d.stream_addr[0],
            min_index,
            row,
            rotation: t.rotation,
            tex,
        };
        match &mut self.sync_caps {
            Some(caps) => cap_compute(caps, &job),
            None => {
                let _ = self.tx.send(Job::CapDraw(Box::new(job)));
            }
        }
    }

    /// Ends a session: the loader thread classifies fronds, writes the caches and loads the assets (CapCommit).
    fn commit(&mut self) {
        let sent = std::mem::take(&mut self.sent);
        let had_draws = !sent.is_empty();
        self.cap_seen.clear();
        let mut done = 0;
        let mut list = Vec::new();
        for (asset, comp) in sent {
            if !matches!(self.asset[asset as usize], AssetSt::NeedCapture) {
                continue;
            }
            list.push((asset, self.asset_paths[asset as usize].clone(), self.comp_paths[comp as usize].clone()));
            self.asset[asset as usize] = AssetSt::Loading;
            self.capturing.insert(asset);
            self.captured += 1;
            done += 1;
        }
        if had_draws {
            let built = self.sync_caps.as_mut().map(std::mem::take).unwrap_or_default();
            let _ = self.tx.send(Job::CapCommit(list, built));
        }
        // B-199: an asset the game drew but that did not capture uses up one of its 3 tries; one that looked visible
        // but was never drawn (occluded, LOD-culled) uses up a miss (8). Assets out of view use up nothing: before, every
        // session charged every uncaptured asset a try, so trees not on screen in a level's first 3 sessions were given
        // up on until the next run, and a level's trees took several runs to capture.
        let seen: HashSet<u32> = self.counts.values().map(|&(a, _)| a).collect();
        for (i, a) in self.asset.iter().enumerate() {
            let i = i as u32;
            if !matches!(a, AssetSt::NeedCapture) {
                continue;
            }
            if seen.contains(&i) {
                *self.tries.entry(i).or_insert(0) += 1;
            } else if self.session_wanted.contains(&i) {
                *self.misses.entry(i).or_insert(0) += 1;
            }
        }
        if self.log {
            eprintln!(
                "n1ffi: tree capture session ended: {done} assets sent to capture, {} total, {} capture frames this run; probe {:.2} ms in total; {:?}",
                self.captured, self.capture_frames, self.probe_ns as f64 / 1e6, self.dbg
            );
        }
        self.grid.clear();
        self.progs.retain(|_, p| p.is_some());
    }

    /// Per-asset coverage: (ready, needing capture, failed, total handles seen).
    pub fn coverage(&self) -> (u32, u32, u32, u32) {
        let mut c = (0, 0, 0, self.asset.len() as u32);
        for a in &self.asset {
            match a {
                AssetSt::Ready(_) => c.0 += 1,
                AssetSt::NeedCapture | AssetSt::Idle | AssetSt::Loading => c.1 += 1,
                AssetSt::Failed => c.2 += 1,
            }
        }
        c
    }

    pub fn uncaptured_paths(&self) -> Vec<&str> {
        self.asset.iter().enumerate().filter(|(_, a)| matches!(a, AssetSt::NeedCapture)).map(|(i, _)| self.asset_paths[i].as_str()).collect()
    }

    /// Frames still queued for the loader thread's results (debug).
    pub fn pending_jobs(&self) -> usize {
        self.comp.iter().filter(|c| matches!(c, CompSt::Loading)).count() + self.asset.iter().filter(|a| matches!(a, AssetSt::Loading)).count()
    }
}

/// `TreeSystem::find_placement` over a grid of placement indices keyed by the floor of their translation.
fn find_in_grid(
    grid: &HashMap<(i32, i32, i32), Vec<usize>, CellHash>,
    raw: &[N1Tree],
    consts: &[f32],
    fast: bool,
) -> Option<(usize, usize)> {
    for row in 0..64 {
            if row == 4 || (consts[row * 4 + 3] - 1.0).abs() > 1e-6 {
                continue;
            }
            let p = [consts[row * 4], consts[row * 4 + 1], consts[row * 4 + 2]];
            // A placement matches when it is within 0.5 of the row on every axis, so its cell (its position's floor) is
            // in floor(p - 0.5)..=floor(p + 0.5): two cells an axis, in the order the 27-cell walk visits them. B-260:
            // the walk did 27 hash lookups for each of 64 rows for every draw of a capture frame, 2,400-3,700 draws,
            // about 100-190 ms on the Deck, nearly all of it for draws that are not trees.
            let (lo, hi) = if fast {
                let f = |v: f32, d: f32| (v + d).floor() as i32;
                ([f(p[0], -0.5), f(p[1], -0.5), f(p[2], -0.5)], [f(p[0], 0.5), f(p[1], 0.5), f(p[2], 0.5)])
            } else {
                let k = [p[0].floor() as i32, p[1].floor() as i32, p[2].floor() as i32];
                ([k[0] - 1, k[1] - 1, k[2] - 1], [k[0] + 1, k[1] + 1, k[2] + 1])
            };
            for x in lo[0]..=hi[0] {
                for y in lo[1]..=hi[1] {
                    for z in lo[2]..=hi[2] {
                        let Some(v) = grid.get(&(x, y, z)) else { continue };
                        for &i in v {
                            let t = &raw[i].transform;
                            if (t[12] - p[0]).abs() < 0.5 && (t[13] - p[1]).abs() < 0.5 && (t[14] - p[2]).abs() < 0.5 {
                                return Some((i, row));
                            }
                        }
                    }
                }
            }
        }
        None
    }


fn cell_of(t: &[f32; 16]) -> (i32, i32, i32) {
    (t[12].floor() as i32, t[13].floor() as i32, t[14].floor() as i32)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tree(x: f32, y: f32, z: f32) -> N1Tree {
        let mut transform = [0.0; 16];
        (transform[12], transform[13], transform[14]) = (x, y, z);
        N1Tree { transform, component: 0, asset: 0, rotation: [0; 3], pad: 0 }
    }

    /// B-260: the cell-range walk finds exactly what the 27-cell walk finds, including with placements closer than 0.5.
    #[test]
    fn fast_match_equals_full_walk() {
        let mut seed = 0x1234_5678_u64;
        let mut rnd = move || {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            ((seed >> 33) as f32) / (1u64 << 31) as f32
        };
        let raw: Vec<N1Tree> = (0..400).map(|_| tree(rnd() * 60.0 - 30.0, rnd() * 60.0 - 30.0, rnd() * 8.0)).collect();
        let mut grid: HashMap<(i32, i32, i32), Vec<usize>, CellHash> = HashMap::default();
        for (i, t) in raw.iter().enumerate() {
            grid.entry(cell_of(&t.transform)).or_default().push(i);
        }
        let mut hits = 0;
        for n in 0..3000 {
            let mut c = vec![0.0f32; 1024];
            for row in 0..64 {
                let (x, y, z) = (rnd() * 60.0 - 30.0, rnd() * 60.0 - 30.0, rnd() * 8.0);
                c[row * 4..row * 4 + 4].copy_from_slice(&[x, y, z, 1.0]);
            }
            if n % 3 == 0 {
                // a row near a real placement, at offsets across the 0.5 limit
                let t = &raw[(rnd() * 400.0) as usize % 400].transform;
                let off = |r: f32| (r - 0.5) * 1.2;
                c[20 * 4..20 * 4 + 4].copy_from_slice(&[t[12] + off(rnd()), t[13] + off(rnd()), t[14] + off(rnd()), 1.0]);
            }
            let (a, b) = (find_in_grid(&grid, &raw, &c, true), find_in_grid(&grid, &raw, &c, false));
            assert_eq!(a, b);
            hits += usize::from(a.is_some());
        }
        assert!(hits > 100, "the test must hit sometimes: {hits}");
        // Cost of a draw that matches nothing (a non-tree draw), 64 rows with w == 1.
        let mut c = vec![0.0f32; 1024];
        for row in 0..64 {
            c[row * 4..row * 4 + 4].copy_from_slice(&[500.0 + row as f32, 0.0, 0.0, 1.0]);
        }
        for fast in [false, true] {
            let t0 = std::time::Instant::now();
            for _ in 0..2000 {
                assert!(std::hint::black_box(find_in_grid(&grid, &raw, std::hint::black_box(&c), fast)).is_none());
            }
            eprintln!("find_in_grid fast={fast}: {:.2} us per draw", t0.elapsed().as_secs_f64() * 1e6 / 2000.0);
        }
    }
}
