//! B-228 decals. UE3 draws a DecalComponent as geometry clipped from the primitives it lands on (its receivers): the
//! static DecalActors (posters, graffiti, oil, tyre marks, sand) and the DecalManager pools' run-time splats (explosion
//! scorch, bullet impacts). The host sends each attached decal with its box (DecalComponent.Planes), frame and size and
//! the frame's instances it lands on (DecalReceivers). For each (decal, receiver) n1 clips the receiver's triangles
//! against the box on the CPU, once: the result is a small world-space mesh drawn as an extra instance with the decal's
//! material, cached until the decal or the receiver changes and given back when nothing names it for a while.
//!
//! The decal's texture coordinates come from its frame (u along -HitTangent, v along -HitBinormal, 0.5 at
//! HitLocation; BOTS_N1_DECAL_UV=+u,+v flips either); its light from the receiver (the same lightmap through the
//! interpolated UV1, or interpolated vertex light words). The geometry sits BOTS_N1_DECAL_BIAS (0.5) units out along
//! HitNormal, as n1 has no depth-bias pipelines. Builds stop for the frame after BOTS_N1_DECAL_MS (1.0) ms of work (at
//! least one per frame). BOTS_N1_DECALS=0 turns decals off.

use crate::vkr::{DecalLightSource, Renderer};
use assetdb::AssetDb;
use scenekit::{Instance, MeshDesc, NONE, Resolver, SKIP, SectionDesc, SlotState};
use std::collections::HashMap;
use std::hash::{Hash, Hasher};
use std::time::{Duration, Instant};

/// `N1Decal` (n1ffi.h). 192 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct N1Decal {
    pub material: u32,
    pub id: u32,
    pub receiver_offset: u32,
    pub receiver_count: u32,
    pub planes: [[f32; 4]; 6],
    pub location: [f32; 3],
    pub width: f32,
    pub tangent: [f32; 3],
    pub height: f32,
    pub binormal: [f32; 3],
    pub backface_angle: f32,
    pub normal: [f32; 3],
    pub flags: u32,
    pub tile: [f32; 2],
    pub offset: [f32; 2],
}
const _: () = assert!(std::mem::size_of::<N1Decal>() == 192);

pub const FLAG_BACKFACES: u32 = 1;

struct Entry {
    mesh: u32,
    light: u32,
    own_light: bool,
    last: u64,
}

#[derive(Default, Clone, Copy)]
pub struct DecalStats {
    pub decals: u32,
    pub drawn: u32,
    pub builds: u64,
    pub triangles: u64,
    pub cached: u32,
    pub build_ms: f32,
}

pub struct Decals {
    on: bool,
    frame: Vec<N1Decal>,
    receivers: Vec<u32>,
    cache: HashMap<u64, Entry>,
    clock: u64,
    budget: Duration,
    bias: f32,
    su: f32,
    sv: f32,
    log: bool,
    pub stats: DecalStats,
}

impl Default for Decals {
    fn default() -> Self {
        Self::new()
    }
}

fn env_f32(name: &str, default: f32) -> f32 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

impl Decals {
    pub fn new() -> Self {
        let uv = std::env::var("BOTS_N1_DECAL_UV").unwrap_or_default();
        Self {
            on: std::env::var("BOTS_N1_DECALS").map(|v| v != "0").unwrap_or(true),
            frame: Vec::new(),
            receivers: Vec::new(),
            cache: HashMap::new(),
            clock: 0,
            budget: Duration::from_micros((env_f32("BOTS_N1_DECAL_MS", 1.0).max(0.0) * 1000.0) as u64),
            bias: env_f32("BOTS_N1_DECAL_BIAS", 0.5),
            su: if uv.contains("+u") { 1.0 } else { -1.0 },
            sv: if uv.contains("+v") { 1.0 } else { -1.0 },
            log: std::env::var("BOTS_N1_DECAL_LOG").is_ok_and(|v| v != "0"),
            stats: DecalStats::default(),
        }
    }

    /// The next frame's decals (copied, used once).
    pub fn set_frame(&mut self, decals: &[N1Decal], receivers: &[u32]) {
        self.frame.clear();
        self.receivers.clear();
        if self.on {
            self.frame.extend_from_slice(decals);
            self.receivers.extend_from_slice(receivers);
        }
    }

