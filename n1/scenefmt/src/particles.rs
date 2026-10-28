//! N9: particle emitters from the scene feed -> GPU-ready instance arrays. Plain Rust, no wgpu types: the `ash`
//! renderer in the port can use this as it is.
//!
//! Input: one feed frame (`primitives[].emitters[]`) and the extractor's
//! `particles.json`. Output: one `EmitterDraw` per live emitter instance, with
//! - sprite emitters: one `SpriteInst` (20 floats) per particle, in the order the game's index array lists them;
//! - mesh emitters: one `MeshInst` (the particle's 4x4 transform, Y-up column-major, plus colour) per particle.
//! All positions are in the N1 renderer's Y-up space (UE3 X,Y,Z -> X,Z,Y).

use anyhow::{Context, Result};
use serde::Deserialize;
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

pub mod matprog;
pub mod shader;

/// `SpriteInst` float layout (80 bytes):
/// 0..3 position, 3 rotation (rad); 4..7 old position, 7 sub-UV interpolation;
/// 8..10 size (x, y); 10..12 UV of the first sub-UV cell's min corner; 12..14 second cell's min corner;
/// 14..16 cell size in UV; 16..20 colour (linear, may exceed 1).
pub type SpriteInst = [f32; 20];
/// `MeshInst`: 0..16 column-major Y-up model matrix, 16..20 colour.
pub type MeshInst = [f32; 20];

#[derive(Deserialize, Clone, Debug, Default)]
pub struct EmitterInfo {
    #[serde(default)]
    pub sort_mode: String,
    #[serde(default)]
    pub screen_alignment: String,
    #[serde(default)]
    pub material: String,
    #[serde(default)]
    pub mesh: String,
    #[serde(default)]
    pub max_draw_count: i32,
    #[serde(default)]
    pub use_max_draw_count: bool,
}

#[derive(Deserialize, Clone, Debug)]
pub struct PMeshDesc {
    pub name: String,
    pub vertex_offset: u64,
    pub vertex_count: u32,
    pub index_offset: u64,
    pub index_count: u32,
    pub bounds_min: [f32; 3],
    pub bounds_max: [f32; 3],
    pub sections: Vec<(u32, u32, String)>,
}

pub struct ParticleAssets {
    pub dir: PathBuf,
    pub emitters: BTreeMap<String, EmitterInfo>,
    /// Material path (lower case) -> the extractor's JSON (class, props, expressions graph, ...).
    pub materials: BTreeMap<String, Value>,
    pub textures: BTreeMap<String, String>,
    pub meshes: Vec<PMeshDesc>,
    pub mesh_bin: Vec<u8>,
}

impl ParticleAssets {
    pub fn load(dir: &Path) -> Result<Self> {
        let v: Value = serde_json::from_slice(&std::fs::read(dir.join("particles.json")).context("particles.json")?)?;
        let emitters = serde_json::from_value(v["emitters"].clone())?;
        let materials = v["materials"].as_object().map(|m| m.iter().map(|(k, x)| (k.to_lowercase(), x.clone())).collect()).unwrap_or_default();
        let textures = serde_json::from_value(v["textures"].clone())?;
        let meshes = serde_json::from_value(v["meshes"].clone())?;
        let mesh_bin = std::fs::read(dir.join("pmeshes.bin")).unwrap_or_default();
        Ok(Self { dir: dir.to_path_buf(), emitters, materials, textures, meshes, mesh_bin })
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Sprite,
    Mesh,
    Trail,
    Beam,
}

/// How a sprite is oriented: 0 camera-facing square, 1 camera-facing rectangle, 2 velocity-aligned.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Align {
    Square = 0,
    Rectangle = 1,
    Velocity = 2,
}

