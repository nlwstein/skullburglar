//! N10: lights, fog and post settings from a scene-feed frame, as plain Rust (no wgpu types).
//!
//! Everything is in the renderer's Y-up space: a UE3 vector (x, y, z) becomes (x, z, y). Colours are linear
//! `LightColor / 255 * Brightness`, the same convention N8 used for the sun (it matched the port).
//!
//! What UE3 does, and what this mirrors:
//! - **Baked lights** (`UseDirectLightMap`, or class `STATIC_AFFECTING`) are already in the lightmaps and vertex
//!   light, so static meshes must not light them again.
//! - **Dynamic objects** (characters, movers) are lit by their `DynamicLightEnvironmentComponent`'s three composite
//!   lights: one directional, one point and one spherical-harmonic light. Those come tagged with `LightEnvironment`.
//! - **Level lights that are neither** (muzzle flashes, spawned or scripted lights) are real dynamic lights and
//!   affect static meshes too.

use anyhow::{Context, Result};
use serde_json::Value;
use std::collections::BTreeMap;

pub mod bin;
pub mod pack;
pub mod post;
pub use post::{PostFeed, ToneMap};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Point,
    Spot,
    Directional,
    Sh,
    Sky,
}

impl Kind {
    /// B-230/B-245: the kinds whose raw `LightColor / 255` the feed carries in [`Light::raw`] (sky lights use that slot
    /// for their lower colour).
    pub fn carries_raw(self) -> bool {
        !matches!(self, Kind::Sky)
    }
}

#[derive(Clone, Debug)]
pub struct Light {
    pub kind: Kind,
    pub name: String,
    /// `LightEnvironment` component path when this is one of a dynamic light environment's composite lights.
    pub env: Option<String>,
    pub enabled: bool,
    /// Linear RGB times Brightness.
    pub color: [f32; 3],
    /// World position (point and spot), Y-up.
    pub pos: [f32; 3],
    /// Direction the light travels (directional) or points (spot axis), Y-up, unit length.
    pub travel: [f32; 3],
    pub radius: f32,
    pub falloff: f32,
    pub inner_deg: f32,
    pub outer_deg: f32,
    /// Built into lightmaps / vertex light: must not be applied again to static meshes.
    pub baked: bool,
    pub cast_dynamic_shadows: bool,
    pub affects_static: bool,
    pub affects_dynamic: bool,
    /// Sky light lower hemisphere colour times LowerBrightness.
    pub lower: [f32; 3],
    /// B-230: point and spot lights: raw `LightColor / 255` (no brightness), or zeros when the feed doesn't carry it
    /// (older hosts). B-245: directional and SH lights too (light environments' lights). UE3 turns an FColor into a linear colour through its sRGB table (`pow(c / 255, 2.2)`) before
    /// multiplying by Brightness; `color` is the plain `LightColor / 255 * Brightness` (see [`Light::ue3_color`]).
    pub raw: [f32; 3],
    /// B-249: sky lights: raw `LowerColor / 255` (zeros when the feed doesn't carry it); `raw` holds their raw
    /// `LightColor / 255` when it does. UE3 decodes both through sRGB before the brightness, like every light colour.
    pub raw_lower: [f32; 3],
    /// SH light: 9 coefficients per channel (R, G, B), `WorldSpaceIncidentLighting`, still in UE3 axes.
    pub sh: Option<[[f32; 9]; 3]>,
    /// B-189: `CastShadows` and `LightShadowMode == LightShadow_Modulate` (a light environment's shadow light).
    pub cast_shadows: bool,
    pub modulated: bool,
    /// `ModShadowColor` (linear, as stored), `ModShadowFadeoutExponent`, `ShadowFalloffExponent` and
    /// `MinShadowFalloffRadius` (the distance from the light where the modulated shadow starts to fade out).
    pub mod_shadow: [f32; 3],
    pub mod_fade_exp: f32,
    pub shadow_falloff_exp: f32,
    pub min_shadow_falloff_radius: f32,
}

/// UE3 axes (x, y, z up) to the renderer's Y-up.
pub fn swz(v: [f32; 3]) -> [f32; 3] {
    [v[0], v[2], v[1]]
}

