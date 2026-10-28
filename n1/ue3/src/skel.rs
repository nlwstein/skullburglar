//! USkeletalMesh (v538 / licensee 73, 50 Cent: Blood on the Sand, Xenon cook).
//!
//! Field order follows the generic UE3 path in UEViewer's `UnMesh3.cpp` (USkeletalMesh3, FStaticLODModel3,
//! FSkelMeshChunk3, FGPUVert3*), written from scratch. BotS-specific facts, found by byte accounting:
//! - `OnFireMaterials`: a second TArray<UMaterialInterface*> after `Materials` (licensee >= 65; UEViewer knows it).
//! - The LOD model stops after the GPU skin vertex buffer: no ExtraVertexInfluences although ArVer >= 534
//!   (UEViewer: "new ArVer, but old engine").

use crate::mesh::{half, unpack_normal};
use crate::package::{Package, Reader};
use crate::props::object_props;
use anyhow::{Context, Result, ensure};

#[derive(Debug, Clone)]
pub struct Bone {
    pub name: String,
    pub flags: u32,
    /// Local (parent-space) rotation quaternion, X Y Z W as stored.
    pub rot: [f32; 4],
    pub pos: [f32; 3],
    pub num_children: i32,
    pub parent: i32,
}

#[derive(Debug, Clone, Default)]
pub struct SkelSection {
    pub material: i16,
    pub chunk: i16,
    pub first_index: u32,
    pub num_tris: u32,
}

#[derive(Debug, Clone, Default)]
pub struct Chunk {
    pub first_vertex: u32,
    /// Chunk-local bone index -> mesh bone index (the BoneMap).
    pub bones: Vec<u16>,
    pub num_rigid: u32,
    pub num_soft: u32,
    pub max_influences: i32,
}

/// One GPU-skin vertex. Influence indices are chunk-local (index into `Chunk::bones`).
#[derive(Debug, Clone, Copy, Default)]
pub struct SkinVert {
    pub pos: [f32; 3],
    pub normal: [f32; 3],
    pub tangent: [f32; 4],
    pub uv: [f32; 2],
    pub bone_idx: [u8; 4],
    pub bone_w: [u8; 4],
}

#[derive(Debug, Clone, Default)]
pub struct SkelLod {
    pub sections: Vec<SkelSection>,
    pub chunks: Vec<Chunk>,
    pub indices: Vec<u16>,
    pub active_bones: Vec<i16>,
    pub required_bones: Vec<u8>,
    pub num_vertices: u32,
    pub full_precision_uvs: bool,
    pub verts: Vec<SkinVert>,
}

#[derive(Debug, Clone, Default)]
pub struct SkeletalMesh {
    pub bounds_origin: [f32; 3],
    pub bounds_extent: [f32; 3],
    pub sphere_radius: f32,
    /// Material object paths (package-local), "" = none.
    pub materials: Vec<String>,
    pub on_fire_materials: Vec<String>,
    pub mesh_origin: [f32; 3],
    pub rot_origin: [i32; 3],
    pub bones: Vec<Bone>,
    pub skeletal_depth: i32,
    pub lods: Vec<SkelLod>,
    /// Bytes after the LOD array (NameIndexMap, PerPolyKDOPs, licensee extras).
    pub trailing: usize,
}

fn obj_array(pkg: &Package, r: &mut Reader) -> Result<Vec<String>> {
    let n = r.i32()?;
    ensure!((0..4096).contains(&n), "bad object array count {n}");
    (0..n).map(|_| Ok(match r.i32()? { 0 => String::new(), o => pkg.obj_path(o) })).collect()
}

fn i16_array(r: &mut Reader) -> Result<Vec<i16>> {
    let (n, d) = r.tarray_raw(2)?;
    Ok((0..n).map(|i| i16::from_be_bytes([d[2 * i], d[2 * i + 1]])).collect())
}

pub fn read_skeletal_mesh(pkg: &Package, idx: i32) -> Result<SkeletalMesh> {
    let (_props, mut r) = object_props(pkg, idx)?;
    let abs = pkg.exports[idx as usize - 1].serial_offset;
    let mut m = SkeletalMesh::default();
    m.bounds_origin = r.vec3()?;
    m.bounds_extent = r.vec3()?;
    m.sphere_radius = r.f32()?;
    m.materials = obj_array(pkg, &mut r).context("materials")?;
    m.on_fire_materials = obj_array(pkg, &mut r).context("on-fire materials")?;
    m.mesh_origin = r.vec3()?;
    m.rot_origin = [r.i32()?, r.i32()?, r.i32()?];
    let nb = r.i32()?;
    ensure!((1..1024).contains(&nb), "bad bone count {nb}");
    for _ in 0..nb {
        let name = crate::props::read_fname(pkg, &mut r)?;
        let flags = r.u32()?;
        let rot = [r.f32()?, r.f32()?, r.f32()?, r.f32()?];
        let pos = r.vec3()?;
        let num_children = r.i32()?;
        let parent = r.i32()?;
        let _unk = r.i32()?; // ArVer >= 515: 4 bytes, usually 0xFFFFFFFF (bone colour?)
        ensure!(parent >= 0 && parent < nb, "bone {name}: bad parent {parent}");
        m.bones.push(Bone { name, flags, rot, pos, num_children, parent });
    }
    m.skeletal_depth = r.i32()?;
    let nl = r.i32()?;
    ensure!((1..8).contains(&nl), "bad LOD count {nl}");
    for li in 0..nl {
        m.lods.push(read_lod(&mut r, abs, nb as usize).with_context(|| format!("LOD {li} at {:#x}", r.o))?);
    }
    m.trailing = r.remaining();
    Ok(m)
}