    /// Appends this frame's decal draws to the renderer-ready instances (`out_i`, `out_o`: `Resolver::translate`'s
    /// output, copied). `host` is the frame's instances as the host gave them (handles), which the receiver indices
    /// name.
    pub fn append(
        &mut self,
        r: &mut Renderer,
        res: &mut Resolver,
        db: &AssetDb,
        host: &[Instance],
        out_i: &mut Vec<Instance>,
        out_o: &mut Vec<u32>,
    ) {
        self.clock += 1;
        let t0 = Instant::now();
        let deadline = t0 + self.budget;
        let mut builds = 0u32;
        let mut drawn = 0u32;
        let frame = std::mem::take(&mut self.frame);
        for d in &frame {
            // Six world-space planes with unit normals, or the feed read something else: an invalid box would let
            // the whole receiver through and tile the decal across it.
            if !planes_valid(&d.planes) {
                continue;
            }
            let mat = res.material_id(r, d.material);
            if mat == NONE || mat == SKIP {
                continue;
            }
            let lo = d.receiver_offset as usize;
            let hi = (lo + d.receiver_count as usize).min(self.receivers.len());
            for k in lo..hi {
                let Some(inst) = host.get(self.receivers[k] as usize) else { continue };
                let Some(SlotState::Ready(_)) = res.mesh_state(inst.mesh) else { continue };
                let light = if inst.light == NONE {
                    NONE
                } else {
                    match res.light_state(inst.light) {
                        Some(SlotState::Ready(id)) => id,
                        Some(SlotState::Pending) => continue, // build once, with the light
                        _ => NONE,
                    }
                };
                let key = key_of(d, inst, light);
                if let Some(e) = self.cache.get_mut(&key) {
                    e.last = self.clock;
                } else {
                    if builds > 0 && Instant::now() >= deadline {
                        continue;
                    }
                    builds += 1;
                    let (e, n) = self.build(r, res, db, d, inst, light);
                    self.stats.builds += 1;
                    self.stats.triangles += n as u64;
                    self.cache.insert(key, e);
                }
                let e = &self.cache[&key];
                if e.mesh == NONE {
                    continue;
                }
                let mut transform = [0f32; 16];
                transform[0] = 1.0;
                transform[5] = 1.0;
                transform[10] = 1.0;
                transform[15] = 1.0;
                out_i.push(Instance {
                    transform,
                    mesh: e.mesh,
                    light: e.light,
                    mat_offset: out_o.len() as u32,
                    mat_count: 1,
                    env: inst.env,
                });
                out_o.push(mat);
                drawn += 1;
            }
        }
        let count = frame.len() as u32;
        self.frame = frame;
        self.frame.clear();
        // What nothing named for ~2 s goes back (the splat pools reuse their components for new decals).
        if self.clock.is_multiple_of(30) {
            let clock = self.clock;
            let mut gone = Vec::new();
            self.cache.retain(|_, e| {
                let keep = e.last + 120 >= clock;
                if !keep && e.mesh != NONE {
                    gone.push((e.mesh, if e.own_light { e.light } else { NONE }));
                }
                keep
            });
            for (m, l) in gone {
                r.free_decal(m, l);
            }
        }
        self.stats.decals = count;
        self.stats.drawn = drawn;
        self.stats.cached = self.cache.len() as u32;
        self.stats.build_ms = t0.elapsed().as_secs_f32() * 1e3;
    }