pub struct EmitterDraw {
    pub kind: Kind,
    /// Index of the feed primitive (ParticleSystemComponent) this emitter belongs to.
    pub prim: usize,
    pub emitter: String,
    pub material: String,
    pub mesh: String,
    pub align: Align,
    pub sub_images: [u32; 2],
    /// Emitter origin (Y-up) for ordering emitters.
    pub origin: [f32; 3],
    pub sprites: Vec<SpriteInst>,
    pub meshes: Vec<MeshInst>,
}

#[inline]
fn swz(v: [f32; 3]) -> [f32; 3] {
    [v[0], v[2], v[1]]
}

fn f(v: &Value, i: usize) -> f32 {
    v.get(i).and_then(Value::as_f64).unwrap_or(0.0) as f32
}

/// The sub-UV frame data `[ImageH, ImageV, Interp, Image2H, Image2V]` of a particle's payload. The feed now sends
/// exactly those 5 floats (from the emitter instance's SubUVDataOffset, B-229). Older feeds and JSON captures without
/// `sub_uv_offset` pass the whole module payload, which is searched for five floats whose cell indices are whole
/// numbers inside the grid, preferring the last non-zero window. That search is unreliable: the real block is
/// followed by zero padding, so `[Image2H, Image2V, 0, 0, 0]` also passes and wins, which drew the next frame
/// unblended in most sub-UV emitters.
pub fn find_subuv(payload: &[f32], grid: [u32; 2]) -> Option<(usize, [f32; 5])> {
    if payload.len() < 5 {
        return None;
    }
    if payload.len() == 5 {
        let p = [payload[0], payload[1], payload[2], payload[3], payload[4]];
        // Exact frame data: trust it, clamping the cells into the grid.
        if p.iter().all(|x| x.is_finite()) {
            let c = |x: f32, n: u32| x.max(0.0).min(n as f32 - 1.0).floor();
            return Some((0, [c(p[0], grid[0]), c(p[1], grid[1]), p[2].clamp(0.0, 1.0), c(p[3], grid[0]), c(p[4], grid[1])]));
        }
        return None;
    }
    let ok_cell = |h: f32, v: f32| h >= 0.0 && v >= 0.0 && h.fract() == 0.0 && v.fract() == 0.0 && (h as u32) < grid[0] && (v as u32) < grid[1];
    let mut best: Option<(usize, [f32; 5])> = None;
    for j in 0..=payload.len() - 5 {
        let p = &payload[j..j + 5];
        if p.iter().any(|x| !x.is_finite()) || !ok_cell(p[0], p[1]) || !ok_cell(p[3], p[4]) {
            continue;
        }
        let nonzero = p.iter().any(|&x| x != 0.0);
        if nonzero || best.is_none() {
            best = Some((j, [p[0], p[1], p[2], p[3], p[4]]));
        }
    }
    best
}

pub fn parse_align(s: &str, fallback: i64) -> Align {
    match (s, fallback) {
        ("PSA_Rectangle", _) | ("", 1) => Align::Rectangle,
        ("PSA_Velocity", _) | ("", 2) => Align::Velocity,
        _ => Align::Square,
    }
}

fn mat_mul_point(m: &[f32; 16], p: [f32; 3]) -> [f32; 3] {
    // UE3 row-major, row vectors: p' = [p 1] * M
    [
        p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12],
        p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
        p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14],
    ]
}