fn f(v: &Value, k: &str, d: f32) -> f32 {
    v.get(k).and_then(Value::as_f64).map(|x| x as f32).unwrap_or(d)
}
fn b(v: &Value, k: &str) -> bool {
    v.get(k).and_then(Value::as_bool).unwrap_or(false)
}
fn col(v: &Value, k: &str) -> [f32; 3] {
    v.get(k)
        .and_then(Value::as_array)
        .map(|a| {
            let g = |i: usize| a.get(i).and_then(Value::as_f64).unwrap_or(0.0) as f32 / 255.0;
            [g(0), g(1), g(2)]
        })
        .unwrap_or([0.0; 3])
}
fn mat(v: &Value) -> Option<[f32; 16]> {
    let a = v.as_array()?;
    if a.len() != 16 {
        return None;
    }
    let mut m = [0f32; 16];
    for (i, x) in a.iter().enumerate() {
        m[i] = x.as_f64()? as f32;
    }
    Some(m)
}
fn norm(v: [f32; 3]) -> [f32; 3] {
    let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt().max(1e-9);
    [v[0] / l, v[1] / l, v[2] / l]
}

impl Light {
    pub fn parse(l: &Value) -> Option<Light> {
        let class = l.get("class")?.as_str()?;
        let kind = match class {
            "PointLightComponent" => Kind::Point,
            "SpotLightComponent" => Kind::Spot,
            "DirectionalLightComponent" => Kind::Directional,
            "SphericalHarmonicLightComponent" => Kind::Sh,
            "SkyLightComponent" => Kind::Sky,
            _ => return None,
        };
        let p = l.get("props")?;
        let m = mat(p.get("LightToWorld")?)?;
        let brightness = f(p, "Brightness", 1.0);
        let c = col(p, "LightColor");
        let class_aff = p.get("LightAffectsClassification").and_then(Value::as_str).unwrap_or("");
        let ch = p.get("LightingChannels");
        let chan = |k: &str| ch.map(|c| b(c, k)).unwrap_or(true);
        let baked = b(p, "UseDirectLightMap") || class_aff == "LAC_STATIC_AFFECTING";
        let lb = f(p, "LowerBrightness", 0.0);
        let sh = p.get("WorldSpaceIncidentLighting").and_then(|w| {
            let mut out = [[0f32; 9]; 3];
            for (ci, k) in ["R", "G", "B"].iter().enumerate() {
                let v = w.get(*k)?.get("V")?.as_array()?;
                for i in 0..9 {
                    out[ci][i] = v.get(i)?.as_f64()? as f32;
                }
            }
            Some(out)
        });
        Some(Light {
            kind,
            name: l.get("name").and_then(Value::as_str).unwrap_or("").to_string(),
            env: p.get("LightEnvironment").and_then(Value::as_str).map(str::to_string),
            enabled: b(p, "bEnabled"),
            color: [c[0] * brightness, c[1] * brightness, c[2] * brightness],
            // A sky light has no position or direction (the binary feed reuses those slots for its raw colours, B-249).
            pos: if kind == Kind::Sky { [0.0; 3] } else { swz([m[12], m[13], m[14]]) },
            travel: if kind == Kind::Sky { [0.0; 3] } else { norm(swz([m[8], m[9], m[10]])) },
            radius: f(p, "Radius", 0.0),
            falloff: f(p, "FalloffExponent", 2.0),
            inner_deg: f(p, "InnerConeAngle", 0.0),
            outer_deg: f(p, "OuterConeAngle", 44.0),
            baked,
            cast_dynamic_shadows: b(p, "CastDynamicShadows"),
            affects_static: chan("Static"),
            affects_dynamic: chan("Dynamic"),
            lower: {
                let lc = col(p, "LowerColor");
                [lc[0] * lb, lc[1] * lb, lc[2] * lb]
            },
            raw: if kind.carries_raw() || kind == Kind::Sky { c } else { [0.0; 3] },
            raw_lower: if kind == Kind::Sky { col(p, "LowerColor") } else { [0.0; 3] },
            sh,
            cast_shadows: b(p, "CastShadows"),
            modulated: p.get("LightShadowMode").and_then(Value::as_str) == Some("LightShadow_Modulate"),
            mod_shadow: p
                .get("ModShadowColor")
                .and_then(Value::as_array)
                .map(|a| {
                    let g = |i: usize| a.get(i).and_then(Value::as_f64).unwrap_or(1.0) as f32;
                    [g(0), g(1), g(2)]
                })
                .unwrap_or([1.0; 3]),
            mod_fade_exp: f(p, "ModShadowFadeoutExponent", 3.0),
            shadow_falloff_exp: f(p, "ShadowFalloffExponent", 2.0),
            min_shadow_falloff_radius: f(p, "MinShadowFalloffRadius", 0.0),
        })
    }