    fn build(
        &self,
        r: &mut Renderer,
        res: &Resolver,
        db: &AssetDb,
        d: &N1Decal,
        inst: &Instance,
        light: u32,
    ) -> (Entry, usize) {
        let empty = Entry { mesh: NONE, light: NONE, own_light: false, last: self.clock };
        let Some(m) = res.mesh_path(inst.mesh).and_then(|p| db.mesh(p)) else {
            return (empty, 0);
        };
        let src = if light == NONE {
            DecalLightSource::None
        } else {
            r.decal_light_source(light, m.positions.len() as u32)
        };
        let (words_src, sc, map) = match src {
            DecalLightSource::Words(w, sc) => (Some(w), sc, false),
            DecalLightSource::Map => (None, [[0.0; 4]; 3], true),
            DecalLightSource::None => (None, [[0.0; 4]; 3], false),
        };
        let clip = Clip::new(d, inst, self.bias, self.su, self.sv);
        let out = clip.run(&m, words_src);
        let vertex_lit = words_src.is_some();
        let n = out.indices.len() / 3;
        if n == 0 {
            return (empty, 0);
        }
        if self.log {
            let (mut lo, mut hi) = ([f32::MAX; 2], [f32::MIN; 2]);
            for uv in &out.uvs {
                for k in 0..2 {
                    lo[k] = lo[k].min(uv[k]);
                    hi[k] = hi[k].max(uv[k]);
                }
            }
            eprintln!(
                "n1ffi: decal {:08X} on {}: {} tris, uv {:?}..{:?}, size {}x{}, planes {:?}",
                d.id,
                res.mesh_path(inst.mesh).unwrap_or("?"),
                n,
                lo,
                hi,
                d.width,
                d.height,
                d.planes
            );
        }
        let (center, radius) = sphere(&out.positions);
        let section = [SectionDesc { first_index: 0, index_count: out.indices.len() as u32, material: 0 }];
        let desc = MeshDesc {
            positions: &out.positions,
            uvs: &out.uvs,
            normals: &out.normals,
            tangents: &out.tangents,
            uv1: &out.uv1,
            colors: &[],
            indices: &out.indices,
            sections: &section,
            center,
            radius,
        };
        let Some(mesh) = r.add_decal_mesh(&desc) else {
            return (empty, 0);
        };
        let (light, own_light) = if map {
            (light, false)
        } else if vertex_lit {
            (r.add_decal_light(&out.words, sc), true)
        } else {
            (NONE, false)
        };
        (Entry { mesh, light, own_light, last: self.clock }, n)
    }
}

fn planes_valid(planes: &[[f32; 4]; 6]) -> bool {
    planes.iter().all(|p| {
        let l = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
        (l - 1.0).abs() < 0.01 && p[3].is_finite()
    })
}

fn key_of(d: &N1Decal, inst: &Instance, light: u32) -> u64 {
    let mut h = std::collections::hash_map::DefaultHasher::new();
    d.id.hash(&mut h);
    d.flags.hash(&mut h);
    for p in &d.planes {
        for x in p {
            x.to_bits().hash(&mut h);
        }
    }
    for x in d.location.iter().chain(&d.tangent).chain(&d.binormal).chain(&d.normal).chain(&d.tile).chain(&d.offset) {
        x.to_bits().hash(&mut h);
    }
    for x in [d.width, d.height, d.backface_angle] {
        x.to_bits().hash(&mut h);
    }
    for x in &inst.transform {
        x.to_bits().hash(&mut h);
    }
    inst.mesh.hash(&mut h);
    light.hash(&mut h);
    h.finish()
}

fn sphere(p: &[[f32; 3]]) -> ([f32; 3], f32) {
    let mut lo = [f32::MAX; 3];
    let mut hi = [f32::MIN; 3];
    for v in p {
        for k in 0..3 {
            lo[k] = lo[k].min(v[k]);
            hi[k] = hi[k].max(v[k]);
        }
    }
    let c = [(lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5];
    let r = p.iter().map(|v| dist(*v, c)).fold(0.0f32, f32::max);
    (c, r)
}

fn dot(a: [f32; 3], b: [f32; 3]) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
fn add_s(a: [f32; 3], b: [f32; 3], s: f32) -> [f32; 3] {
    [a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s]
}
fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}
fn dist(a: [f32; 3], b: [f32; 3]) -> f32 {
    dot(sub(a, b), sub(a, b)).sqrt()
}
fn norm(a: [f32; 3]) -> Option<[f32; 3]> {
    let l = dot(a, a).sqrt();
    (l > 1e-12).then(|| [a[0] / l, a[1] / l, a[2] / l])
}

/// One clipped vertex: world position, world normal, UV1 and the vertex light's 12 bytes.
#[derive(Clone, Copy)]
struct V {
    p: [f32; 3],
    n: [f32; 3],
    uv1: [f32; 2],
    l: [f32; 12],
}

