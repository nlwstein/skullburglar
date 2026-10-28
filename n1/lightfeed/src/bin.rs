//! B-188: the binary lighting feed. The game thread writes these fixed-layout records straight from guest memory (no
//! JSON text, no parse); [`decode`] turns them into the same [`Frame`] the JSON path builds. The C++ side
//! (`hybrid_state.h`) and `n1ffi.h` declare the same layouts; sizes are asserted on every side.
//!
//! Values are what the JSON feed carries after the same per-field defaults, except two things the renderer would
//! compute from them anyway: light colours arrive as `LightColor / 255 * Brightness` (the product `Light::parse`
//! forms) and the post chain arrives as raw numbers that [`decode`] passes through `sane` and the same gating the JSON
//! path applies. The post chain's effect class names (`PostFeed::chain`) are not carried: nothing live reads them.

use crate::post::{GodRays, PostFeed, ToneMap, Uber, sane};
use crate::{Fog, Frame, Kind, Light};
use std::ffi::{CStr, c_char};

/// 2 (B-205): `FeedLight` carries the shadow light's fields (104 bytes, was 80).
/// 3 (B-266): `FeedBin::params`, the post materials' live scalar parameters. A version 2 feed still decodes (no params).
pub const VERSION: u32 = 3;
/// The oldest version `decode` reads.
pub const MIN_VERSION: u32 = 2;
pub const NONE: u32 = u32::MAX;

/// `FeedBin::flags`: this feed carries fog and post (they are sent only when they change); absent means unchanged.
pub const FLAG_FOG_POST: u32 = 1;

// `FeedLight::kind`
pub const KIND_POINT: u32 = 0;
pub const KIND_SPOT: u32 = 1;
pub const KIND_DIRECTIONAL: u32 = 2;
pub const KIND_SH: u32 = 3;
pub const KIND_SKY: u32 = 4;

// `FeedLight::flags`
pub const LF_ENABLED: u32 = 1;
pub const LF_BAKED: u32 = 2;
pub const LF_CAST_DYN_SHADOWS: u32 = 4;
pub const LF_CHAN_STATIC: u32 = 8;
pub const LF_CHAN_DYNAMIC: u32 = 16;
/// B-205: `CastShadows`.
pub const LF_CAST_SHADOWS: u32 = 32;
/// B-205: `LightShadowMode == LightShadow_Modulate`.
pub const LF_MODULATED: u32 = 64;
/// B-249: a sky light carries its raw `LightColor / 255` in `pos` and raw `LowerColor / 255` in `axis` (sky lights
/// have no use for either; the position is irrelevant to a sky light).
pub const LF_SKY_RAW: u32 = 128;

// `FeedPost::flags`
pub const PF_UBER: u32 = 1;
pub const PF_GOD_RAYS: u32 = 2;
pub const PF_LS_BLOOM: u32 = 4;
pub const PF_LS_BALANCE: u32 = 8;
pub const PF_LS_SCENE: u32 = 16;
pub const PF_LS_TONE: u32 = 32;
pub const PF_LAST_SETTINGS: u32 = 64;
/// The uber effect's UseDifferentKernelForBloom.
pub const PF_UBER_DIFF_KERNEL: u32 = 128;
/// Bits 24..27 of `FeedPost::flags`: the number of MaterialEffect paths at `material_effect` (B-227).
pub const PF_MATERIALS_SHIFT: u32 = 24;

/// One light component. 104 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FeedLight {
    pub kind: u32,
    pub flags: u32,
    /// Index into the feed's path table (the owner's DynamicLightEnvironmentComponent), or `NONE`.
    pub env: u32,
    pub radius: f32,
    pub falloff: f32,
    pub inner_deg: f32,
    pub outer_deg: f32,
    /// LightToWorld translation, UE3 axes.
    pub pos: [f32; 3],
    /// LightToWorld Z row (the direction the light travels / the spot axis), UE3 axes, not normalised.
    pub axis: [f32; 3],
    /// LightColor / 255 * Brightness.
    pub color: [f32; 3],
    /// LowerColor / 255 * LowerBrightness (sky lights). B-230: point and spot lights carry their raw LightColor / 255
    /// here instead (they have no lower colour; zeros from older hosts). B-245: directional and SH lights too.
    pub lower: [f32; 3],
    /// Index of this light's 27 SH floats (R 9, G 9, B 9) in the feed's SH array, or -1.
    pub sh: i32,
    /// B-205: `ModShadowColor` R, G, B (linear, as stored; 1 when absent).
    pub mod_shadow: [f32; 3],
    /// `ModShadowFadeoutExponent` (3), `ShadowFalloffExponent` (2), `MinShadowFalloffRadius` (0).
    pub mod_fade_exp: f32,
    pub shadow_falloff_exp: f32,
    pub min_shadow_falloff_radius: f32,
}

