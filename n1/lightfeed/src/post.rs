//! Post-process settings from the feed's `post` object.
//!
//! Two sources: `post.chain` (the level's PostProcessChain effects: MaterialEffect, SfGodRaysEffect and
//! SfUberPostProcessEffect with BloomScale / BloomThreshold) and `post.current` (`LocalPlayer.CurrentPPInfo.LastSettings`,
//! the blended PostProcessSettings including Swordfish's ColourBalance_* and ToneMapping_* fields). Some fields in
//! `LastSettings` are uninitialised garbage (denormals, 1e29 durations), so every number goes through `sane`.

use serde_json::Value;

#[derive(Clone, Debug, PartialEq)]
pub struct ToneMap {
    pub enabled: bool,
    pub min_luminosity: f32,
    pub max_luminosity: f32,
    pub brightness_scale: f32,
}

#[derive(Clone, Debug)]
pub struct PostFeed {
    pub present: bool,
    pub bloom_enabled: bool,
    pub bloom_scale: f32,
    pub bloom_threshold: f32,
    /// ColourBalance multipliers, linear 0..1 per range (shadows, midtones, highlights). Identity is white.
    pub balance: Option<[[f32; 3]; 3]>,
    /// PostProcessSettings Scene_*: desaturation and shadows / midtones / highlights.
    pub desaturation: f32,
    pub scene_shadows: [f32; 3],
    pub scene_midtones: [f32; 3],
    pub scene_highlights: [f32; 3],
    pub tone: ToneMap,
    /// Class names of the chain's effects, in order.
    pub chain: Vec<String>,
    /// The chain's first shown MaterialEffect material (usually the level's colour grade).
    pub material_effect: Option<String>,
    /// B-227: every shown MaterialEffect material, in chain order. A chain can hold the level's grade and an inserted
    /// one (Crib: FireIntensity_PP's PostProcessIntensity_MIC); the grade is the first that decodes as one.
    pub material_effects: Vec<String>,
    /// B-266: the post materials' live scalar parameters, (material path, parameter name, value), binary feed only.
    pub material_params: Vec<(String, String, f32)>,
    pub god_rays: Option<GodRays>,
    /// W4: the uber effect's depth of field and bloom kernels (SfUberPostProcessEffect), `None` without one.
    pub uber: Option<Uber>,
    /// W4: the MaterialEffect's colour grade, when it is a GenericOutdoor_MAT01 instance. Filled by the renderer,
    /// which has the packages (`Grade::from_params`); the feed only names the material.
    pub grade: Option<Grade>,
}

/// SfUberPostProcessEffect's DOF + bloom settings (W4, decoded from the port's post draws).
#[derive(Clone, Debug, PartialEq)]
pub struct Uber {
    pub bloom_scale: f32,
    pub bloom_threshold: f32,
    pub bloom_kernel: f32,
    pub blur_kernel: f32,
    /// The second, bloom-kernel blur and its 15% mix into the composite.
    pub different_bloom_kernel: bool,
    pub falloff: f32,
    pub max_near: f32,
    pub max_far: f32,
    pub focus_radius: f32,
    pub focus_distance: f32,
}

/// GenericOutdoor_MAT01's grade (W4): a foreground / background split on scene depth, in display-linear colour.
/// Parameter names as in the material, values after the instance's overrides.
#[derive(Clone, Debug, PartialEq)]
pub struct Grade {
    pub fg_desaturate: f32,
    pub fg_contrast: f32,
    pub fg_tint: [f32; 3],
    pub bk_brightness: f32,
    pub bk_desaturate: f32,
    pub bk_contrast: f32,
    pub bk_tint: [f32; 3],
    pub blend: f32,
    pub blend_offset: f32,
    pub sky_clamp: f32,
}

