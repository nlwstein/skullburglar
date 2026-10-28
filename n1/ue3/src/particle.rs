//! UParticleSystem -> UParticleEmitter -> UParticleLODLevel -> RequiredModule / TypeDataModule (v538).
//!
//! The scene feed already carries each emitter instance's material, screen alignment and SubImages, so this parser
//! only adds what the instance doesn't: the emitter's **SortMode**, **MaxDrawCount**, the mesh emitter's
//! **MeshAlignment/RollPitchYaw** options, and a look at the full module list (for the doc's census). Everything is
//! read from LOD level 0 (the level the game spawns from; the feed reports `CurrentLODLevel`'s values for the rest).

use crate::package::Package;
use crate::props::{object_props, read_fname, Props};
use anyhow::{Context, Result};
use serde::Serialize;

#[derive(Serialize, Default, Clone, Debug)]
pub struct EmitterInfo {
    /// Full object path of the UParticleEmitter, as the feed's `emitter` string.
    pub path: String,
    pub class: String,
    pub name: String,
    pub material: String,
    /// `PSORTMODE_*` (None, ViewProjDepth, DistanceToView, Age_OldestFirst, Age_NewestFirst); None when unset.
    pub sort_mode: String,
    /// `PSA_*` (Square, Rectangle, Velocity, TypeSpecific).
    pub screen_alignment: String,
    pub sub_images: [i32; 2],
    /// `PSUVIM_*`.
    pub interpolation_method: String,
    pub use_local_space: bool,
    pub use_max_draw_count: bool,
    pub max_draw_count: i32,
    pub emitter_normals_mode: String,
    /// TypeDataModule class (empty for a plain sprite emitter).
    pub type_data: String,
    pub mesh: String,
    pub module_classes: Vec<String>,
    pub lod_levels: usize,
}

fn enum_val(p: &Package, props: &Props, n: &str) -> String {
    props.get(n).and_then(|x| if x.data.len() == 8 { read_fname(p, &mut x.r()).ok() } else { None }).unwrap_or_default()
}

pub fn read_emitter(p: &Package, idx: i32) -> Result<EmitterInfo> {
    let (props, _) = object_props(p, idx)?;
    let mut e = EmitterInfo { path: p.obj_path(idx), class: p.class_name(idx).to_string(), sub_images: [1, 1], ..Default::default() };
    if let Some(n) = props.get("EmitterName") {
        e.name = read_fname(p, &mut n.r()).unwrap_or_default();
    }
    let lods = props.get("LODLevels").map(|x| x.as_obj_array()).unwrap_or_default();
    e.lod_levels = lods.len();
    let lod0 = *lods.first().context("no LODLevels")?;
    let (lp, _) = object_props(p, lod0)?;
    let req = lp.get("RequiredModule").map(|x| x.as_obj()).filter(|&o| o != 0).context("no RequiredModule")?;
    let (rp, _) = object_props(p, req)?;
    let mat = rp.get("Material").map(|x| x.as_obj()).unwrap_or(0);
    if mat != 0 {
        e.material = p.obj_path(mat);
    }
    e.sort_mode = enum_val(p, &rp, "SortMode");
    e.screen_alignment = enum_val(p, &rp, "ScreenAlignment");
    e.interpolation_method = enum_val(p, &rp, "InterpolationMethod");
    e.emitter_normals_mode = enum_val(p, &rp, "EmitterNormalsMode");
    e.use_local_space = rp.get("bUseLocalSpace").is_some_and(|x| x.bool_value);
    e.use_max_draw_count = rp.get("bUseMaxDrawCount").is_none_or(|x| x.bool_value);
    e.max_draw_count = rp.get("MaxDrawCount").map(|x| x.as_i32()).unwrap_or(500);
    for (k, slot) in [("SubImages_Horizontal", 0usize), ("SubImages_Vertical", 1)] {
        if let Some(x) = rp.get(k) {
            e.sub_images[slot] = x.as_i32().max(1);
        }
    }
    e.module_classes = lp.get("Modules").map(|x| x.as_obj_array()).unwrap_or_default().iter().map(|&m| p.class_name(m).to_string()).collect();
    if let Some(td) = lp.get("TypeDataModule").map(|x| x.as_obj()).filter(|&o| o != 0) {
        e.type_data = p.class_name(td).to_string();
        let (tp, _) = object_props(p, td)?;
        if let Some(m) = tp.get("Mesh").map(|x| x.as_obj()).filter(|&o| o != 0) {
            e.mesh = p.obj_path(m);
        }
    }
    Ok(e)
}

/// Every UParticleEmitter export of a package (sprite, mesh and any other subclass), by object path.
pub fn emitters(p: &Package) -> Vec<(i32, String)> {
    (1..=p.exports.len() as i32).filter(|&i| p.class_name(i).starts_with("Particle") && p.class_name(i).ends_with("Emitter")).map(|i| (i, p.obj_path(i))).collect()
}
