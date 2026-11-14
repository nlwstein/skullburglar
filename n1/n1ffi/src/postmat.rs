//! B-266 / B-267: the game's post-process materials besides the colour grade, hand-ported as n1 post passes.
//!
//! The host feeds every shown MaterialEffect material of the player's post chain (paths) and, since feed version 3,
//! their live scalar parameters (`PostFeed::material_params`). n1's matdef compiler builds surface materials only (no
//! SceneTexture, ScreenPosition or post template), and the set of post materials is small and closed, so each one
//! the player can see is ported by hand to a fragment entry point in `post.wgsl`, recognised here by its path.
//! The decoded graphs live in the per-material comments below.
//!
//! An effect runs only while its material is in the chain and its strength is above zero, so it costs nothing
//! otherwise. `BOTS_N1_POST_MATS=0` turns them all off (the old path: grade only).

use lightfeed::PostFeed;

/// The ported post materials.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Kind {
    /// Gangsta Fire (slow motion): `C1HUD.SlowmoEffect_MatInst`, an instance of `FX_GangsterFire.GangsterFire_MAT02`.
    /// Scalar `PARAMAmount` (0..1): barrel warp, a burn of darks and mids, a rotating EyeMask ring with a radially
    /// smeared copy of the scene outside it, panned fire streaks in the corners.
    Gangsta,
    /// Death (`UI_DeathEffect.FaderDeath_MIC`): scalars `StartTime` and `EffectLength` (default 2). Over the first
    /// half the scene desaturates, darkens behind a closing vignette and fades to black; over the second half animated
    /// dark red clouds (`DeathScreenBackground_Alphas`, `HeatVisionClouds_Alpha`) fade in and stay.
    Death,
    /// Critical-health damage (`FX_Damage.DamageEffectMaterial_MIC`, B-292): scalar `CriticalHealthFadeParameter`
    /// (0..1, driven by the HUD from the player's health) fades the scene toward 75% desaturation. The material also
    /// reads the `DamageEffectTexture` render target (hit arrows; its blue channel indexes `DamageRamp`, whose alpha
    /// brightens and whose colour overlays the scene). With no hit the target is black, which leaves exactly the fade
    /// term. The host reads the target back and hands n1 its picture (`n1_set_damage_tex`).
    Damage,
    /// Sniper scope (`C1HUD.SniperScope_MIC`, an instance of `SniperScope_V02_MAT`, B-295): in the player's chain only
    /// while scoped. The scene through a lens mask (blue channel of `SniperScope_v02_EFCT`, sampled along its diagonal at
    /// the squared distance from the centre) with a darkened top edge, then the reticle drawn by the red channel
    /// (dark lines) and the green channel (a lit dot) of the same texture. Scalar `g0.x` is the material's one scalar
    /// parameter, which picks the 16:9 or the narrower screen mapping; without the parameter it is 0 (16:9).
    Scope,
    /// B-345 (#32), the stylized counter-kill ("Merk You Space") backdrop: `FX_MerkYouSpaceRescource.BKGAndGhost_MIC`
    /// (the `MerkSpaceWall_PP01` chain). Scalars `HitTime`, `EffectLength` (0.25), `LV` (1) in g0.xyz. The parchment
    /// `Wall` times clamp(3 x lerp(desaturate(scene, 80%), 0.5, -0.1), 0, 2) (the white room becomes twice the parchment,
    /// the fighters a contrasty sepia), a red flash and two zoomed grey ghosts of the scene for `EffectLength` after
    /// each `HitTime` (the wall zooms and wobbles for twice that), times a pulsing vignette. Decoded in `post.wgsl`.
    MerkBkg,
    /// B-345, the kill's start (`FadeIn_MAT_MIC`, chain `MerkStart_PP`): scalars `StartTime`, `EffectLength` (0.25).
    /// The skull logo (`GLogo_02`, red channel) over white, opening onto the scene as it grows to fill the screen.
    MerkFadeIn,
    /// B-345, the kill's end (`BurnOut_MIC`, chain `MerkEnd_PP`): scalars `StartTime`, `EffectLength` (1.5). The scene
    /// burnt to white (levels with input max 0.1, 40% desaturated) easing back to normal over EffectLength / 1.5.
    MerkBurnOut,
}