/// Build the draws for one feed frame. `cam` is the camera location in UE units (used to order emitters).
pub fn build_frame(_assets: &ParticleAssets, frame: &Value, cam: [f32; 3], include_hidden: bool) -> Vec<EmitterDraw> {
    let mut out = Vec::new();
    for (pi, prim) in frame["primitives"].as_array().into_iter().flatten().enumerate() {
        let Some(ems) = prim["emitters"].as_array() else { continue };
        if !include_hidden && prim["hidden"].as_bool().unwrap_or(false) {
            continue;
        }
        let l2w: Option<[f32; 16]> = prim["transform"].as_array().filter(|a| a.len() == 16).map(|a| std::array::from_fn(|i| a[i].as_f64().unwrap_or(0.0) as f32));
        for e in ems {
            let ty = e["type"].as_str().unwrap_or("");
            let kind = match ty {
                "sprite" => Kind::Sprite,
                "mesh" => Kind::Mesh,
                "trail" => Kind::Trail,
                "beam" => Kind::Beam,
                _ => continue,
            };
            let particles = e["particles"].as_array().map(|a| a.as_slice()).unwrap_or(&[]);
            if particles.is_empty() {
                continue;
            }
            let local = e["local_space"].as_bool().unwrap_or(false);
            let sub = [e["sub_images"][0].as_u64().unwrap_or(1).max(1) as u32, e["sub_images"][1].as_u64().unwrap_or(1).max(1) as u32];
            let method = e["sub_uv_method"].as_i64().unwrap_or(0);
            let lock = e["lock_axis"].as_u64().unwrap_or(0) as u32;
            let align = parse_align("", e["screen_alignment"].as_i64().unwrap_or(0));
            let epath = e["emitter"].as_str().unwrap_or("").to_string();
            let mut d = EmitterDraw {
                kind,
                prim: pi,
                emitter: epath,
                material: e["material"].as_str().unwrap_or("").to_string(),
                mesh: e["mesh"].as_str().unwrap_or("").to_string(),
                align,
                sub_images: sub,
                origin: swz([f(&e["location"], 0), f(&e["location"], 1), f(&e["location"], 2)]),
                sprites: Vec::new(),
                meshes: Vec::new(),
            };
            let _ = cam;
            for pa in particles {
                let Some(pa) = pa.as_array() else { continue };
                if pa.len() < 15 {
                    continue;
                }
                let mut row: Vec<f32> = pa.iter().map(|x| x.as_f64().unwrap_or(0.0) as f32).collect();
                // B-229: the capture says where the sub-UV frame data is; keep just those 5 payload floats.
                if let Some(off) = e["sub_uv_offset"].as_u64().filter(|o| *o >= 0x80 && kind == Kind::Sprite) {
                    let at = 15 + (off as usize - 0x80) / 4;
                    if row.len() >= at + 5 {
                        let sub: Vec<f32> = row[at..at + 5].to_vec();
                        row.truncate(15);
                        row.extend(sub);
                    }
                }
                push_particle(&mut d, &row, local, l2w.as_ref(), method);
                if kind == Kind::Sprite && lock != 0 {
                    if let Some(s) = d.sprites.last_mut() {
                        lock_sprite(s, lock, if local { l2w.as_ref() } else { None });
                    }
                }
            }
            out.push(d);
        }
    }
    out
}

/// Add one particle row to an emitter draw. `row`: [location 3, old_location 3, size 3, rotation, colour 4,
/// relative_time, payload...] in UE3 units; `local` rows are moved to world space with `l2w`. `method` is the
/// emitter's sub-UV interpolation method (0: none).
pub fn push_particle(d: &mut EmitterDraw, pa: &[f32], local: bool, l2w: Option<&[f32; 16]>, method: i64) {
    if pa.len() < 15 {
        return;
    }
    let sub = d.sub_images;
    let g = |i: usize| pa[i];
    let mut pos = [g(0), g(1), g(2)];
    let mut old = [g(3), g(4), g(5)];
    if local {
        if let Some(m) = l2w {
            pos = mat_mul_point(m, pos);
            old = mat_mul_point(m, old);
        }
    }
    let mut size = [g(6), g(7), g(8)];
    // PSA_Square sprites are Size.X wide and tall (UE3 copies X into Y); many emitters only scale X.
    if d.align == Align::Square {
        size[1] = size[0];
    }
    let rot = g(9);
    let col = [g(10), g(11), g(12), g(13)];
    match d.kind {
        Kind::Sprite | Kind::Trail | Kind::Beam => {
            let payload = &pa[15..];
            let (uv1, uv2, uvs, interp) = if (sub[0] > 1 || sub[1] > 1) && method != 0 {
                let (cw, ch) = (1.0 / sub[0] as f32, 1.0 / sub[1] as f32);
                match find_subuv(payload, sub) {
                    Some((_, p)) => ([p[0] * cw, p[1] * ch], [p[3] * cw, p[4] * ch], [cw, ch], p[2]),
                    None => ([0.0, 0.0], [0.0, 0.0], [cw, ch], 0.0),
                }
            } else {
                ([0.0, 0.0], [0.0, 0.0], [1.0, 1.0], 0.0)
            };
            let p = swz(pos);
            let o = swz(old);
            d.sprites.push([
                p[0], p[1], p[2], rot, o[0], o[1], o[2], interp, size[0], size[1], uv1[0], uv1[1], uv2[0], uv2[1], uvs[0], uvs[1], col[0], col[1], col[2], col[3],
            ]);
        }
        Kind::Mesh => {
            // Rotation payload (degrees) follows RelativeTime for mesh emitters (validated in N9 against the
            // port's per-draw matrices); `rot` is the sprite-style roll and is unused here.
            let m = mesh_particle_matrix(pos, size, &pa[15..]);
            d.meshes.push([m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], col[0], col[1], col[2], col[3]]);
        }
    }
}