fn read_lod(r: &mut Reader, abs: usize, nbones: usize) -> Result<SkelLod> {
    let mut lod = SkelLod::default();
    let ns = r.i32()?;
    ensure!((1..256).contains(&ns), "bad section count {ns}");
    for _ in 0..ns {
        let material = r.u16()? as i16;
        let chunk = r.u16()? as i16;
        let first_index = r.u32()?;
        let num_tris = r.u16()? as u32;
        lod.sections.push(SkelSection { material, chunk, first_index, num_tris });
    }
    let (es, _n, d) = r.bulk_array().context("index buffer")?;
    ensure!(es == 2, "index size {es}");
    lod.indices = d.chunks_exact(2).map(|c| u16::from_be_bytes([c[0], c[1]])).collect();
    let _f68 = i16_array(r).context("f68")?;
    lod.active_bones = i16_array(r).context("active bones")?;
    let _f74 = r.tarray_raw(1).context("f74")?;
    let nc = r.i32()?;
    ensure!((1..256).contains(&nc), "bad chunk count {nc}");
    for _ in 0..nc {
        let first_vertex = r.u32()?;
        r.tarray_raw(33).context("rigid verts")?;
        r.tarray_raw(40).context("soft verts")?;
        let bones = i16_array(r)?.into_iter().map(|b| b as u16).collect::<Vec<_>>();
        ensure!(bones.iter().all(|&b| (b as usize) < nbones), "chunk bone out of range");
        let num_rigid = r.u32()?;
        let num_soft = r.u32()?;
        let max_influences = r.i32()?;
        lod.chunks.push(Chunk { first_vertex, bones, num_rigid, num_soft, max_influences });
    }
    let _f80 = r.i32()?;
    lod.num_vertices = r.u32()?;
    r.tarray_raw(16).context("edges")?;
    lod.required_bones = r.tarray_raw(1).context("required bones")?.1.to_vec();
    crate::mesh::skip_bulk(r, abs).context("raw point indices bulk")?;
    // FSkeletalMeshVertexBuffer (ArVer 493..592): bUseFullPrecisionUVs, then BulkSerialize'd verts.
    lod.full_precision_uvs = r.i32()? != 0;
    let (es, n, d) = r.bulk_array().context("GPU skin verts")?;
    let want = if lod.full_precision_uvs { 36 } else { 32 };
    ensure!(es == want, "GPU vert size {es}, expected {want}");
    ensure!(n as u32 == lod.num_vertices, "GPU verts {n} != NumVertices {}", lod.num_vertices);
    for c in d.chunks_exact(es) {
        let mut rr = Reader::new(c, 0);
        let pos = rr.vec3()?;
        // Xenon GPU verts store the packed normals and the influence bytes as big-endian dwords of the PC
        // little-endian layout: bytes are W Z Y X, and influence slot 0 is the *last* byte. `unpack_normal` already
        // undoes the reversal (N12: reversing here too gave random normals, 45-54% face agreement; now 99.7%).
        let t = unpack_normal(&c[12..16]);
        let nz = unpack_normal(&c[16..20]);
        let bone_idx = [c[23], c[22], c[21], c[20]];
        let bone_w = [c[27], c[26], c[25], c[24]];
        let mut rr = Reader::new(c, 28);
        let uv = if lod.full_precision_uvs { [rr.f32()?, rr.f32()?] } else { [half(rr.u16()?), half(rr.u16()?)] };
        lod.verts.push(SkinVert {
            pos,
            normal: [nz[0], nz[1], nz[2]],
            tangent: [t[0], t[1], t[2], if nz[3] < 0.0 { -1.0 } else { 1.0 }],
            uv,
            bone_idx,
            bone_w,
        });
    }
    Ok(lod)
}

/// UE3 `FQuatRotationTranslationMatrix`: row-major 4x4, row vectors (v' = v * M), translation in row 3.
pub fn quat_trans_matrix(q: [f32; 4], t: [f32; 3]) -> [[f32; 4]; 4] {
    let [x, y, z, w] = q;
    let (x2, y2, z2) = (x + x, y + y, z + z);
    let (xx, xy, xz) = (x * x2, x * y2, x * z2);
    let (yy, yz, zz) = (y * y2, y * z2, z * z2);
    let (wx, wy, wz) = (w * x2, w * y2, w * z2);
    [
        [1.0 - (yy + zz), xy + wz, xz - wy, 0.0],
        [xy - wz, 1.0 - (xx + zz), yz + wx, 0.0],
        [xz + wy, yz - wx, 1.0 - (xx + yy), 0.0],
        [t[0], t[1], t[2], 1.0],
    ]
}

