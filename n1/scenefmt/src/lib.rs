//! Renderer-agnostic static scene: what the `extract` tool writes (scene.json + meshes.bin + light.bin + tex/*.dds),
//! loaded into CPU arrays in a Y-up right-handed space (UE3 X,Y,Z -> X,Z,Y). Any renderer (Bevy viewer, raw wgpu, a
//! C-ABI renderer inside the port) consumes `Scene`. Placements can come from the map's actors (extract) or from a
//! live scene feed JSON (`{mesh: "<package.object path>", transform: [16 floats, row-major UE3 FMatrix]}`).

pub mod skel;
pub mod particles;

use anyhow::{Context, Result};
use serde::Deserialize;
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};

#[derive(Deserialize, Clone)]
pub struct SectionDesc {
    pub first_index: u32,
    pub index_count: u32,
    pub material: String,
}

#[derive(Deserialize, Clone)]
pub struct MeshDesc {
    pub name: String,
    pub vertex_offset: u64,
    pub vertex_count: u32,
    pub index_offset: u64,
    pub index_count: u32,
    pub has_uv: bool,
    pub bounds_min: [f32; 3],
    pub bounds_max: [f32; 3],
    pub sections: Vec<SectionDesc>,
}

#[derive(Deserialize, Clone, Default)]
pub struct MaterialDesc {
    pub diffuse: Option<String>,
    pub blend: String,
    pub two_sided: bool,
    pub class: String,
}

#[derive(Deserialize, Clone)]
pub struct PlacementDesc {
    pub mesh: u32,
    pub transform: [f32; 16],
    pub materials: Vec<String>,
    pub light_offset: Option<u64>,
    #[serde(default)]
    pub package: String,
    #[serde(default)]
    pub actor: String,
}

#[derive(Deserialize)]
pub struct SceneDesc {
    pub packages: Vec<String>,
    pub meshes: Vec<MeshDesc>,
    pub materials: BTreeMap<String, MaterialDesc>,
    pub textures: BTreeMap<String, String>,
    pub placements: Vec<PlacementDesc>,
    pub stats: BTreeMap<String, serde_json::Value>,
}

/// Mesh geometry in Y-up space (positions/normals swizzled, winding flipped to stay CCW-front).
pub struct MeshData {
    pub positions: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub uvs: Vec<[f32; 2]>,
    pub indices: Vec<u32>,
}

pub struct Placement {
    pub mesh: usize,
    /// Column-major 4x4 (glam `Mat4::from_cols_array`) in Y-up space.
    pub transform: [f32; 16],
    /// UE3 row-major FMatrix as read (row vectors).
    pub ue_matrix: [f32; 16],
    pub materials: Vec<String>,
    pub vertex_light: Option<Vec<[f32; 3]>>,
}

pub struct Scene {
    pub dir: PathBuf,
    pub desc: SceneDesc,
    pub meshes: Vec<MeshData>,
    pub placements: Vec<Placement>,
}

/// UE3 Z-up left-handed -> Y-up right-handed: swap Y and Z.
#[inline]
pub fn swz(v: [f32; 3]) -> [f32; 3] {
    [v[0], v[2], v[1]]
}

/// Row-major UE3 FMatrix (row vectors) -> column-major matrix in Y-up space: B = C * M^T * C.
pub fn ue_to_yup(m: &[f32; 16]) -> [f32; 16] {
    let s = |i: usize| match i {
        1 => 2,
        2 => 1,
        x => x,
    };
    let mut out = [0f32; 16];
    for j in 0..4 {
        for i in 0..4 {
            out[j * 4 + i] = m[s(j) * 4 + s(i)];
        }
    }
    out
}

fn f32s(b: &[u8]) -> impl Iterator<Item = f32> + '_ {
    b.chunks_exact(4).map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
}