impl Grade {
    /// From a material's named parameters (lowercase names); `None` unless the base is GenericOutdoor_MAT01.
    pub fn from_params(
        base: &str,
        scalars: &std::collections::HashMap<String, f32>,
        vectors: &std::collections::HashMap<String, [f32; 4]>,
    ) -> Option<Grade> {
        if !base.ends_with("GenericOutdoor_MAT01") {
            return None;
        }
        let s = |k: &str, d: f32| scalars.get(k).copied().unwrap_or(d);
        let v = |k: &str, d: [f32; 3]| vectors.get(k).map(|c| [c[0], c[1], c[2]]).unwrap_or(d);
        Some(Grade {
            fg_desaturate: s("foregrounddesaturate", 0.0),
            fg_contrast: s("foregroundcontrast", 0.0),
            fg_tint: v("foregroundtint", [0.0; 3]),
            bk_brightness: s("bkbrightness", 1.0),
            bk_desaturate: s("backgrounddesaturate", 0.0),
            bk_contrast: s("backgroundcontrast", 0.0),
            bk_tint: v("backgroundtint", [1.0; 3]),
            blend: s("fg-bgblend", 1e-4),
            blend_offset: s("fg-bgblendoffset", -0.05),
            sky_clamp: s("skyclampdistance", 2.0),
        })
    }

    /// True when the grade changes nothing (both zones pass the scene through).
    pub fn is_identity(&self) -> bool {
        let z = |x: f32| x.abs() < 1e-4;
        z(self.fg_desaturate)
            && z(self.fg_contrast)
            && self.fg_tint.iter().all(|&c| z(c))
            && z(self.bk_brightness - 1.0)
            && z(self.bk_desaturate)
            && z(self.bk_contrast)
            && self.bk_tint.iter().all(|&c| z(c - 1.0))
    }
}

#[derive(Clone, Debug)]
pub struct GodRays {
    pub exposure: f32,
    pub decay: f32,
    pub magnitude: f32,
    pub color: [f32; 3],
    /// Light point in texture space (x, y).
    pub screen: [f32; 2],
}

impl Default for PostFeed {
    fn default() -> Self {
        PostFeed {
            present: false,
            bloom_enabled: false,
            bloom_scale: 0.0,
            bloom_threshold: 1.0,
            balance: None,
            desaturation: 0.0,
            scene_shadows: [0.0; 3],
            scene_midtones: [1.0; 3],
            scene_highlights: [1.0; 3],
            tone: ToneMap { enabled: false, min_luminosity: 0.4, max_luminosity: 20.0, brightness_scale: 1.0 },
            chain: vec![],
            material_effect: None,
            material_effects: vec![],
            material_params: vec![],
            god_rays: None,
            uber: None,
            grade: None,
        }
    }
}

pub(crate) fn sane(x: Option<f64>, default: f32) -> f32 {
    match x {
        Some(v) if v.is_finite() && v.abs() < 1e5 && (v == 0.0 || v.abs() > 1e-20) => v as f32,
        _ => default,
    }
}

fn num(v: &Value, k: &str) -> Option<f64> {
    v.get(k).and_then(Value::as_f64)
}

fn arr3(v: &Value, k: &str, d: [f32; 3]) -> [f32; 3] {
    match v.get(k).and_then(Value::as_array) {
        Some(a) if a.len() >= 3 => [sane(a[0].as_f64(), d[0]), sane(a[1].as_f64(), d[1]), sane(a[2].as_f64(), d[2])],
        _ => d,
    }
}

fn color3(v: &Value, k: &str) -> [f32; 3] {
    match v.get(k).and_then(Value::as_array) {
        Some(a) if a.len() >= 3 => [0, 1, 2].map(|i| a[i].as_f64().unwrap_or(255.0) as f32 / 255.0),
        _ => [1.0; 3],
    }
}

impl PostFeed {
    /// B-266: a post material's live scalar parameter, when the feed carried it.
    pub fn param(&self, material: &str, name: &str) -> Option<f32> {
        self.material_params.iter().find(|p| p.0 == material && p.1.eq_ignore_ascii_case(name)).map(|p| p.2)
    }