pub fn mat_mul(a: &[[f32; 4]; 4], b: &[[f32; 4]; 4]) -> [[f32; 4]; 4] {
    let mut o = [[0f32; 4]; 4];
    for i in 0..4 {
        for j in 0..4 {
            o[i][j] = (0..4).map(|k| a[i][k] * b[k][j]).sum();
        }
    }
    o
}

/// Inverse of an affine row-vector matrix (rotation/scale in the upper 3x3, translation in row 3), in f64.
pub fn affine_inverse(m: &[[f32; 4]; 4]) -> [[f32; 4]; 4] {
    let a = |i: usize, j: usize| m[i][j] as f64;
    let det = a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) - a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0))
        + a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
    let mut inv = [[0f64; 3]; 3];
    inv[0][0] = (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) / det;
    inv[0][1] = (a(0, 2) * a(2, 1) - a(0, 1) * a(2, 2)) / det;
    inv[0][2] = (a(0, 1) * a(1, 2) - a(0, 2) * a(1, 1)) / det;
    inv[1][0] = (a(1, 2) * a(2, 0) - a(1, 0) * a(2, 2)) / det;
    inv[1][1] = (a(0, 0) * a(2, 2) - a(0, 2) * a(2, 0)) / det;
    inv[1][2] = (a(0, 2) * a(1, 0) - a(0, 0) * a(1, 2)) / det;
    inv[2][0] = (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0)) / det;
    inv[2][1] = (a(0, 1) * a(2, 0) - a(0, 0) * a(2, 1)) / det;
    inv[2][2] = (a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0)) / det;
    let mut o = [[0f32; 4]; 4];
    for i in 0..3 {
        for j in 0..3 {
            o[i][j] = inv[i][j] as f32;
        }
    }
    for j in 0..3 {
        o[3][j] = -(0..3).map(|k| a(3, k) * inv[k][j]).sum::<f64>() as f32;
    }
    o[3][3] = 1.0;
    o
}

impl SkeletalMesh {
    /// Component-space reference pose of every bone (UE3 `RefBases`): local * parent, root first.
    pub fn ref_bases(&self) -> Vec<[[f32; 4]; 4]> {
        let mut out: Vec<[[f32; 4]; 4]> = Vec::with_capacity(self.bones.len());
        for (i, b) in self.bones.iter().enumerate() {
            let local = quat_trans_matrix(b.rot, b.pos);
            out.push(if i == 0 { local } else { mat_mul(&local, &out[b.parent as usize]) });
        }
        out
    }

    /// `RefBasesInvMatrix`: what the game multiplies with SpaceBases to get each bone's skinning matrix.
    pub fn inv_ref_bases(&self) -> Vec<[[f32; 4]; 4]> {
        self.ref_bases().iter().map(affine_inverse).collect()
    }

    /// Consistency checks on LOD `li`: weights sum to 255, influence indices inside the chunk's BoneMap,
    /// chunk vertex ranges tile the buffer, section indices stay inside their chunk. Returns problems found.
    pub fn validate(&self, li: usize) -> Vec<String> {
        let l = &self.lods[li];
        let mut errs = Vec::new();
        let mut next = 0u32;
        for (ci, c) in l.chunks.iter().enumerate() {
            if c.first_vertex != next {
                errs.push(format!("chunk {ci} first {} != {next}", c.first_vertex));
            }
            next = c.first_vertex + c.num_rigid + c.num_soft;
            for v in &l.verts[c.first_vertex as usize..(next as usize).min(l.verts.len())] {
                let sum: u32 = v.bone_w.iter().map(|&w| w as u32).sum();
                if sum != 255 {
                    errs.push(format!("chunk {ci}: weight sum {sum}"));
                    break;
                }
                if (0..4).any(|k| v.bone_w[k] != 0 && v.bone_idx[k] as usize >= c.bones.len()) {
                    errs.push(format!("chunk {ci}: influence index out of BoneMap"));
                    break;
                }
            }
        }
        if next as usize != l.verts.len() {
            errs.push(format!("chunks cover {next} of {} verts", l.verts.len()));
        }
        for (si, s) in l.sections.iter().enumerate() {
            let Some(c) = l.chunks.get(s.chunk as usize) else {
                errs.push(format!("section {si}: bad chunk"));
                continue;
            };
            let (lo, hi) = (c.first_vertex, c.first_vertex + c.num_rigid + c.num_soft);
            let end = (s.first_index + s.num_tris * 3) as usize;
            if end > l.indices.len() || l.indices[s.first_index as usize..end].iter().any(|&i| (i as u32) < lo || (i as u32) >= hi) {
                errs.push(format!("section {si}: indices outside chunk {}", s.chunk));
            }
        }
        errs
    }
}
