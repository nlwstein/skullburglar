//! Level placements: StaticMeshComponents owned by actors, with UE3 LocalToWorld (row-vector FMatrix) and the
//! per-vertex baked light (FLightMap1D directional samples) serialized after the component's properties.

use crate::package::{Package, Reader};
use crate::props::{Props, object_props};
use anyhow::{Result, ensure};

pub type Mat4 = [[f32; 4]; 4];

pub fn identity() -> Mat4 {
    [[1., 0., 0., 0.], [0., 1., 0., 0.], [0., 0., 1., 0.], [0., 0., 0., 1.]]
}

pub fn mul(a: &Mat4, b: &Mat4) -> Mat4 {
    let mut r = [[0f32; 4]; 4];
    for i in 0..4 {
        for j in 0..4 {
            r[i][j] = (0..4).map(|k| a[i][k] * b[k][j]).sum();
        }
    }
    r
}

/// FRotationMatrix(Pitch, Yaw, Roll) * FScaleMatrix applied first and FTranslationMatrix last (row vectors).
pub fn srt(scale: [f32; 3], rot: [i32; 3], trans: [f32; 3]) -> Mat4 {
    let k = std::f32::consts::PI * 2.0 / 65536.0;
    let (sp, cp) = ((rot[0] as f32) * k).sin_cos();
    let (sy, cy) = ((rot[1] as f32) * k).sin_cos();
    let (sr, cr) = ((rot[2] as f32) * k).sin_cos();
    let r = [
        [cp * cy, cp * sy, sp, 0.],
        [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp, 0.],
        [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp, 0.],
        [0., 0., 0., 1.],
    ];
    let s = [[scale[0], 0., 0., 0.], [0., scale[1], 0., 0.], [0., 0., scale[2], 0.], [0., 0., 0., 1.]];
    let mut m = mul(&s, &r);
    m[3][0] = trans[0];
    m[3][1] = trans[1];
    m[3][2] = trans[2];
    m
}

fn translate(t: [f32; 3]) -> Mat4 {
    let mut m = identity();
    m[3][0] = t[0];
    m[3][1] = t[1];
    m[3][2] = t[2];
    m
}

#[derive(Debug, Clone)]
pub struct Placement {
    pub package: String,
    pub actor: String,
    pub actor_class: String,
    pub component: String,
    pub mesh: String, // object path of the StaticMesh
    pub mesh_idx: i32,
    pub materials: Vec<String>, // per-section overrides ("" = use mesh's)
    pub transform: Mat4,
    /// Per-vertex baked light for LOD0 (RGB, already scaled), if the component has a 1D lightmap.
    pub vertex_light: Option<Vec<[f32; 3]>>,
    pub lightmap_kind: u32,
    /// Raw directional vertex light (3 FColor samples per vertex as stored: bytes A,R,G,B each) + 3 RGB scales.
    pub vertex_light_dir: Option<(Vec<[u8; 12]>, [[f32; 3]; 3])>,
    /// 2D (texture) lightmap of LOD0.
    pub lightmap2d: Option<LightMap2D>,
}

/// FLightMap2D: up to 4 coefficient textures (3 directional + simple), a RGB scale per coefficient and the
/// atlas transform (`uv_lm = uv1 * coord_scale + coord_bias`).
#[derive(Debug, Clone)]
pub struct LightMap2D {
    /// Object paths of the LightMapTexture2D exports (package-local), index 3 usually none.
    pub textures: [Option<String>; 4],
    pub texture_idx: [i32; 4],
    pub scale: [[f32; 3]; 4],
    pub coord_scale: [f32; 2],
    pub coord_bias: [f32; 2],
}

fn vec_or(p: &Props, n: &str, d: [f32; 3]) -> [f32; 3] {
    p.get(n).and_then(|p| p.as_vec3()).unwrap_or(d)
}
fn f_or(p: &Props, n: &str, d: f32) -> f32 {
    p.get(n).map(|p| p.as_f32()).unwrap_or(d)
}
fn rot_or(p: &Props, n: &str) -> [i32; 3] {
    p.get(n).and_then(|p| p.as_rot()).unwrap_or([0, 0, 0])
}
fn bool_or(p: &Props, n: &str, d: bool) -> bool {
    p.get(n).map(|p| p.bool_value).unwrap_or(d)
}