impl Kind {
    /// The material paths (outermost instance, or the parent material) that select this effect.
    pub const fn paths(self) -> &'static [&'static str] {
        match self {
            Kind::Gangsta => &["C1HUD.SlowmoEffect_MatInst", "FX_GangsterFire.GangsterFire_MAT02"],
            Kind::Death => &["UI_DeathEffect.FaderDeath_MIC", "UI_DeathEffect.FaderDeath_MAT"],
            Kind::Damage => &["FX_Damage.DamageEffectMaterial_MIC", "FX_Damage.DamageEffectMaterial"],
            Kind::Scope => &["C1HUD.SniperScope_MIC", "C1HUD.SniperScope_V02_MAT"],
            Kind::MerkBkg => &["FX_MerkYouSpaceRescource.BKGAndGhost_MIC", "FX_MerkYouSpaceRescource.BKGAndGhost_MAT"],
            Kind::MerkFadeIn => &["FX_MerkYouSpaceRescource.FadeIn_MAT_MIC", "FX_MerkYouSpaceRescource.FadeIn_MAT"],
            Kind::MerkBurnOut => &["FX_MerkYouSpaceRescource.BurnOut_MIC", "FX_MerkYouSpaceRescource.BurnOut_MAT"],
        }
    }

    /// The fragment entry point in `post.wgsl`.
    pub const fn fs(self) -> &'static str {
        match self {
            Kind::Gangsta => "fs_gangsta",
            Kind::Death => "fs_death",
            Kind::Damage => "fs_damage",
            Kind::Scope => "fs_scope",
            Kind::MerkBkg => "fs_merk_bkg",
            Kind::MerkFadeIn => "fs_merk_fadein",
            Kind::MerkBurnOut => "fs_merk_burnout",
        }
    }

    /// The material's textures, bound as `tex0` and `tex1` ("" = none).
    pub const fn textures(self) -> [&'static str; 2] {
        match self {
            Kind::Gangsta => ["FX_GangsterFire.streak", "FX_GangsterFire.EyeMask"],
            Kind::Death => ["UI_DeathEffect.DeathScreenBackground_Alphas", "UI_DeathEffect.HeatVisionClouds_Alpha"],
            Kind::Damage => ["FX_Damage.DamageRamp", ""],
            Kind::Scope => ["C1HUD.SniperScope_v02_EFCT", ""],
            Kind::MerkBkg => ["FX_MerkYouSpaceRescource.Wall", ""],
            Kind::MerkFadeIn => ["FX_MerkYouSpaceRescource.GLogo_02", ""],
            Kind::MerkBurnOut => ["", ""],
        }
    }

    pub const ALL: [Kind; 7] =
        [Kind::Gangsta, Kind::Death, Kind::Damage, Kind::Scope, Kind::MerkBkg, Kind::MerkFadeIn, Kind::MerkBurnOut];

    /// The stylized counter-kill's effects (B-345), drawn only with `BOTS_N1_COUNTERKILL=native`.
    pub const fn is_merk(self) -> bool {
        matches!(self, Kind::MerkBkg | Kind::MerkFadeIn | Kind::MerkBurnOut)
    }

    pub fn of(path: &str) -> Option<Kind> {
        Kind::ALL.into_iter().find(|k| k.paths().iter().any(|p| p.eq_ignore_ascii_case(path)))
    }
}

/// Every texture a ported effect can bind (loaded once in the background when post materials are on).
pub fn all_textures() -> Vec<&'static str> {
    let mut v: Vec<&'static str> = Kind::ALL.iter().flat_map(|k| k.textures()).filter(|t| !t.is_empty()).collect();
    v.sort_unstable();
    v.dedup();
    v
}