/// The first HeightFogComponent. 44 bytes. `present` is 0 when there is none or it is disabled.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FeedFog {
    pub present: u32,
    /// LightColor / 255 * LightBrightness.
    pub color: [f32; 3],
    /// Raw LightColor / 255 (no brightness); n1 applies its own gamma to it (W4).
    pub light_color: [f32; 3],
    pub density: f32,
    pub height: f32,
    pub start: f32,
    pub extinction: f32,
}

/// Raw post-process numbers. NaN means "the property was not there". 180 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FeedPost {
    /// 0: the player's post chain could not be read (the JSON path would have sent an empty `post` object).
    pub present: u32,
    pub flags: u32,
    /// Path-table index of the chain's first shown MaterialEffect material, or `NONE`. The others follow it in the
    /// path table; `flags` bits 24..27 hold how many there are (0 from older hosts: one).
    pub material_effect: u32,
    /// SfUberPostProcessEffect BloomScale / BloomThreshold.
    pub uber_bloom_scale: f32,
    pub uber_bloom_threshold: f32,
    /// LastSettings Bloom_Scale / Bloom_Threshold.
    pub ls_bloom_scale: f32,
    pub ls_bloom_threshold: f32,
    /// ColourBalance_Shadows, _Midtones, _Highlights as bytes 0..255.
    pub balance: [f32; 9],
    pub desaturation: f32,
    pub shadows: [f32; 3],
    pub midtones: [f32; 3],
    pub highlights: [f32; 3],
    pub tone_min: f32,
    pub tone_max: f32,
    pub tone_scale: f32,
    /// SfGodRaysEffect: exposure, decay, magnitude, ModulateRayColour bytes (3), light point (2), pad.
    pub god: [f32; 9],
    /// W4, SfUberPostProcessEffect: BloomKernelSize, BlurKernelSize, FalloffExponent, MaxNearBlurAmount,
    /// MaxFarBlurAmount, FocusInnerRadius, FocusDistance.
    pub uber_dof: [f32; 7],
}

impl Default for FeedPost {
    fn default() -> Self {
        FeedPost {
            present: 0,
            flags: 0,
            material_effect: NONE,
            uber_bloom_scale: f32::NAN,
            uber_bloom_threshold: f32::NAN,
            ls_bloom_scale: f32::NAN,
            ls_bloom_threshold: f32::NAN,
            balance: [255.0; 9],
            desaturation: f32::NAN,
            shadows: [f32::NAN; 3],
            midtones: [f32::NAN; 3],
            highlights: [f32::NAN; 3],
            tone_min: f32::NAN,
            tone_max: f32::NAN,
            tone_scale: f32::NAN,
            god: [f32::NAN; 9],
            uber_dof: [f32::NAN; 7],
        }
    }
}

/// B-266: one scalar parameter of a post material, live from the game. 12 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FeedParam {
    /// Index among the post's MaterialEffect paths (0 = the one at `FeedPost::material_effect`).
    pub material: u32,
    /// Path-table index of the parameter's name.
    pub name: u32,
    pub value: f32,
}

/// What `n1_set_feed_bin` takes. Pointers are valid for the call only.
#[repr(C)]
pub struct FeedBin {
    pub version: u32,
    pub flags: u32,
    pub light_count: u32,
    pub path_count: u32,
    pub sh_count: u32,
    /// Version 3: the number of `params` (sent with `FLAG_FOG_POST`). Version 2 called it `reserved` (0).
    pub param_count: u32,
    pub lights: *const FeedLight,
    pub paths: *const *const c_char,
    pub sh: *const f32,
    pub fog: FeedFog,
    pub post: FeedPost,
    /// Version 3 only: a version 2 host's struct ends before this field, so `decode` reads it only for version 3.
    pub params: *const FeedParam,
}

