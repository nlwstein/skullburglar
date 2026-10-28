//! GPU-ready packing of the feed's lights (plain Rust, `bytemuck`-free: flat `f32` / `u32` arrays).
//!
//! Layout (mirrors `matdef/src/scene.wgsl`):
//! - dynamic light, 4 x vec4: `[pos.xyz, 1/radius] [rgb, falloff] [axis.xyz, cos_outer] [kind(0 point, 1 spot), 1/(cos_inner - cos_outer), 0, 0]`
//! - light environment, 13 x vec4: `[to_light.xyz, on] [dir rgb, 0] [pos.xyz, 1/radius] [pt rgb, falloff] [sh coefficient i rgb, 0] x9`
//! - per-instance word pair: `[four 8-bit light indices (0xFF = none), environment index + 1 (0 = none)]`

use crate::{EnvShadow, Frame, Kind, Light, LightEnv, swz};

pub const MAX_LIGHTS: usize = 64;
pub const MAX_ENVS: usize = 256;
pub const LIGHT_F32: usize = 16;
pub const ENV_F32: usize = 52;
pub const LISTED: usize = 4;

/// `ue3_gamma` (B-230): use the colour UE3's light pass uses ([`Light::ue3_color`]) instead of the plain
/// `LightColor / 255 * Brightness`.
pub fn pack_light(l: &Light, ue3_gamma: bool) -> [f32; LIGHT_F32] {
    let spot = l.kind == Kind::Spot;
    let color = if ue3_gamma { l.ue3_color() } else { l.color };
    let co = l.outer_deg.to_radians().cos();
    let ci = l.inner_deg.to_radians().cos().max(co + 1e-4);
    [
        l.pos[0], l.pos[1], l.pos[2], 1.0 / l.radius.max(1e-3),
        color[0], color[1], color[2], l.falloff,
        l.travel[0], l.travel[1], l.travel[2], co,
        spot as u32 as f32, 1.0 / (ci - co), 0.0, 0.0,
    ]
}

pub fn pack_env(e: &LightEnv) -> [f32; ENV_F32] {
    pack_env_with(e, false)
}

/// `ue3_gamma` (B-245): the environment's directional, point and SH light colours as UE3 uses them,
/// `pow(LightColor / 255, 2.2) * Brightness` ([`Light::ue3_env_color`]). The port's directional pass (PS 8a36feff on
/// 50 Cent at 71515) has LightColor (255, 202, 143), Brightness 5.362 in c5 as (5.362, 3.212, 1.502), not the plain
/// (5.362, 4.248, 3.007): the plain colour is twice as blue, which turned the characters' skin blue-grey.
pub fn pack_env_with(e: &LightEnv, ue3_gamma: bool) -> [f32; ENV_F32] {
    let col = |l: &Light| if ue3_gamma { l.ue3_env_color() } else { l.color };
    let mut o = [0f32; ENV_F32];
    if let Some(d) = e.directional.as_ref().filter(|d| d.enabled) {
        // to-light is the opposite of the travel direction
        o[0..3].copy_from_slice(&[-d.travel[0], -d.travel[1], -d.travel[2]]);
        o[3] = 1.0;
        o[4..7].copy_from_slice(&col(d));
    }
    if let Some(p) = e.point.as_ref().filter(|p| p.enabled && p.radius > 0.0) {
        o[8..11].copy_from_slice(&p.pos);
        o[11] = 1.0 / p.radius;
        o[12..15].copy_from_slice(&col(p));
        o[15] = p.falloff;
    }
    if let Some((l, c)) = e.sh.as_ref().filter(|s| s.enabled).and_then(|s| s.sh.as_ref().map(|c| (s, c))) {
        // An SH light's colour times brightness scales its coefficients.
        let lc = col(l);
        for i in 0..9 {
            for ch in 0..3 {
                o[16 + i * 4 + ch] = c[ch][i] * lc[ch];
            }
        }
    }
    o
}

/// Everything the renderer uploads per frame.
#[derive(Clone, Debug, Default)]
pub struct DynFrame {
    pub lights: Vec<[f32; LIGHT_F32]>,
    /// Source light index in `Frame::dynamic` for each packed light (debug and tests).
    pub envs: Vec<[f32; ENV_F32]>,
    /// Environment owners, same order as `envs`.
    pub env_owners: Vec<String>,
    /// Environment point-light positions, same order (for nearest-environment joins).
    pub env_pos: Vec<Option<[f32; 3]>>,
    /// B-189: each environment's modulated shadow, same order.
    pub env_shadows: Vec<Option<EnvShadow>>,
}

