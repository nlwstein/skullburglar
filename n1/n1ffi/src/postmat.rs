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
}

impl Kind {
    /// The material paths (outermost instance, or the parent material) that select this effect.
    pub const fn paths(self) -> &'static [&'static str] {
        match self {
            Kind::Gangsta => &["C1HUD.SlowmoEffect_MatInst", "FX_GangsterFire.GangsterFire_MAT02"],
            Kind::Death => &["UI_DeathEffect.FaderDeath_MIC", "UI_DeathEffect.FaderDeath_MAT"],
        }
    }

    /// The fragment entry point in `post.wgsl`.
    pub const fn fs(self) -> &'static str {
        match self {
            Kind::Gangsta => "fs_gangsta",
            Kind::Death => "fs_death",
        }
    }

    /// The material's textures, bound as `tex0` and `tex1` ("" = none).
    pub const fn textures(self) -> [&'static str; 2] {
        match self {
            Kind::Gangsta => ["FX_GangsterFire.streak", "FX_GangsterFire.EyeMask"],
            Kind::Death => ["UI_DeathEffect.DeathScreenBackground_Alphas", "UI_DeathEffect.HeatVisionClouds_Alpha"],
        }
    }

    pub const ALL: [Kind; 2] = [Kind::Gangsta, Kind::Death];

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

/// The active ported effects of a post feed, in chain order. A material whose strength is zero (or not positive) is
/// left out, so the pass isn't recorded at all.
pub fn effects(p: &PostFeed) -> Vec<Effect> {
    let mut out = Vec::new();
    if !p.present {
        return out;
    }
    for m in &p.material_effects {
        let Some(kind) = Kind::of(m) else { continue };
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