fn lerp(a: &V, b: &V, t: f32) -> V {
    let f = |x: f32, y: f32| x + (y - x) * t;
    let l: [f32; 12] = std::array::from_fn(|k| f(a.l[k], b.l[k]));
    V {
        p: [f(a.p[0], b.p[0]), f(a.p[1], b.p[1]), f(a.p[2], b.p[2])],
        n: [f(a.n[0], b.n[0]), f(a.n[1], b.n[1]), f(a.n[2], b.n[2])],
        uv1: [f(a.uv1[0], b.uv1[0]), f(a.uv1[1], b.uv1[1])],
        l,
    }
}

#[derive(Default)]
struct Out {
    positions: Vec<[f32; 3]>,
    normals: Vec<[f32; 3]>,
    tangents: Vec<[f32; 4]>,
    uvs: Vec<[f32; 2]>,
    uv1: Vec<[f32; 2]>,
    words: Vec<u32>,
    indices: Vec<u32>,
}

struct Clip<'a> {
    d: &'a N1Decal,
    t: [f32; 16],
    /// Cofactor rows of the 3x3 (normals: n * cof, up to scale).
    cof: [[f32; 3]; 3],
    mirrored: bool,
    planes: [[f32; 4]; 6],
    bias: f32,
    su: f32,
    sv: f32,
}

/// Most triangles one (decal, receiver) pair makes (a guard against a decal stretched over a huge mesh).
const MAX_TRIS: usize = 32768;

impl<'a> Clip<'a> {
    fn new(d: &'a N1Decal, inst: &Instance, bias: f32, su: f32, sv: f32) -> Self {
        let t = inst.transform;
        let r0 = [t[0], t[1], t[2]];
        let r1 = [t[4], t[5], t[6]];
        let r2 = [t[8], t[9], t[10]];
        let det = dot(r0, cross(r1, r2));
        let mut planes = d.planes;
        // The near and far planes (along HitNormal) get a little slack: NearPlane 0 puts the surface exactly on one.
        for p in &mut planes[4..6] {
            p[3] += 1.0;
        }
        Self {
            d,
            t,
            cof: [cross(r1, r2), cross(r2, r0), cross(r0, r1)],
            mirrored: det < 0.0,
            planes,
            bias,
            su,
            sv,
        }
    }

    fn pos(&self, p: [f32; 3]) -> [f32; 3] {
        let t = &self.t;
        [
            p[0] * t[0] + p[1] * t[4] + p[2] * t[8] + t[12],
            p[0] * t[1] + p[1] * t[5] + p[2] * t[9] + t[13],
            p[0] * t[2] + p[1] * t[6] + p[2] * t[10] + t[14],
        ]
    }

    fn nrm(&self, n: [f32; 3]) -> [f32; 3] {
        let c = &self.cof;
        let w = [
            n[0] * c[0][0] + n[1] * c[1][0] + n[2] * c[2][0],
            n[0] * c[0][1] + n[1] * c[1][1] + n[2] * c[2][1],
            n[0] * c[0][2] + n[1] * c[1][2] + n[2] * c[2][2],
        ];
        let w = if self.mirrored { [-w[0], -w[1], -w[2]] } else { w };
        norm(w).unwrap_or(self.d.normal)
    }

    fn outcode(&self, p: [f32; 3]) -> u8 {
        let mut c = 0u8;
        for (k, pl) in self.planes.iter().enumerate() {
            if p[0] * pl[0] + p[1] * pl[1] + p[2] * pl[2] > pl[3] {
                c |= 1 << k;
            }
        }
        c
    }

