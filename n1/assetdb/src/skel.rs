//! Skeletal meshes for the live renderer (N7's parser, `ue3::skel`): LOD0 with every chunk's BoneMap folded into the
//! vertices, plus what posing needs (parents, reference pose, inverse reference bases). Parsed on demand, kept in
//! memory (a mesh parses in a few milliseconds; nothing is written to the disk cache).

use crate::AssetDb;
use anyhow::{Result, bail};
use std::sync::Arc;

/// One skinned vertex as the GPU takes it: 40 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, bytemuck::Pod, bytemuck::Zeroable)]
pub struct SkinVertex {
    pub pos: [f32; 3],
    pub normal: [f32; 3],
    pub uv: [f32; 2],
    /// Mesh bone indices (BoneMap applied).
    pub bones: [u8; 4],
    /// Weights, summing to 255.
    pub weights: [u8; 4],
}

#[derive(Clone, Debug)]
pub struct SkelSectionAsset {
    pub first_index: u32,
    pub index_count: u32,
    /// Index into `SkelAsset::materials`.
    pub material: usize,
}

#[derive(Clone, Debug)]
pub struct SkelAsset {
    pub name: String,
    pub verts: Vec<SkinVertex>,
    pub indices: Vec<u32>,
    pub sections: Vec<SkelSectionAsset>,
    /// Material object paths per mesh slot ("" = none).
    pub materials: Vec<String>,
    pub parents: Vec<i32>,
    /// Reference pose per bone: local rotation quaternion (X Y Z W) and translation.
    pub ref_local: Vec<([f32; 4], [f32; 3])>,
    /// `RefBasesInvMatrix`, UE3 row-major.
    pub inv_ref: Vec<[f32; 16]>,
    /// B-265: per LOD, which bones it needs (`FStaticLODModel::RequiredBones` as a flag per bone). The game updates
    /// only the current LOD's bones in `SpaceBases`; the rest keep stale values, which LOD0 (what n1 draws) still uses.
    pub lod_required: Vec<Vec<bool>>,
}

impl AssetDb {
    /// LOD0 of a SkeletalMesh by object path; `None` when no package has it (or it fails to parse).
    pub fn skeletal(&self, path: &str) -> Option<Arc<SkelAsset>> {
        if let Some(m) = self.skeletals.lock().unwrap().get(path) {
            return m.clone();
        }
        let r = self.load_skeletal(path).unwrap_or_else(|e| {
            if self.verbose {
                eprintln!("assetdb: skeletal mesh {path}: {e:#}");
            }
            None
        });
        self.skeletals
            .lock()
            .unwrap()
            .insert(path.to_string(), r.clone());
        r
    }

    fn load_skeletal(&self, path: &str) -> Result<Option<Arc<SkelAsset>>> {
        let Some((pkg, idx)) = self.locate(path, false)? else {
            return Ok(None);
        };
        if pkg.class_name(idx) != "SkeletalMesh" {
            bail!("{path} is a {}", pkg.class_name(idx));
        }
        let m = ue3::skel::read_skeletal_mesh(&pkg, idx)?;
        if m.bones.len() > 256 {
            bail!("{} bones (u8 indices)", m.bones.len());
        }
        let Some(l) = m.lods.first() else {
            bail!("no LODs")
        };
        let mut verts: Vec<SkinVertex> = l
            .verts
            .iter()
            .map(|v| SkinVertex {
                pos: v.pos,
                normal: v.normal,
                uv: v.uv,
                bones: [0; 4],
                weights: v.bone_w,
            })
            .collect();
        for c in &l.chunks {
            let end = ((c.first_vertex + c.num_rigid + c.num_soft) as usize).min(verts.len());
            for v in c.first_vertex as usize..end {
                for k in 0..4 {
                    let local = l.verts[v].bone_idx[k] as usize;
                    verts[v].bones[k] = if verts[v].weights[k] == 0 {
                        0
                    } else {
                        *c.bones.get(local).unwrap_or(&0) as u8
                    };
                }
            }
        }
        let flat = |x: &[[f32; 4]; 4]| {
            let mut o = [0f32; 16];
            for r in 0..4 {
                o[r * 4..r * 4 + 4].copy_from_slice(&x[r]);
            }
            o
        };
        Ok(Some(Arc::new(SkelAsset {
            name: path.to_string(),
            verts,
            indices: l.indices.iter().map(|&i| i as u32).collect(),
            sections: l
                .sections
                .iter()
                .map(|s| SkelSectionAsset {
                    first_index: s.first_index,
                    index_count: s.num_tris * 3,
                    material: s.material.max(0) as usize,
                })
                .collect(),
            materials: m.materials.clone(),
            parents: m.bones.iter().map(|b| b.parent).collect(),
            ref_local: m.bones.iter().map(|b| (b.rot, b.pos)).collect(),
            inv_ref: m.inv_ref_bases().iter().map(flat).collect(),
            lod_required: m
                .lods
                .iter()
                .map(|l| required_flags(m.bones.len(), &l.required_bones))
                .collect(),
        })))
    }
}

/// `RequiredBones` (bone indices) as one flag per bone; out-of-range indices are ignored.
pub fn required_flags(bones: usize, required: &[u8]) -> Vec<bool> {
    let mut f = vec![false; bones];
    for &b in required {
        if let Some(x) = f.get_mut(b as usize) {
            *x = true;
        }
    }
    f
}

#[cfg(test)]
mod normal_probe {
    /// Agreement of the decoded vertex normals with the triangle face normals (N12; `SKEL_NORMAL_PROBE=1`).
    #[test]
    #[ignore]
    fn skel_normal_probe() {
        if std::env::var("SKEL_NORMAL_PROBE").is_err() {
            return;
        }
        let cooked = std::env::var("COOKED").unwrap();
        let db = crate::AssetDb::open(
            std::path::Path::new(&cooked),
            std::path::Path::new("../cache/n12"),
        )
        .unwrap();
        for p in [
            "Ch_Type1_50Cent.Meshes.Char_Body",
            "AST_WeaponsEmplacement.MiniGun01_SKN",
            "AST_VehiclesHelicopter.BlackHawk03_SKN",
        ] {
            let Some(a) = db.skeletal(p) else {
                println!("{p}: missing");
                continue;
            };
            let (mut agree, mut n) = (0usize, 0usize);
            for t in a.indices.chunks_exact(3) {
                let v = |i: u32| a.verts[i as usize].pos;
                let (p0, p1, p2) = (v(t[0]), v(t[1]), v(t[2]));
                let e1 = [p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]];
                let e2 = [p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]];
                let f = [
                    e1[1] * e2[2] - e1[2] * e2[1],
                    e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0],
                ];
                let nn = a.verts[t[0] as usize].normal;
                let d = f[0] * nn[0] + f[1] * nn[1] + f[2] * nn[2];
                n += 1;
                if d > 0.0 {
                    agree += 1;
                }
            }
            println!(
                "{p}: {} tris, {:.1}% agree (cross(e1,e2) . n > 0)",
                n,
                100.0 * agree as f64 / n as f64
            );
        }
    }
}