impl DynFrame {
    /// `max_lights` of 0 turns level dynamic lights off (the low-end setting).
    pub fn from_frame(fr: &Frame, max_lights: usize, with_envs: bool) -> DynFrame {
        DynFrame::from_frame_with(fr, max_lights, with_envs, true)
    }

    /// `ue3_gamma` (B-230, on by default): level dynamic lights get UE3's sRGB-decoded colour ([`Light::ue3_color`]).
    pub fn from_frame_with(fr: &Frame, max_lights: usize, with_envs: bool, ue3_gamma: bool) -> DynFrame {
        DynFrame::from_frame_env(fr, max_lights, with_envs, ue3_gamma, false)
    }

    /// As [`DynFrame::from_frame_with`]; `env_gamma` (B-245) packs the light environments with UE3's colours too
    /// ([`pack_env_with`]).
    pub fn from_frame_env(fr: &Frame, max_lights: usize, with_envs: bool, ue3_gamma: bool, env_gamma: bool) -> DynFrame {
        let mut d = DynFrame::default();
        for l in fr.dynamic.iter().filter(|l| l.affects_static).take(max_lights.min(MAX_LIGHTS)) {
            d.lights.push(pack_light(l, ue3_gamma));
        }
        if with_envs {
            for e in fr.envs.iter().take(MAX_ENVS) {
                d.envs.push(pack_env_with(e, env_gamma));
                d.env_owners.push(e.owner.clone());
                d.env_pos.push(e.position());
                d.env_shadows.push(e.shadow());
            }
        }
        d
    }

    pub fn env_for_owner(&self, owner: &str) -> Option<usize> {
        self.env_owners.iter().position(|o| o == owner)
    }

    pub fn nearest_env(&self, p: [f32; 3], max_dist: f32) -> Option<usize> {
        let mut best: Option<(usize, f32)> = None;
        for (i, q) in self.env_pos.iter().enumerate() {
            let Some(q) = q else { continue };
            let d = ((q[0] - p[0]).powi(2) + (q[1] - p[1]).powi(2) + (q[2] - p[2]).powi(2)).sqrt();
            if d <= max_dist && best.is_none_or(|(_, bd)| d < bd) {
                best = Some((i, d));
            }
        }
        best.map(|x| x.0)
    }

    /// Per-instance `[light indices, env + 1]` words for instances given as (centre, radius) spheres (Y-up) and an
    /// optional environment each. Keeps the `LISTED` strongest overlapping lights per instance.
    pub fn instance_words(&self, spheres: &[([f32; 3], f32)], env_of: &dyn Fn(usize) -> Option<usize>, max_listed: usize) -> Vec<[u32; 2]> {
        let k = max_listed.min(LISTED);
        spheres
            .iter()
            .enumerate()
            .map(|(i, (c, r))| {
                let mut cand: Vec<(f32, usize)> = vec![];
                if k > 0 {
                    for (li, l) in self.lights.iter().enumerate() {
                        let rad = 1.0 / l[3];
                        let d = ((l[0] - c[0]).powi(2) + (l[1] - c[1]).powi(2) + (l[2] - c[2]).powi(2)).sqrt();
                        if d < rad + r {
                            let strength = (l[4] * 0.3 + l[5] * 0.59 + l[6] * 0.11) * crate::point_attenuation((d - r).max(0.0), rad, l[7].max(1.0));
                            cand.push((strength, li));
                        }
                    }
                    cand.sort_by(|a, b| b.0.total_cmp(&a.0));
                }
                let mut w = 0xFFFF_FFFFu32;
                for (s, (_, li)) in cand.iter().take(k).enumerate() {
                    w = (w & !(0xFF << (8 * s))) | ((*li as u32) << (8 * s));
                }
                [w, env_of(i).map(|e| e as u32 + 1).unwrap_or(0)]
            })
            .collect()
    }
}

/// Convenience: Y-up point from a UE3 position.
pub fn ue_to_yup(v: [f32; 3]) -> [f32; 3] {
    swz(v)
}