impl Scene {
    pub fn load(dir: &Path) -> Result<Self> {
        let desc: SceneDesc = serde_json::from_slice(&std::fs::read(dir.join("scene.json")).context("scene.json")?)?;
        let mb = std::fs::read(dir.join("meshes.bin")).context("meshes.bin")?;
        let lb = std::fs::read(dir.join("light.bin")).unwrap_or_default();
        let meshes = desc
            .meshes
            .iter()
            .map(|m| {
                let nv = m.vertex_count as usize;
                let o = m.vertex_offset as usize;
                let pos: Vec<f32> = f32s(&mb[o..o + nv * 12]).collect();
                let nrm: Vec<f32> = f32s(&mb[o + nv * 12..o + nv * 24]).collect();
                let uvs: Vec<[f32; 2]> = if m.has_uv {
                    f32s(&mb[o + nv * 24..o + nv * 32]).collect::<Vec<_>>().chunks_exact(2).map(|c| [c[0], c[1]]).collect()
                } else {
                    vec![[0., 0.]; nv]
                };
                let io = m.index_offset as usize;
                let mut idx: Vec<u32> = mb[io..io + m.index_count as usize * 2]
                    .chunks_exact(2)
                    .map(|c| u16::from_le_bytes([c[0], c[1]]) as u32)
                    .collect();
                // The Y/Z swap is a reflection: reverse each triangle so front faces stay front faces.
                // UE3 culls clockwise (front faces are CCW in its left-handed view space); the Y/Z swap mirrors the
                // world and the right-handed camera mirrors it back, so screen-space winding is unchanged and Bevy's
                // CCW front faces match as stored. (Checked against no-cull renders: flipping hides floors/roofs.)
                if std::env::var("N1_FLIP_WINDING").is_ok() {
                    for t in idx.chunks_exact_mut(3) {
                        t.swap(1, 2);
                    }
                }
                MeshData {
                    positions: pos.chunks_exact(3).map(|c| swz([c[0], c[1], c[2]])).collect(),
                    normals: nrm.chunks_exact(3).map(|c| swz([c[0], c[1], c[2]])).collect(),
                    uvs,
                    indices: idx,
                }
            })
            .collect();
        let placements = desc
            .placements
            .iter()
            .map(|p| {
                let nv = desc.meshes[p.mesh as usize].vertex_count as usize;
                Placement {
                    mesh: p.mesh as usize,
                    transform: ue_to_yup(&p.transform),
                    ue_matrix: p.transform,
                    materials: p.materials.clone(),
                    vertex_light: p.light_offset.map(|o| {
                        let o = o as usize;
                        f32s(&lb[o..o + nv * 12]).collect::<Vec<_>>().chunks_exact(3).map(|c| [c[0], c[1], c[2]]).collect()
                    }),
                }
            })
            .collect();
        Ok(Self { dir: dir.to_path_buf(), desc, meshes, placements })
    }

    /// Replace placements with a live-scene list: `[{"mesh": "...", "transform": [16]}, ...]`.
    /// Mesh names are matched exactly, then by dropping leading path components (the live name may carry the
    /// cooked package as prefix). Returns (matched, unmatched).
    pub fn load_placements_json(&mut self, path: &Path) -> Result<(usize, usize)> {
        #[derive(Deserialize)]
        struct Live {
            mesh: String,
            transform: [f32; 16],
            #[serde(default)]
            materials: Vec<String>,
        }
        let v: serde_json::Value = serde_json::from_slice(&std::fs::read(path)?)?;
        let arr = if let Some(a) = v.as_array() { a.clone() } else { v.get("primitives").or(v.get("placements")).and_then(|x| x.as_array()).cloned().unwrap_or_default() };
        let by_name: HashMap<&str, usize> = self.desc.meshes.iter().enumerate().map(|(i, m)| (m.name.as_str(), i)).collect();
        let (mut ok, mut miss) = (0, 0);
        let mut out = Vec::new();
        for e in arr {
            let Ok(l) = serde_json::from_value::<Live>(e) else { miss += 1; continue };
            let mut name = l.mesh.as_str();
            if let Some(p) = name.find(' ') {
                name = &name[p + 1..]; // "StaticMesh Pkg.Obj" style
            }
            let mut found = by_name.get(name).copied();
            let mut n = name;
            while found.is_none() {
                let Some(p) = n.find('.') else { break };
                n = &n[p + 1..];
                found = by_name.get(n).copied();
            }
            let Some(mi) = found else { miss += 1; continue };
            ok += 1;
            let mats = if l.materials.is_empty() {
                self.desc.meshes[mi].sections.iter().map(|s| s.material.clone()).collect()
            } else {
                l.materials
            };
            out.push(Placement { mesh: mi, transform: ue_to_yup(&l.transform), ue_matrix: l.transform, materials: mats, vertex_light: None });
        }
        self.placements = out;
        Ok((ok, miss))
    }