    pub fn from_value(post: &Value) -> PostFeed {
        let mut p = PostFeed { present: true, ..Default::default() };
        if let Some(chain) = post.get("chain").and_then(Value::as_array) {
            for e in chain {
                let class = e.get("class").and_then(Value::as_str).unwrap_or("").to_string();
                let props = e.get("props").cloned().unwrap_or(Value::Null);
                match class.as_str() {
                    "SfUberPostProcessEffect" => {
                        p.bloom_scale = sane(num(&props, "BloomScale"), 0.0);
                        p.bloom_threshold = sane(num(&props, "BloomThreshold"), 1.0);
                        p.bloom_enabled = p.bloom_scale > 0.0;
                        let b = |k: &str| props.get(k).and_then(Value::as_bool).unwrap_or(false);
                        p.uber = Some(Uber {
                            bloom_scale: p.bloom_scale,
                            bloom_threshold: p.bloom_threshold,
                            bloom_kernel: sane(num(&props, "BloomKernelSize"), 16.0),
                            blur_kernel: sane(num(&props, "BlurKernelSize"), 16.0),
                            different_bloom_kernel: b("UseDifferentKernelForBloom"),
                            falloff: sane(num(&props, "FalloffExponent"), 2.0),
                            max_near: sane(num(&props, "MaxNearBlurAmount"), 1.0),
                            max_far: sane(num(&props, "MaxFarBlurAmount"), 1.0),
                            focus_radius: sane(num(&props, "FocusInnerRadius"), 2000.0).max(1.0),
                            focus_distance: sane(num(&props, "FocusDistance"), 0.0),
                        });
                    }
                    "MaterialEffect" => {
                        let shown = props.get("bShowInGame").and_then(Value::as_bool).unwrap_or(true);
                        if let (true, Some(m)) = (shown, props.get("Material").and_then(Value::as_str)) {
                            p.material_effects.push(m.to_string());
                            p.material_effect.get_or_insert_with(|| m.to_string());
                        }
                    }
                    "SfGodRaysEffect" => {
                        if props.get("Enabled").and_then(Value::as_bool).unwrap_or(false) {
                            p.god_rays = Some(GodRays {
                                exposure: sane(num(&props, "Exposure"), 0.0),
                                decay: sane(num(&props, "DecayPerSample"), 0.0),
                                magnitude: sane(num(&props, "RayMagnitude"), 0.0),
                                color: color3(&props, "ModulateRayColour"),
                                screen: [sane(num(&props, "vTextureSpaceLightPointX"), 0.0), sane(num(&props, "vTextureSpaceLightPointY"), 0.0)],
                            });
                        }
                    }
                    _ => {}
                }
                p.chain.push(class);
            }
        }
        if let Some(s) = post.get("current").and_then(|c| c.get("LastSettings")) {
            let on = |k: &str| s.get(k).and_then(Value::as_bool).unwrap_or(false);
            // The uber effect's own values win; LastSettings' bloom is the fallback when the chain has none.
            if !p.bloom_enabled && on("bEnableBloom") {
                p.bloom_scale = sane(num(s, "Bloom_Scale"), 0.0);
                p.bloom_threshold = sane(num(s, "Bloom_Threshold"), 1.0).max(0.0);
                p.bloom_enabled = p.bloom_scale > 0.0;
            }
            if on("bEnableColourBalanceEffect") {
                p.balance = Some([color3(s, "ColourBalance_Shadows"), color3(s, "ColourBalance_Midtones"), color3(s, "ColourBalance_Highlights")]);
            }
            if on("bEnableSceneEffect") {
                p.desaturation = sane(num(s, "Scene_Desaturation"), 0.0);
                p.scene_shadows = arr3(s, "Scene_Shadows", [0.0; 3]);
                p.scene_midtones = arr3(s, "Scene_MidTones", [1.0; 3]);
                p.scene_highlights = arr3(s, "Scene_HighLights", [1.0; 3]);
            }
            p.tone = ToneMap {
                enabled: on("bEnableToneMappingEffect"),
                min_luminosity: sane(num(s, "ToneMapping_MinLuminosity"), 0.4),
                max_luminosity: sane(num(s, "ToneMapping_MaxLuminosity"), 20.0),
                brightness_scale: sane(num(s, "ToneMapping_BrightnessScale"), 1.0),
            };
        }
        p
    }

    /// True when the feed's grade is the identity (white balance, default Scene_*): the visible look then comes from the
    /// chain's MaterialEffect and the tonemapper, not from these settings.
    pub fn grade_is_identity(&self) -> bool {
        let b = self.balance.is_none_or(|b| b.iter().all(|r| r.iter().all(|c| (c - 1.0).abs() < 1e-3)));
        b && self.desaturation.abs() < 1e-4
            && self.scene_shadows.iter().all(|c| c.abs() < 1e-4)
            && self.scene_midtones.iter().all(|c| (c - 1.0).abs() < 1e-4)
            && self.scene_highlights.iter().all(|c| (c - 1.0).abs() < 1e-4)
    }
}
