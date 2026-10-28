//! N7: skeletal meshes (what `skel_extract` writes: skel.json + skel.bin) and their per-frame pose from a live scene
//! feed frame (N2's JSON: `primitives[kind == "skeletal"]` with `transform` and `bones`).
//!
//! Everything here stays in UE3 space (Z up, left-handed, row vectors). The skinning matrix of bone b is
//! `InvRef[b] * SpaceBases[b] * LocalToWorld`, exactly what the game uploads per bone (`InvRef * SpaceBases`, checked
//! bit-for-bit against the port's vertex-shader constants) times the component's LocalToWorld. The renderer
//! swizzles to its Y-up space after skinning.

use anyhow::{Context, Result};
use serde::Deserialize;
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};

use crate::MaterialDesc;

#[derive(Deserialize, Clone)]
pub struct SkelSectionDesc {
    pub first_index: u32,
    pub index_count: u32,
    pub material: String,
    /// Chunk (bone-map group) this section draws from.
    #[serde(default)]
    pub chunk: i16,
}

/// A chunk: its vertex range and BoneMap (chunk-local influence index -> mesh bone). skel.bin's bone indices already
/// have the BoneMap applied; a renderer that uploads per-chunk palettes like the game does needs this.
#[derive(Deserialize, Clone, Default)]
pub struct SkelChunkDesc {
    pub first_vertex: u32,
    pub num_vertices: u32,
    pub bones: Vec<u16>,
}

#[derive(Deserialize, Clone)]
pub struct SkelLodDesc {
    pub vertex_offset: u64,
    pub vertex_count: u32,
    pub index_offset: u64,
    pub index_count: u32,
    pub sections: Vec<SkelSectionDesc>,
    #[serde(default)]
    pub chunks: Vec<SkelChunkDesc>,
}

#[derive(Deserialize, Clone)]
pub struct SkelMeshDesc {
    pub name: String,
    pub package: String,
    pub bone_names: Vec<String>,
    pub parents: Vec<i32>,
    pub ref_local: Vec<[f32; 7]>,
    pub inv_ref: Vec<[f32; 16]>,
    pub bounds_origin: [f32; 3],
    pub bounds_extent: [f32; 3],
    pub sphere_radius: f32,
    pub materials: Vec<String>,
    pub lods: Vec<SkelLodDesc>,
}

#[derive(Deserialize)]
pub struct SkelDesc {
    pub meshes: Vec<SkelMeshDesc>,
    pub materials: BTreeMap<String, MaterialDesc>,
    pub textures: BTreeMap<String, String>,
    pub stats: BTreeMap<String, serde_json::Value>,
}

/// One skinned vertex as stored in skel.bin (40 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SkinVertex {
    pub pos: [f32; 3],
    pub normal: [f32; 3],
    pub uv: [f32; 2],
    pub bones: [u8; 4],
    pub weights: [u8; 4],
}

pub struct SkelLodData {
    pub verts: Vec<SkinVertex>,
    pub indices: Vec<u32>,
}

pub struct SkelScene {
    pub dir: PathBuf,
    pub desc: SkelDesc,
    /// [mesh][lod]
    pub lods: Vec<Vec<SkelLodData>>,
    pub by_name: HashMap<String, usize>,
}

impl SkelScene {
    pub fn load(dir: &Path) -> Result<Self> {
        let desc: SkelDesc = serde_json::from_slice(&std::fs::read(dir.join("skel.json")).context("skel.json")?)?;
        let b = std::fs::read(dir.join("skel.bin")).context("skel.bin")?;
        let f = |o: usize| f32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]);
        let lods = desc
            .meshes
            .iter()
            .map(|m| {
                m.lods
                    .iter()
                    .map(|l| {
                        let vo = l.vertex_offset as usize;
                        let verts = (0..l.vertex_count as usize)
                            .map(|i| {
                                let o = vo + i * 40;
                                SkinVertex {
                                    pos: [f(o), f(o + 4), f(o + 8)],
                                    normal: [f(o + 12), f(o + 16), f(o + 20)],
                                    uv: [f(o + 24), f(o + 28)],
                                    bones: [b[o + 32], b[o + 33], b[o + 34], b[o + 35]],
                                    weights: [b[o + 36], b[o + 37], b[o + 38], b[o + 39]],
                                }
                            })
                            .collect();
                        let io = l.index_offset as usize;
                        let indices = (0..l.index_count as usize)
                            .map(|i| u32::from_le_bytes(b[io + 4 * i..io + 4 * i + 4].try_into().unwrap()))
                            .collect();
                        SkelLodData { verts, indices }
                    })
                    .collect()
            })
            .collect();
        let by_name = desc.meshes.iter().enumerate().map(|(i, m)| (m.name.clone(), i)).collect();
        Ok(Self { dir: dir.to_path_buf(), desc, lods, by_name })
    }

    pub fn material(&self, name: &str) -> MaterialDesc {
        self.desc.materials.get(name).cloned().unwrap_or_default()
    }
}