    fn run(&self, m: &assetdb::MeshAsset, words: Option<&[u32]>) -> Out {
        let nv = m.positions.len();
        let wp: Vec<[f32; 3]> = m.positions.iter().map(|&p| self.pos(p)).collect();
        let code: Vec<u8> = wp.iter().map(|&p| self.outcode(p)).collect();
        let d = self.d;
        let mut out = Out::default();
        let vert = |i: usize| -> V {
            let mut l = [0f32; 12];
            if let Some(w) = words {
                for k in 0..3 {
                    let b = w.get(i * 3 + k).copied().unwrap_or(0).to_le_bytes();
                    for c in 0..4 {
                        l[k * 4 + c] = b[c] as f32;
                    }
                }
            }
            V {
                p: wp[i],
                n: self.nrm(m.normals.get(i).copied().unwrap_or([0.0, 0.0, 1.0])),
                uv1: m.uv1.get(i).copied().unwrap_or([0.0, 0.0]),
                l,
            }
        };
        let mut poly: Vec<V> = Vec::with_capacity(12);
        let mut next: Vec<V> = Vec::with_capacity(12);
        for tri in m.indices.chunks_exact(3) {
            let (a, b, c) = (tri[0] as usize, tri[1] as usize, tri[2] as usize);
            if a >= nv || b >= nv || c >= nv || code[a] & code[b] & code[c] != 0 {
                continue;
            }
            poly.clear();
            poly.extend([vert(a), vert(b), vert(c)]);
            // Facing: the geometric normal, turned to the side the vertex normals are on.
            if d.flags & FLAG_BACKFACES == 0 {
                let mut f = cross(sub(poly[1].p, poly[0].p), sub(poly[2].p, poly[0].p));
                let vn = [
                    poly[0].n[0] + poly[1].n[0] + poly[2].n[0],
                    poly[0].n[1] + poly[1].n[1] + poly[2].n[1],
                    poly[0].n[2] + poly[1].n[2] + poly[2].n[2],
                ];
                if dot(f, vn) < 0.0 {
                    f = [-f[0], -f[1], -f[2]];
                }
                let Some(f) = norm(f) else { continue };
                if dot(f, d.normal) < d.backface_angle {
                    continue;
                }
            }
            let any = code[a] | code[b] | code[c];
            for (k, pl) in self.planes.iter().enumerate() {
                if any & (1 << k) == 0 {
                    continue;
                }
                next.clear();
                let dist = |v: &V| v.p[0] * pl[0] + v.p[1] * pl[1] + v.p[2] * pl[2] - pl[3];
                for i in 0..poly.len() {
                    let (p, q) = (&poly[i], &poly[(i + 1) % poly.len()]);
                    let (dp, dq) = (dist(p), dist(q));
                    if dp <= 0.0 {
                        next.push(*p);
                    }
                    if (dp <= 0.0) != (dq <= 0.0) {
                        next.push(lerp(p, q, dp / (dp - dq)));
                    }
                }
                std::mem::swap(&mut poly, &mut next);
                if poly.len() < 3 {
                    break;
                }
            }
            if poly.len() < 3 {
                continue;
            }
            let base = out.positions.len() as u32;
            for v in &poly {
                self.emit(&mut out, v, words.is_some());
            }
            for j in 1..poly.len() as u32 - 1 {
                if self.mirrored {
                    out.indices.extend([base, base + j + 1, base + j]);
                } else {
                    out.indices.extend([base, base + j, base + j + 1]);
                }
            }
            if out.indices.len() / 3 >= MAX_TRIS {
                break;
            }
        }
        out
    }