    /// Short owner label of a light environment component path (the actor before `.DynamicLightEnvironmentComponent_N`).
    pub fn env_owner(env: &str) -> &str {
        let head = env.split(".DynamicLightEnvironmentComponent").next().unwrap_or(env);
        head.rsplit('.').next().unwrap_or(head)
    }

    /// B-230: the colour UE3's light pass uses, `pow(LightColor / 255, 2.2) * Brightness` (FLinearColor(FColor) goes
    /// through the sRGB table), when the raw colour is known; else `color`. Decoded from the port's point-light pass:
    /// C1PointLightFX_7 at 81610 has LightColor (47, 34, 23), Brightness 18.53, and the pixel shader's
    /// LightColorAndFalloffExponent (c4) is (0.449, 0.220, 0.093, 5): `color` would be (3.415, 2.471, 1.671).
    /// `color * raw^1.2` is the same thing without needing Brightness on its own.
    pub fn ue3_color(&self) -> [f32; 3] {
        if self.raw == [0.0; 3] {
            return self.color;
        }
        let g = |k: usize| self.color[k] * self.raw[k].max(0.0).powf(1.2);
        [g(0), g(1), g(2)]
    }

    /// B-245: [`Light::ue3_color`] for a light environment's light. An older host sends no raw colour for directional
    /// and SH lights; UE3's DynamicLightEnvironment stores its directional as a colour normalised to a 255 maximum
    /// (every 71515 environment has R = 255) times Brightness, so `color / max(color)` recovers the raw colour.
    pub fn ue3_env_color(&self) -> [f32; 3] {
        if self.raw != [0.0; 3] {
            return self.ue3_color();
        }
        let m = self.color[0].max(self.color[1]).max(self.color[2]);
        if m <= 0.0 {
            return self.color;
        }
        let g = |k: usize| self.color[k] * (self.color[k].max(0.0) / m).powf(1.2);
        [g(0), g(1), g(2)]
    }

    /// B-249: a sky light's upper and lower colours as the port's sky pass gets them, `pow(c / 255, 2.2) * brightness`
    /// (pixel constants c4 and c5 at 21115 and 32225: (0.154, 0.215, 0.248) for LightColor (128, 149, 159), Brightness
    /// 0.7, where the plain colour is (0.351, 0.409, 0.436)). Plain colours when the raw ones are unknown.
    pub fn ue3_sky(&self) -> ([f32; 3], [f32; 3]) {
        let dec = |plain: [f32; 3], raw: [f32; 3]| {
            if raw == [0.0; 3] {
                plain
            } else {
                let g = |k: usize| plain[k] * raw[k].max(0.0).powf(1.2);
                [g(0), g(1), g(2)]
            }
        };
        (dec(self.color, self.raw), dec(self.lower, self.raw_lower))
    }

    pub fn luma(&self) -> f32 {
        self.color[0] * 0.3 + self.color[1] * 0.59 + self.color[2] * 0.11
    }
}

/// One dynamic light environment's composite lights.
#[derive(Clone, Debug, Default)]
pub struct LightEnv {
    /// Full `LightEnvironment` component path (the key the feed uses).
    pub path: String,
    pub owner: String,
    pub directional: Option<Light>,
    pub point: Option<Light>,
    pub sh: Option<Light>,
}

/// B-189: a light environment's modulated shadow, as the port draws it (decoded from capture f1938, draws
/// 785-809): UE3's DynamicLightEnvironment puts a zero-brightness point light `LightDistance` (10) bounds radii away
/// in the dominant light direction; it casts a per-object shadow (a perspective depth map of the owner) that
/// multiplies the scene colour by `lerp(m, 1, lit^ModShadowFadeoutExponent)`, where
/// `m = 1 - (1 - ModShadowColor) * fade` and `fade = (1 - max(d * scale + bias, 0)^2)^ShadowFalloffExponent` fades
/// it out between `MinShadowFalloffRadius` and `Radius` from the light.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EnvShadow {
    /// Light position, Y-up.
    pub pos: [f32; 3],
    pub radius: f32,
    pub color: [f32; 3],
    pub fade_exp: f32,
    pub falloff_exp: f32,
    /// d = distance / radius: `fade` uses `max(d * scale + bias, 0)`.
    pub scale: f32,
    pub bias: f32,
}

impl LightEnv {
    /// Where the environment samples: the point light is placed on the object; fall back to nothing.
    pub fn position(&self) -> Option<[f32; 3]> {
        self.point.as_ref().map(|l| l.pos)
    }