pub fn actor_matrix(p: &Props) -> Mat4 {
    let ds = f_or(p, "DrawScale", 1.0);
    let ds3 = vec_or(p, "DrawScale3D", [1., 1., 1.]);
    let pre = vec_or(p, "PrePivot", [0., 0., 0.]);
    let m = srt([ds * ds3[0], ds * ds3[1], ds * ds3[2]], rot_or(p, "Rotation"), vec_or(p, "Location", [0., 0., 0.]));
    mul(&translate([-pre[0], -pre[1], -pre[2]]), &m)
}

pub struct LightInfo {
    pub ty: u32,
    pub flat: Option<Vec<[f32; 3]>>,
    pub dir: Option<(Vec<[u8; 12]>, [[f32; 3]; 3])>,
    pub lm2d: Option<LightMap2D>,
}

/// LODData[0].LightMap: type 1 = per-vertex directional samples, type 2 = texture lightmap.
fn read_vertex_light(pkg: &Package, r: &mut Reader, abs: usize) -> Result<LightInfo> {
    let mut li = LightInfo { ty: 0, flat: None, dir: None, lm2d: None };
    let nlod = r.i32()?;
    if nlod <= 0 {
        return Ok(li);
    }
    let n = r.i32()?;
    r.skip(n.max(0) as usize * 4)?; // ShadowMaps
    let n = r.i32()?;
    r.skip(n.max(0) as usize * 4)?; // ShadowVertexBuffers
    let ty = r.u32()?;
    li.ty = ty;
    if ty == 2 {
        let n = r.i32()?;
        ensure!((0..1024).contains(&n), "bad light guid count {n}");
        r.skip(n as usize * 16)?;
        let mut lm = LightMap2D { textures: [None, None, None, None], texture_idx: [0; 4], scale: [[0.; 3]; 4], coord_scale: [1., 1.], coord_bias: [0., 0.] };
        for i in 0..4 {
            let t = r.i32()?;
            lm.texture_idx[i] = t;
            if t > 0 {
                lm.textures[i] = Some(pkg.obj_path(t));
            }
            lm.scale[i] = r.vec3()?;
        }
        lm.coord_scale = [r.f32()?, r.f32()?];
        lm.coord_bias = [r.f32()?, r.f32()?];
        li.lm2d = Some(lm);
        return Ok(li);
    }
    if ty != 1 {
        return Ok(li);
    }
    let n = r.i32()?;
    ensure!((0..1024).contains(&n), "bad light guid count {n}");
    r.skip(n as usize * 16)?;
    let _owner = r.i32()?;
    let flags = r.u32()?;
    let count = r.i32()?;
    let size = r.i32()?;
    let off = r.i32()?;
    ensure!(flags & 0x21 == 0 && off as usize == abs + r.o, "directional samples not inline (flags {flags:#x})");
    ensure!(size == count * 12, "sample size {size} for {count}");
    let d = r.bytes(size as usize)?;
    let mut scale = [[0f32; 3]; 3];
    for s in scale.iter_mut() {
        *s = r.vec3()?;
    }
    // FColor stored as big-endian ARGB dword → bytes A R G B.
    let inv = 1.0 / 255.0;
    let k = 1.0 / 3f32.sqrt(); // dot(flat tangent-space normal, each SH-ish basis vector)
    let v = d
        .chunks_exact(12)
        .map(|c| {
            let mut out = [0f32; 3];
            for i in 0..3 {
                let col = &c[i * 4..i * 4 + 4];
                for ch in 0..3 {
                    out[ch] += col[1 + ch] as f32 * inv * scale[i][ch] * k;
                }
            }
            out
        })
        .collect();
    li.flat = Some(v);
    li.dir = Some((d.chunks_exact(12).map(|c| <[u8; 12]>::try_from(c).unwrap()).collect(), scale));
    Ok(li)
}