const _: () = {
    assert!(size_of::<FeedLight>() == 104);
    assert!(size_of::<FeedFog>() == 44);
    assert!(size_of::<FeedPost>() == 180);
    assert!(size_of::<FeedParam>() == 12);
};

fn arr<'a, T>(p: *const T, n: u32) -> &'a [T] {
    if n == 0 || p.is_null() { &[] } else { unsafe { std::slice::from_raw_parts(p, n as usize) } }
}

fn swz(v: [f32; 3]) -> [f32; 3] {
    [v[0], v[2], v[1]]
}

fn norm(v: [f32; 3]) -> [f32; 3] {
    let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt().max(1e-9);
    [v[0] / l, v[1] / l, v[2] / l]
}

fn n(x: f32) -> Option<f64> {
    Some(x as f64)
}

/// Post colours are byte triples (0..255) in the feed, as in the JSON.
fn color3(v: &[f32]) -> [f32; 3] {
    [v[0] / 255.0, v[1] / 255.0, v[2] / 255.0]
}

fn arr3(v: &[f32], d: [f32; 3]) -> [f32; 3] {
    [sane(n(v[0]), d[0]), sane(n(v[1]), d[1]), sane(n(v[2]), d[2])]
}

/// The post settings the JSON path would have built from the same game state.
pub fn decode_post(p: &FeedPost, path: impl Fn(u32) -> Option<String>) -> PostFeed {
    let mut out = PostFeed { present: true, ..Default::default() };
    if p.present == 0 {
        return out;
    }
    if p.flags & PF_UBER != 0 {
        out.bloom_scale = sane(n(p.uber_bloom_scale), 0.0);
        out.bloom_threshold = sane(n(p.uber_bloom_threshold), 1.0);
        out.bloom_enabled = out.bloom_scale > 0.0;
        let d = &p.uber_dof;
        out.uber = Some(Uber {
            bloom_scale: out.bloom_scale,
            bloom_threshold: out.bloom_threshold,
            bloom_kernel: sane(n(d[0]), 16.0),
            blur_kernel: sane(n(d[1]), 16.0),
            different_bloom_kernel: p.flags & PF_UBER_DIFF_KERNEL != 0,
            falloff: sane(n(d[2]), 2.0),
            max_near: sane(n(d[3]), 1.0),
            max_far: sane(n(d[4]), 1.0),
            focus_radius: sane(n(d[5]), 2000.0).max(1.0),
            focus_distance: sane(n(d[6]), 0.0),
        });
    }
    if p.material_effect != NONE {
        let n = ((p.flags >> PF_MATERIALS_SHIFT) & 15).max(1);
        out.material_effects = (0..n).filter_map(|k| path(p.material_effect + k)).collect();
        out.material_effect = out.material_effects.first().cloned();
    }
    if p.flags & PF_GOD_RAYS != 0 {
        let g = &p.god;
        out.god_rays = Some(GodRays {
            exposure: sane(n(g[0]), 0.0),
            decay: sane(n(g[1]), 0.0),
            magnitude: sane(n(g[2]), 0.0),
            color: color3(&g[3..6]),
            screen: [sane(n(g[6]), 0.0), sane(n(g[7]), 0.0)],
        });
    }
    if p.flags & PF_LAST_SETTINGS != 0 {
        let on = |f: u32| p.flags & f != 0;
        if !out.bloom_enabled && on(PF_LS_BLOOM) {
            out.bloom_scale = sane(n(p.ls_bloom_scale), 0.0);
            out.bloom_threshold = sane(n(p.ls_bloom_threshold), 1.0).max(0.0);
            out.bloom_enabled = out.bloom_scale > 0.0;
        }
        if on(PF_LS_BALANCE) {
            let b = &p.balance;
            out.balance = Some([color3(&b[0..3]), color3(&b[3..6]), color3(&b[6..9])]);
        }
        if on(PF_LS_SCENE) {
            out.desaturation = sane(n(p.desaturation), 0.0);
            out.scene_shadows = arr3(&p.shadows, [0.0; 3]);
            out.scene_midtones = arr3(&p.midtones, [1.0; 3]);
            out.scene_highlights = arr3(&p.highlights, [1.0; 3]);
        }
        out.tone = ToneMap {
            enabled: on(PF_LS_TONE),
            min_luminosity: sane(n(p.tone_min), 0.4),
            max_luminosity: sane(n(p.tone_max), 20.0),
            brightness_scale: sane(n(p.tone_scale), 1.0),
        };
    }
    out
}