/// UE3 `FQuatRotationTranslationMatrix` (row-major, row vectors).
pub fn quat_trans(q: [f32; 4], t: [f32; 3]) -> [f32; 16] {
    let [x, y, z, w] = q;
    let (x2, y2, z2) = (x + x, y + y, z + z);
    let (xx, xy, xz) = (x * x2, x * y2, x * z2);
    let (yy, yz, zz) = (y * y2, y * z2, z * z2);
    let (wx, wy, wz) = (w * x2, w * y2, w * z2);
    [
        1.0 - (yy + zz), xy + wz, xz - wy, 0.0, //
        xy - wz, 1.0 - (xx + zz), yz + wx, 0.0, //
        xz + wy, yz - wx, 1.0 - (xx + yy), 0.0, //
        t[0], t[1], t[2], 1.0,
    ]
}

/// Row-major 4x4 product a * b (row vectors: apply a, then b).
pub fn mul(a: &[f32; 16], b: &[f32; 16]) -> [f32; 16] {
    let mut o = [0f32; 16];
    for i in 0..4 {
        for j in 0..4 {
            o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        }
    }
    o
}

/// A posed skeletal primitive for one frame.
#[derive(Clone)]
pub struct SkelInstance {
    pub mesh: usize,
    pub lod: usize,
    pub name: String,
    pub owner_class: String,
    /// Component overrides ("" = mesh's own), per mesh material slot.
    pub materials: Vec<String>,
    /// Skinning matrices, one per mesh bone: InvRef * SpaceBase * LocalToWorld, UE3 row-major.
    pub skin: Vec<[f32; 16]>,
    pub bounds_origin: [f32; 3],
    pub bounds_radius: f32,
    pub rendered: bool,
    /// Bones the feed reported as null (not updated by the LOD); filled from the ref pose under their parent.
    pub null_bones: usize,
}

/// Compute the skinning palette of one primitive. `space_bases` are the feed's component-space bone matrices
/// (row-major, `None` = not updated). Missing bones get `RefLocal * SpaceBase[parent]`.
pub fn skin_palette(m: &SkelMeshDesc, space_bases: &[Option<[f32; 16]>], l2w: &[f32; 16]) -> (Vec<[f32; 16]>, usize) {
    let n = m.bone_names.len();
    let mut sb: Vec<[f32; 16]> = Vec::with_capacity(n);
    let mut nulls = 0;
    for b in 0..n {
        let v = match space_bases.get(b).copied().flatten() {
            Some(x) => x,
            None => {
                nulls += 1;
                let r = m.ref_local[b];
                let local = quat_trans([r[0], r[1], r[2], r[3]], [r[4], r[5], r[6]]);
                if b == 0 { local } else { mul(&local, &sb[m.parents[b] as usize]) }
            }
        };
        sb.push(v);
    }
    ((0..n).map(|b| mul(&mul(&m.inv_ref[b], &sb[b]), l2w)).collect(), nulls)
}

fn arr16(v: &serde_json::Value) -> Option<[f32; 16]> {
    let a = v.as_array()?;
    if a.len() != 16 {
        return None;
    }
    let mut o = [0f32; 16];
    for (i, x) in a.iter().enumerate() {
        o[i] = x.as_f64()? as f32;
    }
    Some(o)
}

/// Pose every skeletal primitive of a feed frame (already parsed JSON). Returns (instances, unknown mesh paths).
pub fn pose_frame(scene: &SkelScene, frame: &serde_json::Value) -> (Vec<SkelInstance>, Vec<String>) {
    let mut out = Vec::new();
    let mut unknown = Vec::new();
    for p in frame["primitives"].as_array().into_iter().flatten() {
        if p["kind"] != "skeletal" {
            continue;
        }
        let mesh = p["mesh"].as_str().unwrap_or_default();
        let Some(&mi) = scene.by_name.get(mesh) else {
            unknown.push(mesh.to_string());
            continue;
        };
        let Some(l2w) = arr16(&p["transform"]) else { continue };
        let bones: Vec<Option<[f32; 16]>> = p["bones"].as_array().map(|a| a.iter().map(arr16).collect()).unwrap_or_default();
        let md = &scene.desc.meshes[mi];
        let (skin, null_bones) = skin_palette(md, &bones, &l2w);
        let lod = (p["lod"].as_u64().unwrap_or(0) as usize).min(md.lods.len() - 1);
        let b = &p["bounds"];
        let o = b["origin"].as_array().map(|a| [0, 1, 2].map(|i| a[i].as_f64().unwrap_or(0.0) as f32)).unwrap_or([0.0; 3]);
        out.push(SkelInstance {
            mesh: mi,
            lod,
            name: p["name"].as_str().unwrap_or_default().to_string(),
            owner_class: p["owner_class"].as_str().unwrap_or_default().to_string(),
            materials: p["materials"].as_array().map(|a| a.iter().map(|x| x.as_str().unwrap_or_default().to_string()).collect()).unwrap_or_default(),
            skin,
            bounds_origin: o,
            bounds_radius: b["radius"].as_f64().unwrap_or(1e9) as f32,
            rendered: p["rendered"].as_bool().unwrap_or(true),
            null_bones,
        });
    }
    (out, unknown)
}