/// Baked light of every StaticMeshComponent export, including level-instance components whose `StaticMesh` is inherited
/// from an archetype (not serialized on the instance, so `placements` skips them). `(component path, light)`.
pub fn component_lights(pkg: &Package) -> Vec<(String, LightInfo)> {
    let mut out = Vec::new();
    for ci in pkg.exports_of_class("StaticMeshComponent").collect::<Vec<_>>() {
        let Ok((_cp, mut r)) = object_props(pkg, ci) else { continue };
        let abs = pkg.exports[ci as usize - 1].serial_offset;
        if let Ok(li) = read_vertex_light(pkg, &mut r, abs) {
            out.push((pkg.obj_path(ci), li));
        }
    }
    out
}

pub fn placements(pkg: &Package) -> Vec<(Placement, Option<String>)> {
    let mut out = Vec::new();
    for ci in pkg.exports_of_class("StaticMeshComponent").collect::<Vec<_>>() {
        let res = (|| -> Result<Option<Placement>> {
            let (cp, mut r) = object_props(pkg, ci)?;
            let Some(mesh) = cp.get("StaticMesh").map(|p| p.as_obj()).filter(|&m| m != 0) else { return Ok(None) };
            if bool_or(&cp, "HiddenGame", false) {
                return Ok(None);
            }
            let owner = pkg.exports[ci as usize - 1].outer;
            let owner_class = pkg.class_name(owner).to_string();
            let mut m = srt(
                {
                    let s = f_or(&cp, "Scale", 1.0);
                    let s3 = vec_or(&cp, "Scale3D", [1., 1., 1.]);
                    [s * s3[0], s * s3[1], s * s3[2]]
                },
                rot_or(&cp, "Rotation"),
                vec_or(&cp, "Translation", [0., 0., 0.]),
            );
            if owner > 0 && owner_class != "Level" && owner_class != "Package" {
                let (ap, _) = object_props(pkg, owner)?;
                if bool_or(&ap, "bHidden", false) {
                    return Ok(None);
                }
                let smc = ap.get("StaticMeshComponent").map(|p| p.as_obj()).unwrap_or(0);
                let col = ap.get("CollisionComponent").map(|p| p.as_obj()).unwrap_or(0);
                if col == ci && smc != ci {
                    return Ok(None);
                }
                // Actor with a main StaticMeshComponent: only that one (others are usually collision helpers),
                // unless it is listed in Components.
                if smc != 0 && smc != ci {
                    let comps = ap.get("Components").map(|p| p.as_obj_array()).unwrap_or_default();
                    if !comps.contains(&ci) {
                        return Ok(None);
                    }
                }
                m = mul(&m, &actor_matrix(&ap));
            }
            let mats = cp
                .list
                .iter()
                .filter(|p| p.name == "Materials")
                .flat_map(|p| p.as_obj_array())
                .map(|o| if o != 0 { pkg.obj_path(o) } else { String::new() })
                .collect();
            let abs = pkg.exports[ci as usize - 1].serial_offset;
            let li = read_vertex_light(pkg, &mut r, abs).unwrap_or_else(|e| {
                if std::env::var_os("BOTS_LEVEL_LIGHT_LOG").is_some() {
                    eprintln!("level light {}: {e:#}", pkg.obj_path(ci));
                }
                LightInfo { ty: 99, flat: None, dir: None, lm2d: None }
            });
            Ok(Some(Placement {
                package: pkg.name.clone(),
                actor: pkg.obj_path(owner),
                actor_class: owner_class,
                component: pkg.obj_path(ci),
                mesh: pkg.obj_path(mesh),
                mesh_idx: mesh,
                materials: mats,
                transform: m,
                vertex_light: li.flat,
                lightmap_kind: li.ty,
                vertex_light_dir: li.dir,
                lightmap2d: li.lm2d,
            }))
        })();
        match res {
            Ok(Some(p)) => out.push((p, None)),
            Ok(None) => {}
            Err(e) => out.push((
                Placement {
                    package: pkg.name.clone(),
                    actor: String::new(),
                    actor_class: String::new(),
                    component: pkg.obj_path(ci),
                    mesh: String::new(),
                    mesh_idx: 0,
                    materials: vec![],
                    transform: identity(),
                    vertex_light: None,
                    lightmap_kind: 0,
                    vertex_light_dir: None,
                    lightmap2d: None,
                },
                Some(format!("{e:#}")),
            )),
        }
    }
    out
}