/// Added to a sprite's sub-UV interpolation (float 7, which is in 0..1) to mark it axis-locked: `LOCK_PLANE` puts it in
/// the plane whose normal is floats 4..7 (in place of the old position, which only velocity alignment reads);
/// `LOCK_ROTATE` keeps it facing the camera but turning only about the axis in floats 4..7.
pub const LOCK_PLANE: f32 = 2.0;
pub const LOCK_ROTATE: f32 = 4.0;

/// Apply an OrientationAxisLock module (`lock`: EParticleAxisLock) to a sprite instance. UE3
/// (`FParticleSystemSceneProxy::GetAxisLockValues`) swaps the camera's right/up for fixed axes: EPAL_X..EPAL_NEGATIVE_Z
/// make a quad in the plane normal to that axis (EPAL_Z: a flat glow on the floor), EPAL_ROTATE_X..Z a quad that
/// turns about the axis toward the camera. Local-space emitters take the axes through the component transform.
pub fn lock_sprite(s: &mut SpriteInst, lock: u32, l2w: Option<&[f32; 16]>) {
    let (axis, mark) = match lock {
        1 => ([1.0, 0.0, 0.0], LOCK_PLANE),
        2 => ([0.0, 1.0, 0.0], LOCK_PLANE),
        3 => ([0.0, 0.0, 1.0], LOCK_PLANE),
        4 => ([-1.0, 0.0, 0.0], LOCK_PLANE),
        5 => ([0.0, -1.0, 0.0], LOCK_PLANE),
        6 => ([0.0, 0.0, -1.0], LOCK_PLANE),
        7 => ([1.0, 0.0, 0.0], LOCK_ROTATE),
        8 => ([0.0, 1.0, 0.0], LOCK_ROTATE),
        9 => ([0.0, 0.0, 1.0], LOCK_ROTATE),
        _ => return,
    };
    let a: [f32; 3] = match l2w {
        // UE3 row vectors: a direction picks up the upper 3x3 only.
        Some(m) => std::array::from_fn(|j| axis[0] * m[j] + axis[1] * m[4 + j] + axis[2] * m[8 + j]),
        None => axis,
    };
    let a = swz(a);
    s[4] = a[0];
    s[5] = a[1];
    s[6] = a[2];
    s[7] += mark;
}

