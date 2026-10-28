//! SpeedTree (N11): `SpeedTreeActor` placements, their `SpeedTreeComponent`'s baked vertex light, and the
//! `SpeedTree` assets' materials.
//!
//! The geometry is NOT here: a `SpeedTree` export only holds the SpeedTree SDK's own `.spt` description
//! (`__IdvSpt_02_` token stream of Bezier parameter curves, 12-21 KB), from which the SpeedTreeRT library compiled
//! into the XEX generates branches, fronds and leaves at load. n1 gets that generated geometry from the port's
//! draws instead (n1ffi `trees.rs`).

use crate::level::{Mat4, actor_matrix, mul, srt};
use crate::package::{Package, Reader};
use crate::props::{Props, object_props};
use anyhow::{Result, bail, ensure};

/// One FLightMap1D: 3 directional FColor samples per vertex (bytes A,R,G,B each) and 3 RGB scales.
#[derive(Debug, Clone)]
pub struct VertexLight {
    pub samples: Vec<[u8; 12]>,
    pub scale: [[f32; 3]; 3],
}

#[derive(Debug, Clone)]
pub struct TreePlacement {
    pub package: String,
    /// Component object path inside the package, e.g. `TheWorld.PersistentLevel.SpeedTreeActor_0.SpeedTreeComponent_27`
    /// (the scene feed's primitive name is `<package>.<component>`).
    pub component: String,
    pub actor: String,
    /// SpeedTree asset object path, e.g. `AST_FoliageTree.Shrub04`.
    pub tree: String,
    /// Full LocalToWorld including rotation (UE3 row-vector FMatrix).
    pub transform: Mat4,
    /// The actor's rotation (pitch, yaw, roll); UE3 keeps it out of the component's LocalToWorld for SpeedTree and
    /// passes it as a separate rotation matrix (see the n11 doc).
    pub rotation: [i32; 3],
    /// Uniform-ish scale and location as in the scene feed's transform (no rotation).
    pub scale: [f32; 3],
    pub location: [f32; 3],
    /// Baked light per geometry kind in serialization order (branch, frond, leaf card, billboard, ...); None where the
    /// component stores no 1D lightmap for that kind.
    pub lights: Vec<Option<VertexLight>>,
    pub light_error: Option<String>,
}

#[derive(Debug, Clone, Default)]
pub struct TreeAsset {
    pub path: String,
    pub branch_material: String,
    pub frond_material: String,
    pub leaf_material: String,
    pub billboard_material: String,
    pub spt_bytes: usize,
    pub random_seed: i32,
}

fn obj_path_or_empty(pkg: &Package, p: &Props, name: &str) -> String {
    match p.get(name).map(|p| p.as_obj()) {
        Some(o) if o != 0 => pkg.obj_path(o),
        _ => String::new(),
    }
}

pub fn tree_assets(pkg: &Package) -> Vec<TreeAsset> {
    pkg.exports_of_class("SpeedTree")
        .collect::<Vec<_>>()
        .into_iter()
        .filter_map(|i| {
            let (p, _) = object_props(pkg, i).ok()?;
            let total = pkg.export_data(i).len();
            Some(TreeAsset {
                path: pkg.obj_path(i),
                branch_material: obj_path_or_empty(pkg, &p, "BranchMaterial"),
                frond_material: obj_path_or_empty(pkg, &p, "FrondMaterial"),
                leaf_material: obj_path_or_empty(pkg, &p, "LeafMaterial"),
                billboard_material: obj_path_or_empty(pkg, &p, "BillboardMaterial"),
                spt_bytes: total.saturating_sub(p.end),
                random_seed: p.get("RandomSeed").map(|p| p.as_i32()).unwrap_or(0),
            })
        })
        .collect()
}