    /// The environment's modulated shadow, if its shadow light casts one.
    pub fn shadow(&self) -> Option<EnvShadow> {
        let p = self.point.as_ref()?;
        if !(p.enabled && p.cast_shadows && p.modulated && p.radius > 0.0) {
            return None;
        }
        // f1938: radius 1352, scale 6.5, bias -5.5 (MinShadowFalloffRadius = 0.846 radius in every feed frame).
        let f = p.min_shadow_falloff_radius.clamp(0.0, p.radius * 0.999);
        let scale = p.radius / (p.radius - f);
        Some(EnvShadow {
            pos: p.pos,
            radius: p.radius,
            color: p.mod_shadow,
            fade_exp: p.mod_fade_exp,
            falloff_exp: p.shadow_falloff_exp,
            scale,
            bias: -f / (p.radius - f),
        })
    }
}

#[derive(Clone, Debug)]
pub struct Fog {
    pub color: [f32; 3],
    /// W4: LightColor alone (0..1, gamma-encoded as authored); the port's fog pass uses LightColor^2.2 and ignores
    /// LightBrightness.
    pub light_color: [f32; 3],
    pub density: f32,
    /// World height (Y-up) the fog is anchored at.
    pub height: f32,
    pub start: f32,
    pub extinction: f32,
}

#[derive(Clone, Debug, Default)]
pub struct Frame {
    /// The level's shadow-casting directional light (the sun), if any.
    pub sun: Option<Light>,
    /// Other level directional lights that affect dynamic objects but aren't the sun (the fill).
    pub fills: Vec<Light>,
    /// Sky lights, summed into a hemisphere ambient.
    pub sky_up: [f32; 3],
    pub sky_down: [f32; 3],
    /// B-249: the same sums with each sky light decoded as UE3 does ([`Light::ue3_sky`]).
    pub sky_up_ue3: [f32; 3],
    pub sky_down_ue3: [f32; 3],
    /// Level lights that are really dynamic: they light static meshes too.
    pub dynamic: Vec<Light>,
    /// Count of baked level lights skipped (already in lightmaps).
    pub baked_skipped: usize,
    pub envs: Vec<LightEnv>,
    pub fog: Option<Fog>,
    pub post: PostFeed,
    pub camera_pos: Option<[f32; 3]>,
}

impl Frame {
    pub fn from_json(s: &str) -> Result<Frame> {
        let v: Value = serde_json::from_str(s).context("feed frame json")?;
        Frame::from_value(&v)
    }

    pub fn load(path: &std::path::Path) -> Result<Frame> {
        let s = std::fs::read_to_string(path).with_context(|| format!("read {}", path.display()))?;
        Frame::from_json(&s)
    }

    pub fn from_value(v: &Value) -> Result<Frame> {
        let lights: Vec<Light> = v
            .get("lights")
            .and_then(Value::as_array)
            .map(|a| a.as_slice())
            .unwrap_or(&[])
            .iter()
            .filter_map(Light::parse)
            .collect();
        // Fog: the first attached HeightFogComponent.
        let mut fog = None;
        if let Some(h) = v.get("fog").and_then(Value::as_array).and_then(|a| a.iter().find(|x| x.get("class").and_then(Value::as_str) == Some("HeightFogComponent"))) {
            let p = &h["props"];
            if b(p, "bEnabled") {
                let c = col(p, "LightColor");
                let k = f(p, "LightBrightness", 1.0);
                fog = Some(Fog {
                    color: [c[0] * k, c[1] * k, c[2] * k],
                    light_color: c,
                    density: f(p, "Density", 0.0),
                    height: f(p, "Height", 0.0),
                    start: f(p, "StartDistance", 0.0),
                    extinction: f(p, "ExtinctionDistance", 1e8),
                });
            }
        }
        let post = v.get("post").map(PostFeed::from_value).unwrap_or_default();
        let mut fr = Frame::assemble(lights, fog, post);
        if let Some(c) = v.get("camera").and_then(|c| c.get("location")).and_then(Value::as_array) {
            if c.len() == 3 {
                fr.camera_pos = Some(swz([c[0].as_f64().unwrap_or(0.0) as f32, c[1].as_f64().unwrap_or(0.0) as f32, c[2].as_f64().unwrap_or(0.0) as f32]));
            }
        }
        Ok(fr)
    }