/// One active effect: its kind and the parameters its pass reads (`g0` in `post.wgsl`).
#[derive(Clone, Debug, PartialEq)]
pub struct Effect {
    pub kind: Kind,
    pub params: [f32; 4],
}

/// `BOTS_N1_POST_LOG=1`: print the post-material list every time it changes (the host logs it once per chain object,
/// which misses a material added to a live chain, e.g. `ScenarioEdgeDetect_MAT`).
fn log_chain(mats: &[String], params: &[(String, String, f32)]) {
    use std::sync::{Mutex, OnceLock};
    static ON: OnceLock<bool> = OnceLock::new();
    static LAST: Mutex<Vec<String>> = Mutex::new(Vec::new());
    if !*ON.get_or_init(|| std::env::var("BOTS_N1_POST_LOG").is_ok_and(|v| v != "0")) {
        return;
    }
    static LAST_PARAMS: Mutex<Vec<(String, String, f32)>> = Mutex::new(Vec::new());
    let Ok(mut last) = LAST.lock() else { return };
    if last.as_slice() != mats {
        eprintln!("n1ffi: post materials now {mats:?}");
        *last = mats.to_vec();
    }
    // The scalar parameters the feed carries for the chain's materials (the scope's aspect switch among them).
    let Ok(mut lp) = LAST_PARAMS.lock() else { return };
    if lp.as_slice() != params {
        for (m, n, v) in params {
            eprintln!("n1ffi: post param {m} {n} = {v}");
        }
        *lp = params.to_vec();
    }
}

/// `BOTS_N1_DAMAGE_FX=0` leaves the critical-health desaturation out (the pre-B-292 path).
fn damage_fx_on() -> bool {
    use std::sync::OnceLock;
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var("BOTS_N1_DAMAGE_FX").map_or(true, |v| v != "0"))
}

/// `BOTS_N1_SCOPE_FX=0` leaves the sniper scope overlay out (the pre-B-295 path: the scene is shown unmasked).
fn scope_fx_on() -> bool {
    use std::sync::OnceLock;
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| std::env::var("BOTS_N1_SCOPE_FX").map_or(true, |v| v != "0"))
}

/// B-345 (#32): n1 draws the stylized counter-kills itself (their post materials here, their lights in `light.rs`; the
/// host culls the level to the game's own view of the kill). The default since 1.9.3. `BOTS_N1_COUNTERKILL=port` (the
/// game's renderer draws those frames) or `off` (neither), or 1.9.1's `BOTS_N1_COUNTERKILL_PORT` when the new variable
/// is unset, leave all of that out. Mirrors the host's `bots::counterkill::ParseMode` (src/counterkill_port.h).
pub fn merk_native_on() -> bool {
    use std::sync::OnceLock;
    static ON: OnceLock<bool> = OnceLock::new();
    *ON.get_or_init(|| {
        merk_native_mode(
            std::env::var("BOTS_N1_COUNTERKILL").ok().as_deref(),
            std::env::var("BOTS_N1_COUNTERKILL_PORT").ok().as_deref(),
        )
    })
}

/// `merk_native_on` for given values of `BOTS_N1_COUNTERKILL` and `BOTS_N1_COUNTERKILL_PORT` (None = unset).
pub fn merk_native_mode(counterkill: Option<&str>, legacy_port: Option<&str>) -> bool {
    match counterkill.filter(|v| !v.is_empty()) {
        Some(v) => !(v.eq_ignore_ascii_case("port") || v.eq_ignore_ascii_case("off") || v == "0"),
        None => legacy_port.is_none_or(str::is_empty),
    }
}

/// Whether a post feed is in a stylized counter-kill (a `FX_MerkYouSpaceRescource.` material in the chain).
pub fn in_merk_kill(p: &PostFeed) -> bool {
    p.present && p.material_effects.iter().any(|m| is_merk_path(m))
}