/// B-266: the post materials' parameters as (material path, parameter name, value); `first` is the path index of the
/// first MaterialEffect path, which `FeedParam::material` counts from.
pub fn decode_params(first: u32, params: &[FeedParam], path: impl Fn(u32) -> Option<String>) -> Vec<(String, String, f32)> {
    params
        .iter()
        .filter_map(|q| Some((path(first.checked_add(q.material)?)?, path(q.name)?, q.value)))
        .filter(|q| q.2.is_finite())
        .collect()
}

/// A decoded feed: the frame, and whether it carried fog and post.
pub struct Decoded {
    pub frame: Frame,
    pub has_fog_post: bool,
}

/// # Safety
/// `b`'s pointers must be valid for their counts, and `paths` entries NUL-terminated strings.
pub unsafe fn decode(b: &FeedBin) -> Result<Decoded, String> {
    if !(MIN_VERSION..=VERSION).contains(&b.version) {
        return Err(format!("feed version {}, expected {MIN_VERSION}..{VERSION}", b.version));
    }
    let lights = arr(b.lights, b.light_count);
    let paths: Vec<String> = arr(b.paths, b.path_count)
        .iter()
        .map(|&p| if p.is_null() { String::new() } else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() })
        .collect();
    let sh = arr(b.sh, b.sh_count * 27);
    let path = |i: u32| paths.get(i as usize).cloned();
    let mut out = Vec::with_capacity(lights.len());
    for l in lights {
        let kind = match l.kind {
            KIND_POINT => Kind::Point,
            KIND_SPOT => Kind::Spot,
            KIND_DIRECTIONAL => Kind::Directional,
            KIND_SH => Kind::Sh,
            KIND_SKY => Kind::Sky,
            _ => continue,
        };
        let raw_slot = kind.carries_raw();
        let sky_raw = kind == Kind::Sky && l.flags & LF_SKY_RAW != 0;
        let sh_coeffs = (l.sh >= 0).then(|| l.sh as usize * 27).filter(|&o| o + 27 <= sh.len()).map(|o| {
            let mut c = [[0f32; 9]; 3];
            for (ch, row) in c.iter_mut().enumerate() {
                row.copy_from_slice(&sh[o + ch * 9..o + ch * 9 + 9]);
            }
            c
        });
        out.push(Light {
            kind,
            name: String::new(),
            env: if l.env == NONE { None } else { path(l.env) },
            enabled: l.flags & LF_ENABLED != 0,
            color: l.color,
            pos: if sky_raw || kind == Kind::Sky { [0.0; 3] } else { swz(l.pos) },
            travel: if sky_raw || kind == Kind::Sky { [0.0; 3] } else { norm(swz(l.axis)) },
            radius: l.radius,
            falloff: l.falloff,
            inner_deg: l.inner_deg,
            outer_deg: l.outer_deg,
            baked: l.flags & LF_BAKED != 0,
            cast_dynamic_shadows: l.flags & LF_CAST_DYN_SHADOWS != 0,
            affects_static: l.flags & LF_CHAN_STATIC != 0,
            affects_dynamic: l.flags & LF_CHAN_DYNAMIC != 0,
            lower: if raw_slot { [0.0; 3] } else { l.lower },
            raw: if raw_slot { l.lower } else if sky_raw { l.pos } else { [0.0; 3] },
            raw_lower: if sky_raw { l.axis } else { [0.0; 3] },
            sh: sh_coeffs,
            // B-205: the shadow light's fields (B-189's modulated shadows).
            cast_shadows: l.flags & LF_CAST_SHADOWS != 0,
            modulated: l.flags & LF_MODULATED != 0,
            mod_shadow: l.mod_shadow,
            mod_fade_exp: l.mod_fade_exp,
            shadow_falloff_exp: l.shadow_falloff_exp,
            min_shadow_falloff_radius: l.min_shadow_falloff_radius,
        });
    }
    let has_fog_post = b.flags & FLAG_FOG_POST != 0;
    let fog = (has_fog_post && b.fog.present != 0).then(|| Fog {
        color: b.fog.color,
        light_color: b.fog.light_color,
        density: b.fog.density,
        height: b.fog.height,
        start: b.fog.start,
        extinction: b.fog.extinction,
    });
    let mut post = if has_fog_post { decode_post(&b.post, path) } else { PostFeed::default() };
    if has_fog_post && b.version >= 3 && b.post.material_effect != NONE {
        // Read through a raw pointer: the field is past the end of a version 2 host's struct.
        let params = unsafe { std::ptr::addr_of!(b.params).read_unaligned() };
        post.material_params = decode_params(b.post.material_effect, arr(params, b.param_count.min(256)), path);
    }
    Ok(Decoded { frame: Frame::assemble(out, fog, post), has_fog_post })
}