    /// Sort parsed lights into the sun, fills, sky, dynamic lights and per-owner environments. Shared by the JSON and
    /// the binary feed.
    pub fn assemble(lights: Vec<Light>, fog: Option<Fog>, post: PostFeed) -> Frame {
        let mut fr = Frame::default();
        let mut envs: BTreeMap<String, LightEnv> = BTreeMap::new();
        let mut level: Vec<Light> = vec![];
        for l in lights {
            if let Some(e) = l.env.clone() {
                let ent = envs.entry(e.clone()).or_insert_with(|| LightEnv { owner: Light::env_owner(&e).to_string(), path: e, ..Default::default() });
                match l.kind {
                    Kind::Directional => ent.directional = Some(l),
                    Kind::Point | Kind::Spot => ent.point = Some(l),
                    Kind::Sh => ent.sh = Some(l),
                    Kind::Sky => {}
                }
            } else {
                level.push(l);
            }
        }
        fr.envs = envs.into_values().collect();
        // The sun is the brightest enabled shadow-casting level directional light.
        let sun_idx = level
            .iter()
            .enumerate()
            .filter(|(_, l)| l.kind == Kind::Directional && l.enabled && l.cast_dynamic_shadows)
            .max_by(|a, b| a.1.luma().total_cmp(&b.1.luma()))
            .map(|(i, _)| i);
        for (i, l) in level.into_iter().enumerate() {
            match l.kind {
                Kind::Directional if Some(i) == sun_idx => fr.sun = Some(l),
                Kind::Directional => {
                    if l.enabled {
                        fr.fills.push(l);
                    }
                }
                Kind::Sky => {
                    if l.enabled {
                        for k in 0..3 {
                            fr.sky_up[k] += l.color[k];
                            fr.sky_down[k] += l.lower[k];
                        }
                        let (up, dn) = l.ue3_sky();
                        for k in 0..3 {
                            fr.sky_up_ue3[k] += up[k];
                            fr.sky_down_ue3[k] += dn[k];
                        }
                    }
                }
                Kind::Sh => {}
                Kind::Point | Kind::Spot => {
                    if l.baked {
                        fr.baked_skipped += 1;
                    } else if l.enabled && l.luma() > 1e-4 && l.radius > 0.0 {
                        fr.dynamic.push(l);
                    }
                }
            }
        }
        fr.fog = fog;
        fr.post = post;
        fr
    }

    /// Index of the light environment whose point light is nearest `p` (Y-up), within `max_dist`.
    pub fn nearest_env(&self, p: [f32; 3], max_dist: f32) -> Option<usize> {
        let mut best: Option<(usize, f32)> = None;
        for (i, e) in self.envs.iter().enumerate() {
            let Some(q) = e.position() else { continue };
            let d = ((q[0] - p[0]).powi(2) + (q[1] - p[1]).powi(2) + (q[2] - p[2]).powi(2)).sqrt();
            if d <= max_dist && best.is_none_or(|(_, bd)| d < bd) {
                best = Some((i, d));
            }
        }
        best.map(|x| x.0)
    }
}

/// UE3 point-light distance attenuation: `pow(saturate(1 - (d/radius)^2), FalloffExponent)`. A zero exponent means
/// UE3's inverse-square-style curve; here a smooth `(1 - x)^2` window so it still reaches zero at the radius.
pub fn point_attenuation(dist: f32, radius: f32, falloff: f32) -> f32 {
    let x = (dist / radius.max(1e-3)).powi(2);
    let w = (1.0 - x).clamp(0.0, 1.0);
    if falloff <= 0.0 { w * w } else { w.powf(falloff) }
}

/// Spot cone attenuation: `saturate((cos - cos_outer) / (cos_inner - cos_outer))` squared.
pub fn spot_attenuation(cos_angle: f32, inner_deg: f32, outer_deg: f32) -> f32 {
    let co = outer_deg.to_radians().cos();
    let ci = inner_deg.to_radians().cos().max(co + 1e-4);
    let t = ((cos_angle - co) / (ci - co)).clamp(0.0, 1.0);
    t * t
}

/// UE3's `FSHVector::SHBasisFunction` for a unit vector in UE3 axes.
pub fn sh_basis(v: [f32; 3]) -> [f32; 9] {
    let (x, y, z) = (v[0], v[1], v[2]);
    [
        0.282095,
        -0.488603 * y,
        0.488603 * z,
        -0.488603 * x,
        1.092548 * x * y,
        -1.092548 * y * z,
        0.315392 * (3.0 * z * z - 1.0),
        -1.092548 * x * z,
        0.546274 * (x * x - y * y),
    ]
}