/// UE3 mesh particle transform -> column-major Y-up matrix. `payload[0..3]` is the rotation in degrees
/// (pitch, yaw, roll; FRotator order), scale is the particle Size.
pub fn mesh_particle_matrix(pos: [f32; 3], size: [f32; 3], payload: &[f32]) -> [f32; 16] {
    let r = |i: usize| payload.get(i).copied().unwrap_or(0.0).to_radians();
    let (p, y, rl) = (r(0), r(1), r(2));
    let (sp, cp, sy, cy, sr, cr) = (p.sin(), p.cos(), y.sin(), y.cos(), r_sin(rl), rl.cos());
    // FRotationMatrix rows (UE3, row vectors): X, Y, Z axes.
    let x = [cp * cy, cp * sy, sp];
    let yv = [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp];
    let z = [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp];
    let s = [size[0], size[1], size[2]];
    // UE rows scaled; convert to Y-up column-major: B = C M^T C, with the same swap as scenefmt::ue_to_yup.
    let ue = [
        [x[0] * s[0], x[1] * s[0], x[2] * s[0], 0.0],
        [yv[0] * s[1], yv[1] * s[1], yv[2] * s[1], 0.0],
        [z[0] * s[2], z[1] * s[2], z[2] * s[2], 0.0],
        [pos[0], pos[1], pos[2], 1.0],
    ];
    let flat: [f32; 16] = std::array::from_fn(|i| ue[i / 4][i % 4]);
    crate::ue_to_yup(&flat)
}

#[inline]
fn r_sin(x: f32) -> f32 {
    x.sin()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn find_subuv_exact_and_search() {
        // B-229, AcridSmokeSimple_Unlit_SubUV (8x8, Linear_Blend): the whole payload, as an old feed sent it. The real
        // block is at float 4 (cell 7,3 blending 0.75 into 0,4); the search's "last non-zero" rule picks float 7.
        let whole = [0.0, 0.0, 4.39, 0.0, 7.0, 3.0, 0.75, 0.0, 4.0, 0.0, 0.0, 0.0];
        assert_eq!(find_subuv(&whole, [8, 8]), Some((7, [0.0, 4.0, 0.0, 0.0, 0.0])));
        // The feed now sends just the frame data.
        assert_eq!(find_subuv(&whole[4..9], [8, 8]), Some((0, [7.0, 3.0, 0.75, 0.0, 4.0])));
        // Out-of-grid cells are clamped, interp kept in 0..1.
        assert_eq!(find_subuv(&[9.0, -1.0, 1.5, 2.0, 1.0], [4, 2]), Some((0, [3.0, 0.0, 1.0, 2.0, 1.0])));
        assert_eq!(find_subuv(&[f32::NAN, 0.0, 0.0, 0.0, 0.0], [4, 2]), None);
    }

    #[test]
    fn lock_sprite_marks_and_axes() {
        let mut s: SpriteInst = [0.0; 20];
        s[4] = 9.0;
        s[7] = 0.25;
        // EPAL_Z: the quad's normal is UE3 +Z, Y-up +Y; the interpolation keeps its value under the mark
        lock_sprite(&mut s, 3, None);
        assert_eq!(&s[4..7], &[0.0, 1.0, 0.0]);
        assert_eq!(s[7], LOCK_PLANE + 0.25);
        // EPAL_ROTATE_X: axis UE3 +X
        let mut r: SpriteInst = [0.0; 20];
        lock_sprite(&mut r, 7, None);
        assert_eq!(&r[4..8], &[1.0, 0.0, 0.0, LOCK_ROTATE]);
        // A local-space emitter takes the axis through the component transform (a 90 degree yaw: X -> Y)
        let mut m = [0.0f32; 16];
        m[1] = 1.0;
        m[4] = -1.0;
        m[10] = 1.0;
        m[15] = 1.0;
        let mut l: SpriteInst = [0.0; 20];
        lock_sprite(&mut l, 1, Some(&m));
        assert_eq!(&l[4..7], &[0.0, 0.0, 1.0]);
        // EPAL_NONE leaves the instance alone
        let mut n: SpriteInst = [0.0; 20];
        lock_sprite(&mut n, 0, None);
        assert_eq!(n, [0.0; 20]);
    }
}