/// Differences between two frames' `Debug` text, comparing numbers within a relative `tol` (the JSON feed writes 7
/// significant digits). Empty when they match.
pub fn diff_frames(a: &Frame, b: &Frame, tol: f64) -> Vec<String> {
    fn tokens(s: &str) -> Vec<(bool, String)> {
        let mut out: Vec<(bool, String)> = Vec::new();
        let mut cur = String::new();
        let mut num = false;
        for c in s.chars() {
            let is_num = c.is_ascii_digit() || c == '.' || c == '-' || (num && (c == 'e' || c == '+'));
            if is_num != num && !cur.is_empty() {
                out.push((num, std::mem::take(&mut cur)));
            }
            num = is_num;
            cur.push(c);
        }
        if !cur.is_empty() {
            out.push((num, cur));
        }
        out
    }
    // The binary feed does not carry the post chain's class names (nothing live reads them), the JSON feed not the post
    // materials' parameters (B-266).
    let (mut a, mut b) = (a.clone(), b.clone());
    a.post.chain.clear();
    b.post.chain.clear();
    a.post.material_params.clear();
    b.post.material_params.clear();
    let (sa, sb) = (format!("{a:#?}"), format!("{b:#?}"));
    let (ta, tb) = (tokens(&sa), tokens(&sb));
    let mut diffs = Vec::new();
    if ta.len() != tb.len() {
        diffs.push(format!("structure differs ({} vs {} tokens)", ta.len(), tb.len()));
    }
    let mut ctx = String::new();
    for (x, y) in ta.iter().zip(&tb) {
        if !x.0 {
            ctx = x.1.lines().last().unwrap_or("").trim().to_string();
        }
        let same = if x.0 && y.0 {
            match (x.1.parse::<f64>(), y.1.parse::<f64>()) {
                (Ok(p), Ok(q)) => (p - q).abs() <= tol * p.abs().max(q.abs()).max(1e-3),
                _ => x.1 == y.1,
            }
        } else {
            x.1 == y.1
        };
        if !same {
            diffs.push(format!("{ctx} {} vs {}", x.1, y.1));
            if diffs.len() >= 12 {
                break;
            }
        }
    }
    diffs
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layouts() {
        assert_eq!(size_of::<FeedBin>(), 24 + 24 + 44 + 180 + 8);
    }

    #[test]
    fn roundtrip_matches_json_semantics() {
        let json = r#"{"lights":[
          {"class":"DirectionalLightComponent","props":{"LightToWorld":[1,0,0,0, 0,1,0,0, 0.3,0.2,-0.9,0, 0,0,0,1],"Brightness":2,"LightColor":[255,128,0,255],"bEnabled":true,"CastDynamicShadows":true,"LightingChannels":{"Static":true,"Dynamic":true}}},
          {"class":"PointLightComponent","props":{"LightToWorld":[1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1],"Brightness":1.5,"LightColor":[10,20,30,255],"bEnabled":true,"Radius":500,"FalloffExponent":2,"LightEnvironment":"A.B.DynamicLightEnvironmentComponent_3","LightingChannels":{"Static":false,"Dynamic":true},"CastShadows":true,"LightShadowMode":"LightShadow_Modulate","ModShadowColor":[0.6437,0.6242,0.6226,1],"ModShadowFadeoutExponent":4,"ShadowFalloffExponent":1.5,"MinShadowFalloffRadius":423}}
        ],"fog":[{"class":"HeightFogComponent","props":{"bEnabled":true,"LightColor":[200,210,220,255],"LightBrightness":0.5,"Density":0.001,"Height":100,"StartDistance":10,"ExtinctionDistance":9000}}],
        "post":{"chain":[{"class":"SfUberPostProcessEffect","props":{"BloomScale":0.7,"BloomThreshold":0.9,"BloomKernelSize":12,"BlurKernelSize":14,"UseDifferentKernelForBloom":true,"FalloffExponent":2.5,"MaxNearBlurAmount":0.8,"MaxFarBlurAmount":0.9,"FocusInnerRadius":1500,"FocusDistance":300}}],"current":{"LastSettings":{"bEnableColourBalanceEffect":true,"ColourBalance_Shadows":[255,200,200,255],"ColourBalance_Midtones":[255,255,255,255],"ColourBalance_Highlights":[250,250,250,255],"bEnableToneMappingEffect":true,"ToneMapping_BrightnessScale":1.2}}}}"#;
        let jf = Frame::from_json(json).unwrap();
        let lights = [
            FeedLight {
                kind: KIND_DIRECTIONAL,
                flags: LF_ENABLED | LF_CAST_DYN_SHADOWS | LF_CHAN_STATIC | LF_CHAN_DYNAMIC,
                env: NONE,
                radius: 0.0,
                falloff: 2.0,
                inner_deg: 0.0,
                outer_deg: 44.0,
                pos: [0.0; 3],
                axis: [0.3, 0.2, -0.9],
                color: [2.0 * (255.0 / 255.0), 2.0 * (128.0 / 255.0), 0.0],
                // B-245: directional lights carry their raw LightColor / 255 here.
                lower: [1.0, 128.0 / 255.0, 0.0],
                sh: -1,
                mod_shadow: [1.0; 3],
                mod_fade_exp: 3.0,
                shadow_falloff_exp: 2.0,
                min_shadow_falloff_radius: 0.0,
            },
            FeedLight {
                kind: KIND_POINT,
                flags: LF_ENABLED | LF_CHAN_DYNAMIC | LF_CAST_SHADOWS | LF_MODULATED,
                env: 0,
                radius: 500.0,
                falloff: 2.0,
                inner_deg: 0.0,
                outer_deg: 44.0,
                pos: [10.0, 20.0, 30.0],
                axis: [0.0, 0.0, 1.0],
                color: [1.5 * (10.0 / 255.0), 1.5 * (20.0 / 255.0), 1.5 * (30.0 / 255.0)],
                lower: [10.0 / 255.0, 20.0 / 255.0, 30.0 / 255.0],
                sh: -1,
                mod_shadow: [0.6437, 0.6242, 0.6226],
                mod_fade_exp: 4.0,
                shadow_falloff_exp: 1.5,
                min_shadow_falloff_radius: 423.0,
            },
        ];
        let paths = [std::ffi::CString::new("A.B.DynamicLightEnvironmentComponent_3").unwrap()];
        let pp = [paths[0].as_ptr()];
        let mut post = FeedPost { present: 1, flags: PF_UBER | PF_LAST_SETTINGS | PF_LS_BALANCE | PF_LS_TONE, ..Default::default() };
        post.uber_bloom_scale = 0.7;
        post.uber_bloom_threshold = 0.9;
        post.uber_dof = [12.0, 14.0, 2.5, 0.8, 0.9, 1500.0, 300.0];
        post.flags |= PF_UBER_DIFF_KERNEL;
        post.balance = [255.0, 200.0, 200.0, 255.0, 255.0, 255.0, 250.0, 250.0, 250.0];
        post.tone_scale = 1.2;
        let bin = FeedBin {
            version: VERSION,
            flags: FLAG_FOG_POST,
            light_count: 2,
            path_count: 1,
            sh_count: 0,
            param_count: 0,
            lights: lights.as_ptr(),
            paths: pp.as_ptr(),
            sh: std::ptr::null(),
            fog: FeedFog {
                present: 1,
                color: [200.0 / 255.0 * 0.5, 210.0 / 255.0 * 0.5, 220.0 / 255.0 * 0.5],
                light_color: [200.0 / 255.0, 210.0 / 255.0, 220.0 / 255.0],
                density: 0.001,
                height: 100.0,
                start: 10.0,
                extinction: 9000.0,
            },
            post,
            params: std::ptr::null(),
        };
        let d = unsafe { decode(&bin) }.unwrap();
        assert!(d.has_fog_post);
        let diffs = diff_frames(&jf, &d.frame, 1e-5);
        assert!(diffs.is_empty(), "{diffs:#?}");
    }

    /// B-227: every MaterialEffect path the host sends, in order; an older host's single path still decodes.
    #[test]
    fn post_material_effects() {
        let paths = ["X.Light", "MIS_CribGeneric.Crib_MAT01", "FX_EnvironmentLibrary.Fire.PostProcessIntensity_MIC"];
        let path = |i: u32| paths.get(i as usize).map(|s| s.to_string());
        let mut post = FeedPost { present: 1, material_effect: 1, flags: 2 << PF_MATERIALS_SHIFT, ..Default::default() };
        let p = decode_post(&post, path);
        assert_eq!(p.material_effects, [paths[1], paths[2]]);
        assert_eq!(p.material_effect.as_deref(), Some(paths[1]));
        post.flags = 0;
        assert_eq!(decode_post(&post, path).material_effects, [paths[1]]);
        post.material_effect = NONE;
        assert!(decode_post(&post, path).material_effects.is_empty());
    }

    /// B-266: parameters name their material by its place among the MaterialEffect paths; bad indices and NaN drop.
    #[test]
    fn post_params() {
        let paths = ["X.Light", "GEN_PostProcess.AmbushPP_MAT01", "C1HUD.SlowmoEffect_MatInst", "PARAMAmount", "Other"];
        let path = |i: u32| paths.get(i as usize).map(|s| s.to_string());
        let params = [
            FeedParam { material: 1, name: 3, value: 0.5 },
            FeedParam { material: 0, name: 4, value: 2.0 },
            FeedParam { material: 9, name: 3, value: 1.0 },
            FeedParam { material: 1, name: 99, value: 1.0 },
            FeedParam { material: 1, name: 4, value: f32::NAN },
        ];
        let got = decode_params(1, &params, path);
        assert_eq!(
            got,
            [
                ("C1HUD.SlowmoEffect_MatInst".to_string(), "PARAMAmount".to_string(), 0.5),
                ("GEN_PostProcess.AmbushPP_MAT01".to_string(), "Other".to_string(), 2.0),
            ]
        );
    }

    /// B-266: a version 3 feed carries parameters with its post; a version 2 one (no `params` field) still decodes.
    #[test]
    fn versions() {
        let names = ["A.Grade", "C1HUD.SlowmoEffect_MatInst", "PARAMAmount"].map(|s| std::ffi::CString::new(s).unwrap());
        let pp: Vec<*const c_char> = names.iter().map(|c| c.as_ptr()).collect();
        let params = [FeedParam { material: 1, name: 2, value: 0.75 }];
        let post = FeedPost { present: 1, material_effect: 0, flags: 2 << PF_MATERIALS_SHIFT, ..Default::default() };
        let mut bin = FeedBin {
            version: VERSION,
            flags: FLAG_FOG_POST,
            light_count: 0,
            path_count: 3,
            sh_count: 0,
            param_count: 1,
            lights: std::ptr::null(),
            paths: pp.as_ptr(),
            sh: std::ptr::null(),
            fog: FeedFog::default(),
            post,
            params: params.as_ptr(),
        };
        let d = unsafe { decode(&bin) }.unwrap();
        assert_eq!(d.frame.post.param("C1HUD.SlowmoEffect_MatInst", "PARAMAmount"), Some(0.75));
        bin.version = 2;
        bin.param_count = 0;
        let d = unsafe { decode(&bin) }.unwrap();
        assert!(d.frame.post.material_params.is_empty());
        assert_eq!(d.frame.post.material_effects.len(), 2);
        bin.version = 1;
        assert!(unsafe { decode(&bin) }.is_err());
    }
}