/// Diffuse irradiance of an SH light for a surface normal given in UE3 axes: the incident-radiance SH weighted by the
/// cosine lobe (pi, 2pi/3, pi/4 per band), not divided by pi (UE3 does not), clamped at zero.
pub fn sh_diffuse(sh: &[[f32; 9]; 3], n_ue: [f32; 3]) -> [f32; 3] {
    let b = sh_basis(n_ue);
    // UE3 Lambert diffuse transfer (pi, 2pi/3, pi/4), as the port's SH light pass uses (W4).
    let p = std::f32::consts::PI;
    let w = [p, 2.0 * p / 3.0, 2.0 * p / 3.0, 2.0 * p / 3.0, p / 4.0, p / 4.0, p / 4.0, p / 4.0, p / 4.0];
    let mut o = [0.0; 3];
    for c in 0..3 {
        let mut s = 0.0;
        for i in 0..9 {
            s += sh[c][i] * b[i] * w[i];
        }
        o[c] = s.max(0.0);
    }
    o
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p3(n: &str) -> Option<Frame> {
        let p = std::env::var("FEED_DIR").unwrap_or_else(|_| format!("{}/repos/bots-recomp/run/scenefeed/p3", std::env::var("HOME").unwrap()));
        Frame::load(&std::path::Path::new(&p).join(n)).ok()
    }

    #[test]
    fn attenuation_curve() {
        assert!((point_attenuation(0.0, 1000.0, 2.0) - 1.0).abs() < 1e-6);
        assert_eq!(point_attenuation(1000.0, 1000.0, 2.0), 0.0);
        assert!((point_attenuation(500.0, 1000.0, 2.0) - 0.5625).abs() < 1e-5);
        assert!(spot_attenuation(1.0, 20.0, 44.0) > 0.99);
        assert_eq!(spot_attenuation(0.0, 20.0, 44.0), 0.0);
    }

    #[test]
    fn ue3_light_colour_is_srgb_decoded() {
        // B-230: C1PointLightFX_7 at 81610, LightColor (47, 34, 23), Brightness 18.5302; the port's light pass had
        // LightColorAndFalloffExponent (0.44886, 0.22016, 0.09317, 5).
        let json = r#"{"lights":[{"class":"PointLightComponent","props":{"LightToWorld":[1,0,0,0, 0,1,0,0, 0,0,1,0, 10135.2,345.5,360.7,1],"Brightness":18.5302,"LightColor":[47,34,23,0],"bEnabled":true,"Radius":1246.3,"FalloffExponent":5,"LightingChannels":{"Static":true,"Dynamic":true}}}]}"#;
        let fr = Frame::from_json(json).unwrap();
        let l = &fr.dynamic[0];
        let c = l.ue3_color();
        for (a, b) in c.iter().zip([0.44886, 0.22016, 0.09317]) {
            assert!((a - b).abs() < 0.002, "{c:?}");
        }
        assert!((l.color[0] - 3.4155).abs() < 1e-3);
        let packed = pack::DynFrame::from_frame(&fr, pack::MAX_LIGHTS, false).lights[0];
        assert!((packed[4] - 0.44886).abs() < 0.002 && packed[7] == 5.0);
        let plain = pack::DynFrame::from_frame_with(&fr, pack::MAX_LIGHTS, false, false).lights[0];
        assert!((plain[4] - 3.4155).abs() < 1e-3);
    }

    #[test]
    fn sun_and_sky_colours_are_srgb_decoded() {
        // B-249: the 21115 level (feed frame 2 of run draws1). The port's pixel constants: the sun pass c4 = (4.787, 1.945,
        // 0.680) for LightColor (250, 166, 103), Brightness 5; the sky pass c4 = (0.154, 0.215, 0.248) for LightColor
        // (128, 149, 159), Brightness 0.7, and c5 = (0.030, 0.024, 0.016) for the lower colour (LowerBrightness 0.7).
        let json = r#"{"lights":[{"class":"DirectionalLightComponent","props":{"LightToWorld":[1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],"Brightness":5,"LightColor":[250,166,103,0],"bEnabled":true,"CastDynamicShadows":true,"LightingChannels":{"Static":true,"Dynamic":true}}},
            {"class":"SkyLightComponent","props":{"LightToWorld":[1,0,0,0, 0,1,0,0, 0,0,1,0, 5,6,7,1],"Brightness":0.7,"LightColor":[128,149,159,0],"LowerBrightness":0.7,"LowerColor":[60,56,48,0],"bEnabled":true}}]}"#;
        let fr = Frame::from_json(json).unwrap();
        let near = |c: &[f32], want: [f32; 3]| c.iter().zip(want).all(|(a, b)| (a - b).abs() < 0.003);
        let sun = fr.sun.as_ref().unwrap();
        assert!(near(&sun.ue3_color(), [4.787, 1.945, 0.680]), "{:?}", sun.ue3_color());
        assert!(near(&sun.color, [4.902, 3.255, 2.020]));
        assert!(near(&fr.sky_up_ue3, [0.154, 0.215, 0.248]), "{:?}", fr.sky_up_ue3);
        assert!(near(&fr.sky_up, [0.351, 0.409, 0.436]));
        let low = (|c: f32| (c / 255.0).powf(2.2) * 0.7);
        assert!(near(&fr.sky_down_ue3, [low(60.0), low(56.0), low(48.0)]), "{:?}", fr.sky_down_ue3);
        // A host without the raw colours (zeros) keeps the plain sums.
        let mut l = Light::parse(&serde_json::from_str::<Value>(r#"{"class":"SkyLightComponent","props":{"LightToWorld":[1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],"Brightness":0.7,"LightColor":[128,149,159,0],"bEnabled":true}}"#).unwrap()).unwrap();
        l.raw = [0.0; 3];
        assert_eq!(l.ue3_sky().0, l.color);
    }

    #[test]
    fn env_light_colour_is_srgb_decoded() {
        // B-245: 50 Cent's light environment at 71515 (feed frame 1 of run d1), directional LightColor (255, 202, 143),
        // Brightness 5.362; the port's directional pass (PS 8a36feff) had c5 = (5.362, 3.2116, 1.502).
        let json = r#"{"lights":[{"class":"DirectionalLightComponent","props":{"LightToWorld":[1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],"Brightness":5.362,"LightColor":[255,202,143,66],"bEnabled":true,"LightEnvironment":"L.Ch_Type1_50Cent_2.DynamicLightEnvironmentComponent_106","LightingChannels":{"Static":true,"Dynamic":true}}},
            {"class":"SphericalHarmonicLightComponent","props":{"LightToWorld":[0,0,1,0, 0,1,0,0, 1,0,0,0, 0,0,0,1],"Brightness":1,"LightColor":[255,255,255,0],"bEnabled":true,"LightEnvironment":"L.Ch_Type1_50Cent_2.DynamicLightEnvironmentComponent_106","WorldSpaceIncidentLighting":{"R":{"V":[0.6511,0,0,0,0,0,0,0,0]},"G":{"V":[0.5459,0,0,0,0,0,0,0,0]},"B":{"V":[0.3833,0,0,0,0,0,0,0,0]}}}}]}"#;
        let fr = Frame::from_json(json).unwrap();
        let e = &fr.envs[0];
        let port = [5.362, 3.2116, 1.502];
        let near = |c: &[f32], want: [f32; 3]| c.iter().zip(want).all(|(a, b)| (a - b).abs() < 0.003);
        let on = pack::pack_env_with(e, true);
        assert!(near(&on[4..7], port), "{:?}", &on[4..7]);
        let off = pack::pack_env(e);
        assert!(near(&off[4..7], [5.362, 4.2476, 3.0069]), "{:?}", &off[4..7]);
        // The SH light is white: its coefficients pass through either way (the port's c7 is the DC term as fed).
        assert!(near(&on[16..19], [0.6511, 0.5459, 0.3833]));
        // An older host sends no raw colour: the 255-normalised colour recovers it.
        let mut d = e.directional.clone().unwrap();
        d.raw = [0.0; 3];
        assert!(near(&d.ue3_env_color(), port), "{:?}", d.ue3_env_color());
        let on_fr = pack::DynFrame::from_frame_env(&fr, 0, true, true, true);
        assert!(near(&on_fr.envs[0][4..7], port));
    }

    #[test]
    fn sh_dc_only_is_flat() {
        let mut sh = [[0.0; 9]; 3];
        sh[0][0] = 1.0;
        let a = sh_diffuse(&sh, [0.0, 0.0, 1.0]);
        let b = sh_diffuse(&sh, [1.0, 0.0, 0.0]);
        assert!((a[0] - b[0]).abs() < 1e-6 && (a[0] - 0.886227).abs() < 1e-5);
    }

    #[test]
    fn env_shadow_matches_port() {
        // A light environment's shadow light as the feed carries it; f1938's projection pass (draw 792) used
        // radius 1352 and fade scale/bias 6.5/-5.5, i.e. MinShadowFalloffRadius = 11/13 of the radius.
        let v: Value = serde_json::from_str(r#"{"class": "PointLightComponent", "name": "p", "props": {
            "LightToWorld": [1,0,0,0, 0,1,0,0, 0,0,1,0, 3436.02,193.442,1136.1,1], "Radius": 1352.0,
            "Brightness": 0, "LightColor": [255,255,255,0], "bEnabled": true, "CastShadows": true,
            "LightShadowMode": "LightShadow_Modulate", "ModShadowColor": [0.6437, 0.6242, 0.6226, 1],
            "ModShadowFadeoutExponent": 3, "ShadowFalloffExponent": 2, "MinShadowFalloffRadius": 1144.0,
            "LightEnvironment": "X.DynamicLightEnvironmentComponent_1"}}"#).unwrap();
        let l = Light::parse(&v).unwrap();
        let e = LightEnv { point: Some(l), ..Default::default() };
        let s = e.shadow().unwrap();
        assert!((s.scale - 6.5).abs() < 0.01 && (s.bias + 5.5).abs() < 0.01, "{s:?}");
        assert_eq!(s.pos, [3436.02, 1136.1, 193.442]);
        assert_eq!((s.fade_exp, s.falloff_exp), (3.0, 2.0));
        let mut e2 = e.clone();
        e2.point.as_mut().unwrap().modulated = false;
        assert!(e2.shadow().is_none());
    }

    #[test]
    fn recorded_frame() {
        let Some(f) = p3("frame_000006.json") else { return };
        let sun = f.sun.as_ref().expect("sun");
        // Mission 6 sun: brightness 1.5, travels (0.15, -0.85, -0.51) in UE axes -> Y-up (0.15, -0.51, -0.85)
        assert!((sun.color[0] - 252.0 / 255.0 * 1.5).abs() < 1e-3);
        assert!((sun.travel[0] - 0.1512).abs() < 2e-3 && (sun.travel[1] + 0.5078).abs() < 2e-3);
        assert_eq!(f.envs.len(), 35);
        assert!(f.envs.iter().all(|e| e.directional.is_some() && e.point.is_some() && e.sh.is_some()));
        // only the scripted point light and the disabled movable light are dynamic; the 70-odd level lights are baked
        assert_eq!(f.dynamic.len(), 1, "{:?}", f.dynamic.iter().map(|l| &l.name).collect::<Vec<_>>());
        assert!(f.baked_skipped > 50);
        let fog = f.fog.as_ref().unwrap();
        assert_eq!((fog.height, fog.start), (6443.0, 4000.0));
        assert!(f.post.bloom_enabled && (f.post.bloom_scale - 0.5).abs() < 1e-6 && (f.post.bloom_threshold - 1.5).abs() < 1e-6);
        assert!(f.post.grade_is_identity());
        assert_eq!(f.post.material_effect.as_deref(), Some("GEN_PostProcess.FlyingLevel_MAT01"));
        assert!(f.post.tone.enabled && f.post.tone.max_luminosity == 20.0);
    }
}