    pub fn material(&self, name: &str) -> MaterialDesc {
        self.desc.materials.get(name).cloned().unwrap_or_default()
    }

    /// Static batching: pre-transform every placement section into world space and merge by (material, grid cell).
    /// Returns batches with u32 indices and per-vertex colors (baked light, or `ambient` where none).
    pub fn merged(&self, cell: f32, ambient: f32) -> Vec<Batch> {
        let mut map: HashMap<(String, i32, i32, i32), Batch> = HashMap::new();
        for p in &self.placements {
            let md = &self.meshes[p.mesh];
            let desc = &self.desc.meshes[p.mesh];
            let m = &p.transform; // column-major
            let tp = |v: [f32; 3]| -> [f32; 3] {
                [
                    m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12],
                    m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13],
                    m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14],
                ]
            };
            let tn = |v: [f32; 3]| -> [f32; 3] {
                let r = [m[0] * v[0] + m[4] * v[1] + m[8] * v[2], m[1] * v[0] + m[5] * v[1] + m[9] * v[2], m[2] * v[0] + m[6] * v[1] + m[10] * v[2]];
                let l = (r[0] * r[0] + r[1] * r[1] + r[2] * r[2]).sqrt().max(1e-6);
                [r[0] / l, r[1] / l, r[2] / l]
            };
            let det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
            // centre of the placement picks the cell
            let c = tp([
                (desc.bounds_min[0] + desc.bounds_max[0]) * 0.5,
                (desc.bounds_min[2] + desc.bounds_max[2]) * 0.5,
                (desc.bounds_min[1] + desc.bounds_max[1]) * 0.5,
            ]);
            let key_cell = ((c[0] / cell).floor() as i32, (c[1] / cell).floor() as i32, (c[2] / cell).floor() as i32);
            for (si, s) in desc.sections.iter().enumerate() {
                let mat = p.materials.get(si).cloned().unwrap_or_else(|| s.material.clone());
                let b = map.entry((mat.clone(), key_cell.0, key_cell.1, key_cell.2)).or_insert_with(|| Batch { material: mat, ..Default::default() });
                let idx = &md.indices[s.first_index as usize..(s.first_index + s.index_count) as usize];
                let mut remap: HashMap<u32, u32> = HashMap::new();
                for t in idx.chunks_exact(3) {
                    let tri = if det < 0.0 { [t[0], t[2], t[1]] } else { [t[0], t[1], t[2]] };
                    for vi in tri {
                        let ni = *remap.entry(vi).or_insert_with(|| {
                            let n = b.positions.len() as u32;
                            let v = vi as usize;
                            b.positions.push(tp(md.positions[v]));
                            b.normals.push(tn(md.normals[v]));
                            b.uvs.push(md.uvs[v]);
                            let col = p.vertex_light.as_ref().map(|l| l[v]).unwrap_or([ambient; 3]);
                            b.colors.push([col[0], col[1], col[2], 1.0]);
                            n
                        });
                        b.indices.push(ni);
                    }
                }
            }
        }
        let mut v: Vec<Batch> = map.into_values().filter(|b| !b.indices.is_empty()).collect();
        v.sort_by(|a, b| a.material.cmp(&b.material));
        v
    }
}

#[derive(Default)]
pub struct Batch {
    pub material: String,
    pub positions: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub uvs: Vec<[f32; 2]>,
    pub colors: Vec<[f32; 4]>,
    pub indices: Vec<u32>,
}

/// Camera pose in UE3 units: location + yaw/pitch (degrees). Direction in Y-up space.
pub fn ue_dir(yaw_deg: f32, pitch_deg: f32) -> [f32; 3] {
    let (y, p) = (yaw_deg.to_radians(), pitch_deg.to_radians());
    swz([p.cos() * y.cos(), p.cos() * y.sin(), p.sin()])
}