fn read_light_1d(r: &mut Reader, abs: usize) -> Result<Option<VertexLight>> {
    let ty = r.u32()?;
    match ty {
        0 => Ok(None),
        1 => {
            let n = r.i32()?;
            ensure!((0..1024).contains(&n), "bad light guid count {n}");
            r.skip(n as usize * 16)?;
            let _owner = r.i32()?;
            let flags = r.u32()?;
            let count = r.i32()?;
            let size = r.i32()?;
            let off = r.i32()?;
            ensure!(flags & 0x21 == 0 && off as usize == abs + r.o, "samples not inline (flags {flags:#x})");
            ensure!(size == count * 12, "sample size {size} for {count}");
            let d = r.bytes(size as usize)?;
            // This build: 4 scale vectors (3 directional coefficients + simple), then the SimpleSamples bulk array.
            let mut scale = [[0f32; 3]; 3];
            for s in scale.iter_mut() {
                *s = r.vec3()?;
            }
            let _simple_scale = r.vec3()?;
            let _flags = r.u32()?;
            let n = r.i32()?;
            let size = r.i32()?;
            let _off = r.i32()?;
            ensure!(size >= 0 && n >= 0, "bad simple samples");
            r.skip(size as usize)?;
            Ok(Some(VertexLight { samples: d.chunks_exact(12).map(|c| <[u8; 12]>::try_from(c).unwrap()).collect(), scale }))
        }
        t => bail!("lightmap type {t}"),
    }
}

fn vec3(p: &Props, n: &str, d: [f32; 3]) -> [f32; 3] {
    p.get(n).and_then(|p| p.as_vec3()).unwrap_or(d)
}

pub fn tree_placements(pkg: &Package) -> Vec<TreePlacement> {
    let mut out = Vec::new();
    for ci in pkg.exports_of_class("SpeedTreeComponent").collect::<Vec<_>>() {
        let Ok((cp, mut r)) = object_props(pkg, ci) else { continue };
        let tree = obj_path_or_empty(pkg, &cp, "SpeedTree");
        if tree.is_empty() || cp.get("HiddenGame").map(|p| p.bool_value).unwrap_or(false) {
            continue;
        }
        let owner = pkg.exports[ci as usize - 1].outer;
        let comp_m = srt(
            {
                let s = cp.get("Scale").map(|p| p.as_f32()).unwrap_or(1.0);
                let s3 = vec3(&cp, "Scale3D", [1., 1., 1.]);
                [s * s3[0], s * s3[1], s * s3[2]]
            },
            cp.get("Rotation").and_then(|p| p.as_rot()).unwrap_or([0; 3]),
            vec3(&cp, "Translation", [0.; 3]),
        );
        let (mut transform, mut rotation, mut scale, mut location) = (comp_m, [0; 3], [1.; 3], [0.; 3]);
        if owner > 0 {
            if let Ok((ap, _)) = object_props(pkg, owner) {
                if ap.get("bHidden").map(|p| p.bool_value).unwrap_or(false) {
                    continue;
                }
                transform = mul(&comp_m, &actor_matrix(&ap));
                rotation = ap.get("Rotation").and_then(|p| p.as_rot()).unwrap_or([0; 3]);
                let ds = ap.get("DrawScale").map(|p| p.as_f32()).unwrap_or(1.0);
                let s3 = vec3(&ap, "DrawScale3D", [1., 1., 1.]);
                scale = [ds * s3[0], ds * s3[1], ds * s3[2]];
                location = vec3(&ap, "Location", [0.; 3]);
            }
        }
        let abs = pkg.exports[ci as usize - 1].serial_offset;
        let mut lights = Vec::new();
        let mut light_error = None;
        for _ in 0..5 {
            if r.remaining() < 4 {
                break;
            }
            match read_light_1d(&mut r, abs) {
                Ok(l) => lights.push(l),
                Err(e) => {
                    light_error = Some(format!("{e:#}"));
                    break;
                }
            }
        }
        out.push(TreePlacement {
            package: pkg.name.clone(),
            component: pkg.obj_path(ci),
            actor: pkg.obj_path(owner),
            tree,
            transform,
            rotation,
            scale,
            location,
            lights,
            light_error,
        });
    }
    out
}