pub fn is_merk_path(m: &str) -> bool {
    const PKG: &str = "fx_merkyouspacerescource.";
    m.len() > PKG.len() && m[..PKG.len()].eq_ignore_ascii_case(PKG)
}

/// The active ported effects of a post feed, in chain order. A material whose strength is zero (or not positive) is
/// left out, so the pass isn't recorded at all.
pub fn effects(p: &PostFeed) -> Vec<Effect> {
    effects_with(p, merk_native_on())
}

/// [`effects`] with the counter-kill switch given (`merk`: draw the stylized counter-kill's effects).
pub fn effects_with(p: &PostFeed, merk: bool) -> Vec<Effect> {
    let mut out = Vec::new();
    if !p.present {
        return out;
    }
    log_chain(&p.material_effects, &p.material_params);
    for m in &p.material_effects {
        let Some(kind) = Kind::of(m) else { continue };
        if kind == Kind::Damage && !damage_fx_on() {
            continue;
        }
        if kind == Kind::Scope && !scope_fx_on() {
            continue;
        }
        if kind.is_merk() && !merk {
            continue;
        }
        if out.iter().any(|e: &Effect| e.kind == kind) {
            continue;
        }
        let e = match kind {
            Kind::Gangsta => {
                // A version 2 feed has no parameters: the material in the chain means the mode is on.
                let a = p.param(m, "PARAMAmount").unwrap_or(1.0);
                if a.is_finite() && a > 0.0 {
                    Some(Effect { kind, params: [a.min(1.0), 0.0, 0.0, 0.0] })
                } else {
                    None
                }
            }
            Kind::Damage => {
                // Kept with a zero fade too: the hit-direction picture (B-292) may be showing; the renderer drops the
                // pass when neither the fade nor a picture is there.
                let a = p.param(m, "CriticalHealthFadeParameter").unwrap_or(0.0);
                let a = if a.is_finite() { a.clamp(0.0, 1.0) } else { 0.0 };
                Some(Effect { kind, params: [a, 0.0, 0.0, 0.0] })
            }
            Kind::Scope => {
                // Its only scalar parameter, whatever its name (the game sets it from the screen's aspect ratio).
                let k = p.material_params.iter().find(|(pm, _, _)| pm.eq_ignore_ascii_case(m)).map_or(0.0, |(_, _, v)| *v);
                let k = if k.is_finite() { k.clamp(0.0, 1.0) } else { 0.0 };
                Some(Effect { kind, params: [k, 0.0, 0.0, 0.0] })
            }
            // B-345: the game sets HitTime / StartTime (world time) on the instances as the kill plays. Without them there
            // is no flash (HitTime far in the past), and the wipe and burn are over (the plain scene).
            Kind::MerkBkg => {
                let hit = p.param(m, "HitTime").filter(|v| v.is_finite()).unwrap_or(-1.0e9);
                let len = p.param(m, "EffectLength").filter(|v| v.is_finite() && *v > 0.0).unwrap_or(0.25);
                let lv = p.param(m, "LV").filter(|v| v.is_finite()).unwrap_or(1.0);
                Some(Effect { kind, params: [hit, len, lv, 0.0] })
            }
            Kind::MerkFadeIn | Kind::MerkBurnOut => {
                let start = p.param(m, "StartTime").filter(|v| v.is_finite()).unwrap_or(-1.0e9);
                let dflt = if kind == Kind::MerkFadeIn { 0.25 } else { 1.5 };
                let len = p.param(m, "EffectLength").filter(|v| v.is_finite() && *v > 0.0).unwrap_or(dflt);
                Some(Effect { kind, params: [start, len, 0.0, 0.0] })
            }
            Kind::Death => {
                // StartTime is in the material's Time (the world time n1 already has); without it there's no ramp.
                let start = p.param(m, "StartTime");
                let len = p.param(m, "EffectLength").unwrap_or(2.0);
                match start {
                    Some(t0) if t0.is_finite() && len.is_finite() => {
                        Some(Effect { kind, params: [t0, len.max(1e-3), 0.0, 0.0] })
                    }
                    _ => None,
                }
            }
        };
        out.extend(e);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn merk_native_mode_matches_the_host() {
        // Same table as tests/counterkill_port_test.cpp: native unless port/off is asked for.
        assert!(merk_native_mode(None, None));
        assert!(merk_native_mode(Some(""), None));
        assert!(merk_native_mode(Some("native"), Some("0")));
        assert!(merk_native_mode(Some("bogus"), None));
        assert!(!merk_native_mode(Some("Port"), None));
        assert!(!merk_native_mode(Some("off"), None));
        assert!(!merk_native_mode(Some("0"), None));
        assert!(!merk_native_mode(Some(""), Some("0")));
        assert!(!merk_native_mode(None, Some("0")));
        assert!(!merk_native_mode(None, Some("1")));
        assert!(merk_native_mode(None, Some("")));
    }

    fn feed(mats: &[&str], params: &[(&str, &str, f32)]) -> PostFeed {
        let mut p = PostFeed::default();
        p.present = true;
        p.material_effects = mats.iter().map(|s| s.to_string()).collect();
        p.material_params = params.iter().map(|(m, n, v)| (m.to_string(), n.to_string(), *v)).collect();
        p
    }

    #[test]
    fn gangsta_amount() {
        let m = "C1HUD.SlowmoEffect_MatInst";
        let e = effects(&feed(&["GEN_PostProcess.AmbushPP_MAT01", m], &[(m, "PARAMAmount", 0.4)]));
        assert_eq!(e, [Effect { kind: Kind::Gangsta, params: [0.4, 0.0, 0.0, 0.0] }]);
        // Off at zero, clamped at one, on without parameters (version 2 feed).
        assert!(effects(&feed(&[m], &[(m, "paramamount", 0.0)])).is_empty());
        assert_eq!(effects(&feed(&[m], &[(m, "PARAMAmount", 3.0)]))[0].params[0], 1.0);
        assert_eq!(effects(&feed(&[m], &[]))[0].params[0], 1.0);
        assert!(effects(&feed(&[m], &[(m, "PARAMAmount", f32::NAN)])).is_empty());
    }

    #[test]
    fn death_times() {
        let m = "UI_DeathEffect.FaderDeath_MIC";
        let e = effects(&feed(&["GEN_PostProcess.FlyingLevel_MAT01", m], &[(m, "StartTime", 12.5)]));
        assert_eq!(e, [Effect { kind: Kind::Death, params: [12.5, 2.0, 0.0, 0.0] }]);
        let e = effects(&feed(&[m], &[(m, "StartTime", 3.0), (m, "EffectLength", 4.0)]));
        assert_eq!(e[0].params[..2], [3.0, 4.0]);
        // No start time: nothing to ramp from, so no pass.
        assert!(effects(&feed(&[m], &[(m, "EffectLength", 4.0)])).is_empty());
        // Both in one chain, in chain order.
        let g = "C1HUD.SlowmoEffect_MatInst";
        let e = effects(&feed(&[m, g], &[(m, "StartTime", 1.0), (g, "PARAMAmount", 1.0)]));
        assert_eq!(e.iter().map(|x| x.kind).collect::<Vec<_>>(), [Kind::Death, Kind::Gangsta]);
    }

    #[test]
    fn damage_fade() {
        let m = "FX_Damage.DamageEffectMaterial_MIC";
        let e = effects(&feed(&["GEN_PostProcess.AmbushPP_MAT01", m], &[(m, "CriticalHealthFadeParameter", 0.65)]));
        assert_eq!(e, [Effect { kind: Kind::Damage, params: [0.65, 0.0, 0.0, 0.0] }]);
        // In the chain with a zero fade (or without the parameter) it stays, for the hit picture (the renderer drops the
        // pass when no picture is showing); clamped at one.
        assert_eq!(effects(&feed(&[m], &[(m, "CriticalHealthFadeParameter", 0.0)]))[0].params[0], 0.0);
        assert_eq!(effects(&feed(&[m], &[]))[0].params[0], 0.0);
        assert!(effects(&feed(&["a.b"], &[])).is_empty());
        assert_eq!(effects(&feed(&[m], &[(m, "CriticalHealthFadeParameter", 2.0)]))[0].params[0], 1.0);
    }

    #[test]
    fn scope_in_chain() {
        let m = "C1HUD.SniperScope_MIC";
        // Present in the chain: on, with the parameter (0 without it).
        assert_eq!(effects(&feed(&["a.b", m], &[]))[0], Effect { kind: Kind::Scope, params: [0.0; 4] });
        assert_eq!(effects(&feed(&[m], &[(m, "AnyName", 1.0)]))[0].params[0], 1.0);
        assert_eq!(effects(&feed(&[m], &[("other", "x", 1.0)]))[0].params[0], 0.0);
        assert!(effects(&feed(&["a.b"], &[])).is_empty());
        assert_eq!(Kind::of("c1hud.sniperscope_v02_mat"), Some(Kind::Scope));
    }

    #[test]
    fn merk_kill() {
        let bkg = "FX_MerkYouSpaceRescource.BKGAndGhost_MIC";
        let f = feed(&["GEN_PostProcess.AmbushPP_MAT01", bkg], &[(bkg, "HitTime", 12.0), (bkg, "EffectLength", 0.3)]);
        assert!(in_merk_kill(&f));
        // Off unless the switch says native.
        assert!(effects_with(&f, false).is_empty());
        assert_eq!(effects_with(&f, true), [Effect { kind: Kind::MerkBkg, params: [12.0, 0.3, 1.0, 0.0] }]);
        // Defaults: no hit yet, the instance's own length and level.
        let e = effects_with(&feed(&[bkg], &[]), true);
        assert_eq!(e[0].params[1..3], [0.25, 1.0]);
        assert!(e[0].params[0] < -1.0e8);
        let fi = "fx_merkyouspacerescource.fadein_mat_mic";
        assert_eq!(effects_with(&feed(&[fi], &[(fi, "StartTime", 5.0)]), true)[0], Effect { kind: Kind::MerkFadeIn, params: [5.0, 0.25, 0.0, 0.0] });
        let bo = "FX_MerkYouSpaceRescource.BurnOut_MIC";
        assert_eq!(effects_with(&feed(&[bo], &[(bo, "StartTime", 7.0)]), true)[0].params[..2], [7.0, 1.5]);
        // The unported variant (PosterAndGhost) is a kill too, but has no pass: the host hands such frames to the game.
        let pg = feed(&["FX_MerkYouSpaceRescource.PosterAndGhost_MIC"], &[]);
        assert!(in_merk_kill(&pg) && effects_with(&pg, true).is_empty());
        assert!(!in_merk_kill(&feed(&["C1HUD.SniperScope_MIC"], &[])));
    }

    #[test]
    fn unknown_and_absent() {
        assert!(effects(&feed(&["GEN_PostProcess.AmbushPP_MAT01"], &[])).is_empty());
        let mut p = feed(&["C1HUD.SlowmoEffect_MatInst"], &[]);
        p.present = false;
        assert!(effects(&p).is_empty());
        assert_eq!(Kind::of("fx_gangsterfire.gangsterfire_mat02"), Some(Kind::Gangsta));
    }

    #[test]
    fn textures_listed() {
        assert!(all_textures().contains(&"FX_GangsterFire.EyeMask"));
        // Every effect's entry point exists in post.wgsl.
        let src = crate::shaders::post_source();
        for k in Kind::ALL {
            assert!(src.contains(&format!("fn {}(", k.fs())), "{}", k.fs());
        }
    }
}