#[cfg(test)]
mod summary {
    /// `LIGHTFEED_SUMMARY=<feed json> cargo test -p lightfeed summary -- --ignored --nocapture`
    #[test]
    #[ignore]
    fn summary() {
        let p = std::env::var("LIGHTFEED_SUMMARY").unwrap();
        for p in p.split(',') {
            let v: serde_json::Value = serde_json::from_slice(&std::fs::read(p).unwrap()).unwrap();
            let f = super::Frame::from_value(&v).unwrap();
            let mut g = f.clone(); g.envs.clear(); println!("FULL {:#?}", g);
            println!("{p}\n sun {:?}\n fills {} sky_up {:?} sky_down {:?} dynamic {} baked {} envs {}",
                f.sun.as_ref().map(|s| (s.color, s.travel, s.enabled)), f.fills.len(), f.sky_up, f.sky_down, f.dynamic.len(), f.baked_skipped, f.envs.len());
            for e in f.envs.iter().filter(|e| e.path.contains("50Cent")) {
                println!(" packed {:?}", crate::pack::pack_env(e));
                println!(" 50cent dir {:?} sh0 {:?}", e.directional.as_ref().map(|d| (d.color, d.travel)), e.sh.as_ref().map(|s| [s.sh.unwrap()[0][0], s.sh.unwrap()[1][0]]));
            }
        }
    }
}