    fn emit(&self, out: &mut Out, v: &V, words: bool) {
        let d = self.d;
        let rel = sub(v.p, d.location);
        let u = self.su * dot(rel, d.tangent) / d.width.max(1e-3) * d.tile[0] + 0.5 + d.offset[0];
        let w = self.sv * dot(rel, d.binormal) / d.height.max(1e-3) * d.tile[1] + 0.5 + d.offset[1];
        let n = norm(v.n).unwrap_or(d.normal);
        let tdir = [d.tangent[0] * self.su, d.tangent[1] * self.su, d.tangent[2] * self.su];
        let t = norm(add_s(tdir, n, -dot(tdir, n))).unwrap_or(tdir);
        let vdir = [d.binormal[0] * self.sv, d.binormal[1] * self.sv, d.binormal[2] * self.sv];
        let sign = if dot(cross(n, t), vdir) < 0.0 { -1.0 } else { 1.0 };
        out.positions.push(add_s(v.p, d.normal, self.bias));
        out.normals.push(n);
        out.tangents.push([t[0], t[1], t[2], sign]);
        out.uvs.push([u, w]);
        out.uv1.push(v.uv1);
        if words {
            for k in 0..3 {
                let b: [u8; 4] = std::array::from_fn(|c| v.l[k * 4 + c].round().clamp(0.0, 255.0) as u8);
                out.words.push(u32::from_le_bytes(b));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A 400 x 400 floor quad (two triangles, normals up) at z = 0, translated by (1000, 0, 0); words per vertex.
    fn floor() -> (assetdb::MeshAsset, Instance) {
        let m = assetdb::MeshAsset {
            name: "floor".into(),
            positions: vec![[-200.0, -200.0, 0.0], [200.0, -200.0, 0.0], [200.0, 200.0, 0.0], [-200.0, 200.0, 0.0]],
            normals: vec![[0.0, 0.0, 1.0]; 4],
            uvs: vec![[0.0, 0.0]; 4],
            indices: vec![0, 1, 2, 0, 2, 3],
            sections: Vec::new(),
            center: [0.0; 3],
            radius: 300.0,
            tangents: Vec::new(),
            colors: Vec::new(),
            uv1: vec![[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
        };
        let mut t = [0f32; 16];
        t[0] = 1.0;
        t[5] = 1.0;
        t[10] = 1.0;
        t[15] = 1.0;
        t[12] = 1000.0;
        let inst = Instance { transform: t, mesh: 0, light: NONE, mat_offset: 0, mat_count: 0, env: NONE };
        (m, inst)
    }

    /// A 100 x 100 decal pointing down at (1000, 0, 0), as the game fills one (Planes from the frame).
    fn decal(normal_z: f32) -> N1Decal {
        let (l, t, b, n) = ([1000.0, 0.0, 0.0], [0.0, 1.0, 0.0], [-1.0, 0.0, 0.0], [0.0, 0.0, normal_z]);
        let pl = |d: [f32; 3], w: f32| [d[0], d[1], d[2], w];
        N1Decal {
            material: 0,
            id: 1,
            receiver_offset: 0,
            receiver_count: 1,
            planes: [
                pl([0.0, -1.0, 0.0], 50.0),
                pl([0.0, 1.0, 0.0], 50.0),
                pl([1.0, 0.0, 0.0], 1050.0),
                pl([-1.0, 0.0, 0.0], -950.0),
                pl([0.0, 0.0, 1.0], 0.0),
                pl([0.0, 0.0, -1.0], 300.0),
            ],
            location: l,
            width: 100.0,
            tangent: t,
            height: 100.0,
            binormal: b,
            backface_angle: 0.001,
            normal: n,
            flags: 0,
            tile: [1.0, 1.0],
            offset: [0.0, 0.0],
        }
    }

    fn area(o: &Out) -> f32 {
        o.indices
            .chunks_exact(3)
            .map(|t| {
                let (a, b, c) = (o.positions[t[0] as usize], o.positions[t[1] as usize], o.positions[t[2] as usize]);
                dot(cross(sub(b, a), sub(c, a)), cross(sub(b, a), sub(c, a))).sqrt() * 0.5
            })
            .sum()
    }

    #[test]
    fn clips_floor_to_box() {
        let (m, inst) = floor();
        let d = decal(1.0);
        let words: Vec<u32> = (0..12).map(|k| if k < 6 { 0x40404040 } else { 0x80808080 }).collect();
        let o = Clip::new(&d, &inst, 0.5, -1.0, -1.0).run(&m, Some(&words));
        assert!((area(&o) - 100.0 * 100.0).abs() < 1.0, "area {}", area(&o));
        assert_eq!(o.words.len(), o.positions.len() * 3);
        for (p, uv) in o.positions.iter().zip(&o.uvs) {
            assert!((p[2] - 0.5).abs() < 1e-4, "biased along the normal");
            assert!((-1e-3..=1.0 + 1e-3).contains(&uv[0]) && (-1e-3..=1.0 + 1e-3).contains(&uv[1]), "{uv:?}");
            // u along -tangent (-Y), v along -binormal (+X).
            let du = 0.5 - p[1] / 100.0;
            let dv = 0.5 + (p[0] - 1000.0) / 100.0;
            assert!((uv[0] - du).abs() < 1e-3 && (uv[1] - dv).abs() < 1e-3, "{p:?} {uv:?}");
        }
    }

    #[test]
    fn skips_back_faces() {
        let (m, inst) = floor();
        let o = Clip::new(&decal(-1.0), &inst, 0.5, -1.0, -1.0).run(&m, None);
        assert!(o.indices.is_empty());
    }
}
