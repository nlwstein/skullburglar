//! Material expression graph (the extractor's JSON) -> a WGSL `fn material(i: MatIn) -> vec4<f32>` body.
//! rgb = EmissiveColor, a = Opacity (or the masked-out opacity for BLEND_Masked). Plain string codegen, so the
//! in-port renderer compiles the same text with naga. Unsupported expression classes become a neutral constant and
//! are listed in `unknown` (so a census shows what a real material compiler still has to cover).
//!
//! The fragment inputs the generated code may read (`MatIn`, declared by the renderer's shader template):
//! `uv` (sprite cell UV), `uv2` (second sub-UV cell), `interp`, `vcol` (particle colour), `pixel_w` (view depth of the
//! pixel), `scene_w` (view depth of the opaque scene behind it), `time`, `screen` (0..1), `ndv` (dot of the vertex
//! normal and the direction to the camera; 1 for camera-facing sprites).

use serde_json::Value;
use std::collections::{BTreeMap, HashMap};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Blend {
    Opaque,
    Masked,
    Translucent,
    Additive,
    Modulate,
}

pub struct Compiled {
    pub blend: Blend,
    pub two_sided: bool,
    pub clip: f32,
    /// Texture object paths in slot order (at most 4).
    pub textures: Vec<String>,
    pub wgsl: String,
    /// Reads scene depth (DepthBiasedAlpha): needs the depth texture.
    pub soft: bool,
    pub unknown: Vec<String>,
}

pub fn blend_from(name: &str) -> Blend {
    match name {
        "BLEND_Masked" => Blend::Masked,
        "BLEND_Translucent" => Blend::Translucent,
        "BLEND_Additive" => Blend::Additive,
        "BLEND_Modulate" => Blend::Modulate,
        _ => Blend::Opaque,
    }
}

/// A texture's unpack range (UE3 `UnpackMin`/`UnpackMax`, e.g. -1..1 for normal maps): the sampled value becomes
/// `tex * (max - min) + min`. `None`: the texture is used as it is.
pub type Unpack = Option<([f32; 3], [f32; 3])>;

/// What the compiler needs to know about a texture beyond its path.
#[derive(Clone, Copy, Debug, Default)]
pub struct TexInfo {
    pub unpack: Unpack,
    /// The texture is sampled through an sRGB view but the original fetches it with Xenos's gamma sign, whose
    /// piecewise-linear curve reads dark texels far brighter than sRGB (0.1 -> 0.025 vs 0.010): re-decode the colour
    /// with that curve (`xe_pwl` in the particle shader prelude). B-229: the smoke textures are dark and multiplied up
    /// (AcridSmokeSimple: Smoke_Subuv x 6), so sRGB left the near-camera smoke at about half the port's brightness.
    pub pwl: bool,
    /// With `pwl`: the texture is uploaded raw (UNORM view), so the sample already holds the stored value and the curve
    /// applies directly (`xe_pwl_raw`), as the port's fetch does it: no sRGB decode in the sampler and no `pow`
    /// re-encode in the shader, and the filtering happens on the stored values, as in the port (G6: the re-encode cost
    /// a low-end laptop about 1 ms of GPU per frame in 71515's explosions).
    pub raw: bool,
}

struct Cg<'a> {
    ex: &'a Value,
    tex_info: &'a dyn Fn(&str) -> TexInfo,
    /// Material instance parameter values by lowercase name: {"scalar":{}, "vector":{}, "texture":{}}.
    ov: &'a Value,
    out: String,
    memo: HashMap<i64, (String, u8)>,
    textures: Vec<String>,
    soft: bool,
    unknown: Vec<String>,
    n: usize,
}

/// B-273 nodes (Abs, Divide, CameraVector) are compiled unless `BOTS_N1_PARTICLE_NODES=0`.
fn nodes_on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("BOTS_N1_PARTICLE_NODES").map_or(true, |v| v != "0"))
}

fn flt(x: f64) -> String {
    let s = format!("{:?}", x as f32);
    if s.contains("inf") || s.contains("NaN") { "0.0".into() } else { s }
}

impl<'a> Cg<'a> {
    /// The instance's value for a parameter expression `e` (`kind`: scalar, vector or texture), if it sets one.
    fn param(&self, e: &Value, kind: &str) -> Option<&'a Value> {
        let name = e["props"]["ParameterName"].as_str()?.to_lowercase();
        self.ov[kind].get(name.as_str())
    }
    fn prop_f(&self, e: &Value, k: &str, d: f64) -> f64 {
        e["props"][k].as_f64().unwrap_or(d)
    }
    fn let_(&mut self, expr: String) -> String {
        let id = format!("v{}", self.n);
        self.n += 1;
        self.out.push_str(&format!("    let {id}: vec4<f32> = {expr};\n"));
        id
    }
    /// `sample` (a vec4 expression read from texture `tex`) with the texture's unpack range applied, as UE3's
    /// TextureSample does: FakeLit_Bumpy_NORM, the normal map of the fake-lit smoke (AcridSmokeLit_SubUV and others),
    /// is stored 0..1 and unpacked to -1..1. Without it the fake light term was a flat ~1.9 instead of 0.1..3.4 (B-229).
    fn unpacked(&self, tex: &str, sample: String) -> String {
        let info = (self.tex_info)(tex);
        let sample = match (info.pwl, info.raw) {
            (true, true) => format!("xe_pwl_raw({sample})"),
            (true, false) => format!("xe_pwl({sample})"),
            _ => sample,
        };
        match info.unpack {
            Some((mn, mx)) => format!(
                "({sample} * vec4<f32>({}, {}, {}, 1.0) + vec4<f32>({}, {}, {}, 0.0))",
                flt((mx[0] - mn[0]) as f64), flt((mx[1] - mn[1]) as f64), flt((mx[2] - mn[2]) as f64),
                flt(mn[0] as f64), flt(mn[1] as f64), flt(mn[2] as f64)
            ),
            None => sample,
        }
    }
    fn slot(&mut self, tex: &str) -> usize {
        if let Some(i) = self.textures.iter().position(|t| t == tex) {
            return i;
        }
        if self.textures.len() < 4 {
            self.textures.push(tex.to_string());
            self.textures.len() - 1
        } else {
            3
        }
    }
    /// An input struct ({Expression:{obj}, Mask...}) -> (register, width), or None if unconnected.
    fn input(&mut self, e: &Value, k: &str) -> Option<(String, u8)> {
        let inp = &e["props"][k];
        let obj = inp["Expression"]["obj"].as_i64()?;
        let (reg, w) = self.expr(obj);
        if inp["Mask"].as_i64().unwrap_or(0) == 1 {
            let sel: Vec<usize> = ["MaskR", "MaskG", "MaskB", "MaskA"].iter().enumerate().filter(|(_, m)| inp[**m].as_i64().unwrap_or(0) == 1).map(|(i, _)| i).collect();
            return Some(self.swizzle(&reg, &sel));
        }
        Some((reg, w))
    }
    fn swizzle(&mut self, reg: &str, sel: &[usize]) -> (String, u8) {
        let c = |i: usize| ["x", "y", "z", "w"][i];
        match sel.len() {
            0 => (reg.to_string(), 4),
            1 => {
                let r = self.let_(format!("vec4<f32>({reg}.{})", c(sel[0])));
                (r, 1)
            }
            n => {
                let mut parts: Vec<String> = sel.iter().map(|&i| format!("{reg}.{}", c(i))).collect();
                while parts.len() < 4 {
                    parts.push("0.0".into());
                }
                let r = self.let_(format!("vec4<f32>({})", parts.join(", ")));
                (r, n as u8)
            }
        }
    }
    fn in_or(&mut self, e: &Value, k: &str, constk: &str, d: f64) -> (String, u8) {
        if let Some(r) = self.input(e, k) {
            return r;
        }
        let v = self.prop_f(e, constk, d);
        (self.let_(format!("vec4<f32>({})", flt(v))), 1)
    }
    fn expr(&mut self, obj: i64) -> (String, u8) {
        if let Some(r) = self.memo.get(&obj) {
            return r.clone();
        }
        let ex = self.ex;
        let Some(e) = ex.get(obj.to_string()) else {
            let r = (self.let_("vec4<f32>(0.0)".into()), 1);
            return r;
        };
        let class = e["class"].as_str().unwrap_or("").trim_start_matches("MaterialExpression").to_string();
        let uv_default = "i.uv".to_string();
        let r: (String, u8) = match class.as_str() {
            "Constant" => (self.let_(format!("vec4<f32>({})", flt(self.prop_f(e, "R", 0.0)))), 1),
            "Constant2Vector" => (self.let_(format!("vec4<f32>({}, {}, 0.0, 0.0)", flt(self.prop_f(e, "R", 0.0)), flt(self.prop_f(e, "G", 0.0)))), 2),
            "Constant3Vector" => (self.let_(format!("vec4<f32>({}, {}, {}, 0.0)", flt(self.prop_f(e, "R", 0.0)), flt(self.prop_f(e, "G", 0.0)), flt(self.prop_f(e, "B", 0.0)))), 3),
            "Constant4Vector" => (self.let_(format!("vec4<f32>({}, {}, {}, {})", flt(self.prop_f(e, "R", 0.0)), flt(self.prop_f(e, "G", 0.0)), flt(self.prop_f(e, "B", 0.0)), flt(self.prop_f(e, "A", 0.0)))), 4),
            "VectorParameter" => {
                let d = self.param(e, "vector").unwrap_or(&e["props"]["DefaultValue"]).as_array().map(|a| a.iter().map(|x| x.as_f64().unwrap_or(0.0)).collect::<Vec<_>>()).unwrap_or(vec![0.0; 4]);
                let g = |i: usize| flt(d.get(i).copied().unwrap_or(0.0));
                (self.let_(format!("vec4<f32>({}, {}, {}, {})", g(0), g(1), g(2), g(3))), 4)
            }
            "ScalarParameter" => {
                let v = self.param(e, "scalar").and_then(Value::as_f64).unwrap_or_else(|| self.prop_f(e, "DefaultValue", 0.0));
                (self.let_(format!("vec4<f32>({})", flt(v))), 1)
            }
            "Multiply" | "Add" | "Subtract" => {
                let (a, wa) = self.in_or(e, "A", "ConstA", 0.0);
                let (b, wb) = self.in_or(e, "B", "ConstB", if class == "Multiply" { 1.0 } else { 0.0 });
                let op = match class.as_str() { "Multiply" => "*", "Add" => "+", _ => "-" };
                (self.let_(format!("{a} {op} {b}")), wa.max(wb))
            }
            "MultiplyConstant" => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(1.0)".into()), 1));
                let m = self.prop_f(e, "MultiplierR", 1.0);
                (self.let_(format!("{a} * {}", flt(m))), w)
            }
            "MultiplyConstant3" => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(1.0)".into()), 3));
                let (r, g, b) = (self.prop_f(e, "MultiplierR", 1.0), self.prop_f(e, "MultiplierG", 1.0), self.prop_f(e, "MultiplierB", 1.0));
                (self.let_(format!("{a} * vec4<f32>({}, {}, {}, 1.0)", flt(r), flt(g), flt(b))), w.max(3))
            }
            "OneMinus" => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                (self.let_(format!("vec4<f32>(1.0) - {a}")), w)
            }
            "Clamp" | "Clamp2" => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                // Clamp2's constant bounds are ClampMin / ClampMax (as ue3 matir reads them); MinDefault / MaxDefault kept as fallbacks (B-229).
                let (dlo, dhi) = (self.prop_f(e, "MinDefault", 0.0), self.prop_f(e, "MaxDefault", 1.0));
                let (dlo, dhi) = (self.prop_f(e, "ClampMin", dlo), self.prop_f(e, "ClampMax", dhi));
                let lo = match self.input(e, "Min") { Some((r, _)) => r, None => self.let_(format!("vec4<f32>({})", flt(dlo))) };
                let hi = match self.input(e, "Max") { Some((r, _)) => r, None => self.let_(format!("vec4<f32>({})", flt(dhi))) };
                (self.let_(format!("clamp({a}, {lo}, {hi})")), w)
            }
            // ConstantClamp: Input clamped to the constant Min (default 0) .. Max (default 1). Before this it fell
            // through to the unknown-expression arm (constant 1.0), which turned the blood-spray mask into a solid
            // Modulate quad. BOTS_N1_PARTICLE_CONSTCLAMP=0 restores the old fallback for A/B.
            "ConstantClamp" if std::env::var("BOTS_N1_PARTICLE_CONSTCLAMP").map_or(true, |v| v != "0") => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                let (lo, hi) = (self.prop_f(e, "Min", 0.0), self.prop_f(e, "Max", 1.0));
                (self.let_(format!("clamp({a}, vec4<f32>({}), vec4<f32>({}))", flt(lo), flt(hi))), w)
            }
            // Abs, Divide and CameraVector (B-273): these hit the unknown-expression arm (constant 1.0) too. Abs: the lens
            // flare and sun masks; Divide: PixelDepth / constant fades (lens flares, dust swirl, the Driving02 credits);
            // CameraVector: SplashDrops. BOTS_N1_PARTICLE_NODES=0 restores the old fallback for A/B.
            "Abs" if nodes_on() => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                (self.let_(format!("abs({a})")), w)
            }
            "Divide" if nodes_on() => {
                let (a, wa) = self.in_or(e, "A", "ConstA", 0.0);
                let (b, wb) = self.in_or(e, "B", "ConstB", 1.0);
                // A zero divisor gives a tiny one (UE3's HLSL gives inf; a sprite pixel must not turn NaN).
                (self.let_(format!("{a} / select({b}, vec4<f32>(1.0e-6), abs({b}) < vec4<f32>(1.0e-6))")), wa.max(wb))
            }
            // The unit vector from the pixel to the camera, UE3 world axes (the renderer's Y-up space swaps y and z).
            // Sprites face the camera, so the view axis stands in for the per-pixel direction (as `ndv` does for Fresnel).
            "CameraVector" if nodes_on() => (self.let_("vec4<f32>(-u.fwd.x, -u.fwd.z, -u.fwd.y, 0.0)".into()), 3),
            // DestDepth: the view depth of the opaque scene behind the pixel (the lens-flare reflections fade by it).
            "DestDepth" if nodes_on() => {
                self.soft = true;
                (self.let_("vec4<f32>(i.scene_w)".into()), 1)
            }
            "Floor" | "Frac" => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                (self.let_(format!("{}({a})", if class == "Floor" { "floor" } else { "fract" })), w)
            }
            "Sine" | "Cosine" => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                let per = self.prop_f(e, "Period", 1.0).max(1e-6);
                (self.let_(format!("{}({a} * {})", if class == "Sine" { "sin" } else { "cos" }, flt(std::f64::consts::TAU / per))), w)
            }
            "Power" => {
                let (a, w) = self.input(e, "Base").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                let (x, _) = self.in_or(e, "Exponent", "ConstExponent", 2.0);
                (self.let_(format!("pow(max({a}, vec4<f32>(0.0)), {x})")), w)
            }
            "Normalize" => {
                let (a, _) = self.input(e, "VectorInput").unwrap_or_else(|| (self.let_("vec4<f32>(0.0, 0.0, 1.0, 0.0)".into()), 3));
                (self.let_(format!("vec4<f32>(normalize({a}.xyz), 0.0)")), 3)
            }
            "DotProduct" => {
                let (a, _) = self.input(e, "A").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 3));
                let (b, _) = self.input(e, "B").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 3));
                (self.let_(format!("vec4<f32>(dot({a}.xyz, {b}.xyz))")), 1)
            }
            "Desaturation" => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 3));
                let (p, _) = self.in_or(e, "Percent", "Percent", 1.0);
                (self.let_(format!("vec4<f32>(mix({a}.xyz, vec3<f32>(dot({a}.xyz, vec3<f32>(0.3, 0.59, 0.11))), {p}.x), {a}.w)")), w)
            }
            "LinearInterpolate" => {
                let (a, wa) = self.in_or(e, "A", "ConstA", 0.0);
                let (b, wb) = self.in_or(e, "B", "ConstB", 1.0);
                let (t, _) = self.in_or(e, "Alpha", "ConstAlpha", 0.5);
                (self.let_(format!("mix({a}, {b}, {t})")), wa.max(wb))
            }
            "AppendVector" => {
                let (a, wa) = self.input(e, "A").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                let (b, wb) = self.input(e, "B").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                let c = |r: &str, i: usize| format!("{r}.{}", ["x", "y", "z", "w"][i]);
                let mut parts: Vec<String> = (0..wa as usize).map(|i| c(&a, i)).chain((0..wb as usize).map(|i| c(&b, i))).collect();
                parts.truncate(4);
                let w = parts.len() as u8;
                while parts.len() < 4 { parts.push("0.0".into()); }
                (self.let_(format!("vec4<f32>({})", parts.join(", "))), w)
            }
            "ComponentMask" => {
                let (a, _) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                let sel: Vec<usize> = ["R", "G", "B", "A"].iter().enumerate().filter(|(_, k)| e["props"][**k].as_bool().unwrap_or(false)).map(|(i, _)| i).collect();
                self.swizzle(&a, &sel)
            }
            "TimesHalfPlusHalf" => {
                let (a, w) = self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0)".into()), 1));
                (self.let_(format!("{a} * 0.5 + vec4<f32>(0.5)")), w)
            }
            "Time" => (self.let_("vec4<f32>(u.time)".into()), 1),
            "PixelDepth" => (self.let_("vec4<f32>(i.pixel_w)".into()), 1),
            // xy: the 0..1 screen position (as before). zw: UE3's clip-space z and w. Its projection makes clip
            // z = view depth - near and w = view depth; bScreenAlign divides by w. Smoke_UnlitBumpOffset_MAT's
            // "Screen Clipping" term (z * 0.01, clamped to 1) fades smoke within ~100 units of the camera; with z = 0
            // the whole material was invisible.
            "ScreenPosition" => {
                let zw = if e["props"]["bScreenAlign"].as_bool().unwrap_or(false) {
                    "1.0 - u.near / max(i.pixel_w, 1.0e-4), 1.0"
                } else {
                    "i.pixel_w - u.near, i.pixel_w"
                };
                (self.let_(format!("vec4<f32>(i.screen.x, i.screen.y, {zw})")), 4)
            }
            "VertexColor" | "MeshEmitterVertexColor" => (self.let_("i.vcol".into()), 4),
            "TextureCoordinate" | "TextureCoordinate2" => {
                let (tu, tv) = (self.prop_f(e, "UTiling", self.prop_f(e, "TilingU", 1.0)), self.prop_f(e, "VTiling", self.prop_f(e, "TilingV", 1.0)));
                (self.let_(format!("vec4<f32>(i.uv.x * {}, i.uv.y * {}, 0.0, 0.0)", flt(tu), flt(tv))), 2)
            }
            "Panner" => {
                let (c, _) = self.input(e, "Coordinate").unwrap_or_else(|| (self.let_(format!("vec4<f32>({uv_default}, 0.0, 0.0)")), 2));
                let (sx, sy) = (self.prop_f(e, "SpeedX", 0.0), self.prop_f(e, "SpeedY", 0.0));
                (self.let_(format!("{c} + vec4<f32>(u.time * {}, u.time * {}, 0.0, 0.0)", flt(sx), flt(sy))), 2)
            }
            "Rotator" => {
                let (c, _) = self.input(e, "Coordinate").unwrap_or_else(|| (self.let_(format!("vec4<f32>({uv_default}, 0.0, 0.0)")), 2));
                let (cx, cy, sp) = (self.prop_f(e, "CenterX", 0.5), self.prop_f(e, "CenterY", 0.5), self.prop_f(e, "Speed", 0.25));
                let a = format!("(u.time * {} * 6.2831853)", flt(sp));
                (self.let_(format!("vec4<f32>(vec2<f32>({cx}, {cy}) + mat2x2<f32>(cos{a}, sin{a}, -sin{a}, cos{a}) * ({c}.xy - vec2<f32>({cx}, {cy})), 0.0, 0.0)", cx = flt(cx), cy = flt(cy), a = a)), 2)
            }
            "BumpOffset" => match self.input(e, "Coordinate") {
                Some(c) => c,
                None => (self.let_(format!("vec4<f32>({uv_default}, 0.0, 0.0)")), 2),
            },
            "TextureSample" | "TextureSampleParameter2D" => {
                let tex = self.param(e, "texture").and_then(Value::as_str).unwrap_or_else(|| e["props"]["Texture"]["path"].as_str().unwrap_or("")).to_string();
                let s = self.slot(&tex);
                let (c, _) = self.input(e, "Coordinates").unwrap_or_else(|| (self.let_(format!("vec4<f32>({uv_default}, 0.0, 0.0)")), 2));
                let e = self.unpacked(&tex, format!("textureSample(tex{s}, samp, {c}.xy)"));
                (self.let_(e), 4)
            }
            "ParticleSubUV" | "MeshSubUV" | "TextureSampleParameterSubUV" | "TextureSampleParameterMeshSubUV" => {
                let tex = self.param(e, "texture").and_then(Value::as_str).unwrap_or_else(|| e["props"]["Texture"]["path"].as_str().unwrap_or("")).to_string();
                let s = self.slot(&tex);
                // B-236: decode each sub-image fetch (PWL, unpack) before the blend, as the port does: the hardware
                // decodes per fetch and the shader mixes the results. Mixing first and decoding after bent the blend.
                let e = if class.contains("MeshSubUV") {
                    self.unpacked(&tex, format!("textureSample(tex{s}, samp, i.uv)"))
                } else {
                    let a = self.unpacked(&tex, format!("textureSample(tex{s}, samp, i.uv)"));
                    let b = self.unpacked(&tex, format!("textureSample(tex{s}, samp, i.uv2)"));
                    format!("mix({a}, {b}, i.interp)")
                };
                (self.let_(e), 4)
            }
            "DepthBiasedAlpha" => {
                self.soft = true;
                let (a, w) = self.input(e, "Alpha").unwrap_or_else(|| (self.let_("vec4<f32>(1.0)".into()), 1));
                let (b, _) = self.in_or(e, "Bias", "Bias", 1.0);
                let sc = self.prop_f(e, "BiasScale", 1.0);
                // UE3 MaterialTemplate: DepthBias = (1 - Bias) * BiasScale; alpha *= saturate((SceneDepth - PixelDepth) /
                // max(DepthBias, 0.001)). Bias 0.2 / scale 1000 (the explosion glows) fades over 800 units, not 200.
                (self.let_(format!("{a} * clamp((i.scene_w - i.pixel_w) / max((1.0 - {b}.x) * {}, 0.001), 0.0, 1.0)", flt(sc))), w)
            }
            // Approximations for the few view-dependent nodes: a camera-facing sprite has N = V.
            // UE3: pow(1 - max(dot(N, CameraVector), 0), Exponent), with the vertex normal (a Normal input, a
            // tangent-space normal map, is not applied). A stub of 0 here turned the flicker sphere of
            // FX_Fire.LightFlicker_MAT (Modulate, emissive = F + 2048 (1 - F) flicker) into a solid yellow dome.
            "Fresnel" => {
                let exp = self.prop_f(e, "Exponent", 3.0);
                (self.let_(format!("vec4<f32>(pow(max(1.0 - max(i.ndv, 0.0), 0.0), {}))", flt(exp))), 1)
            }
            "ReflectionVector" => (self.let_("vec4<f32>(0.0, 0.0, 1.0, 0.0)".into()), 3),
            "Transform" => self.input(e, "Input").unwrap_or_else(|| (self.let_("vec4<f32>(0.0, 0.0, 1.0, 0.0)".into()), 3)),
            other => {
                self.unknown.push(other.to_string());
                (self.let_("vec4<f32>(1.0)".into()), 1)
            }
        };
        self.memo.insert(obj, r.clone());
        r
    }
}

/// Compile one material (the extractor's JSON for a `Material`; `parent` is its parent for instances), with every
/// texture used as it is (no unpack ranges known).
pub fn compile(mat: &Value) -> Compiled {
    compile_with(mat, &|_| TexInfo::default())
}

/// As `compile`; `tex_info(texture path)` gives each texture's unpack range and decode.
pub fn compile_with(mat: &Value, tex_info: &dyn Fn(&str) -> TexInfo) -> Compiled {
    let props = &mat["props"];
    let blend = blend_from(props["BlendMode"].as_str().unwrap_or(""));
    let two_sided = props["TwoSided"].as_bool().unwrap_or(false);
    let clip = props["OpacityMaskClipValue"].as_f64().unwrap_or(0.333333) as f32;
    let empty = Value::Object(Default::default());
    let ex = mat.get("expressions").unwrap_or(&empty);
    let ov = mat.get("overrides").unwrap_or(&empty);
    let mut cg = Cg { ex, tex_info, ov, out: String::new(), memo: HashMap::new(), textures: Vec::new(), soft: false, unknown: Vec::new(), n: 0 };
    let root = |cg: &mut Cg, k: &str| -> Option<(String, u8)> {
        let inp = &props[k];
        let obj = inp["Expression"]["obj"].as_i64()?;
        let (reg, w) = cg.expr(obj);
        if inp["Mask"].as_i64().unwrap_or(0) == 1 {
            let sel: Vec<usize> = ["MaskR", "MaskG", "MaskB", "MaskA"].iter().enumerate().filter(|(_, m)| inp[**m].as_i64().unwrap_or(0) == 1).map(|(i, _)| i).collect();
            return Some(cg.swizzle(&reg, &sel));
        }
        Some((reg, w))
    };
    let emissive = root(&mut cg, "EmissiveColor").map(|x| x.0);
    let opacity = root(&mut cg, if blend == Blend::Masked { "OpacityMask" } else { "Opacity" }).map(|x| x.0);
    // Lit materials (smoke, dust) get most of their colour from DiffuseColor; with no per-particle lighting yet,
    // light it with a flat ambient-plus-sun estimate so it is not left black (UE3 lights it from the light env).
    let unlit = props["LightingModel"].as_str().is_some_and(|m| m.contains("Unlit"));
    let diffuse = if unlit { None } else { root(&mut cg, "DiffuseColor").map(|x| x.0) };
    // B-250: `lit_irr` is the irradiance on the particle (the flat estimate for sprites; a mesh particle gets the sky
    // hemisphere, the sun and the dynamic lights, see shader.rs `LIT_MESH`) and `lit_spec` the Phong highlight.
    let spec = if unlit || diffuse.is_none() {
        None
    } else {
        match (root(&mut cg, "SpecularColor"), root(&mut cg, "SpecularPower")) {
            (Some((c, _)), p) => Some(format!("{c}.xyz * lit_spec(i, {})", p.map(|(p, _)| format!("{p}.x")).unwrap_or_else(|| "15.0".into()))),
            _ => None,
        }
    };
    let rgb = match (emissive, diffuse) {
        (Some(e), Some(d)) => format!("({e}.xyz + {d}.xyz * lit_irr(i))"),
        (None, Some(d)) => format!("({d}.xyz * lit_irr(i))"),
        (Some(e), None) => format!("{e}.xyz"),
        (None, None) => "vec3<f32>(0.0)".into(),
    };
    let rgb = match spec {
        Some(sp) => format!("({rgb} + {sp})"),
        None => rgb,
    };
    let a = opacity.map(|r| format!("{r}.x")).unwrap_or_else(|| "1.0".into());
    let mut wgsl = String::from("fn material(i: MatIn) -> vec4<f32> {\n");
    wgsl.push_str(&cg.out);
    wgsl.push_str(&format!("    return vec4<f32>({rgb}, {a});\n}}\n"));
    Compiled { blend, two_sided, clip, textures: cg.textures, wgsl, soft: cg.soft, unknown: cg.unknown }
}

/// Compile every material in the assets (parents are followed for instances).
pub fn compile_all(materials: &BTreeMap<String, Value>) -> BTreeMap<String, Compiled> {
    let mut out = BTreeMap::new();
    for (k, m) in materials {
        let mut m = m;
        let mut depth = 0;
        while m["class"] == "MaterialInstanceConstant" && depth < 4 {
            let Some(p) = m["parent"].as_str().and_then(|p| materials.get(&p.to_lowercase())) else { break };
            m = p;
            depth += 1;
        }
        out.insert(k.clone(), compile(m));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn texture_unpack_range() {
        // B-229: a TextureSample of a normal map stored 0..1 with UnpackMin -1, UnpackMax 1 reads as -1..1.
        let mat = serde_json::json!({
            "props": {"BlendMode": "BLEND_Translucent", "EmissiveColor": {"Expression": {"obj": 1}}},
            "expressions": {"1": {"class": "MaterialExpressionTextureSample", "props": {"Texture": {"path": "Pkg.Bumpy_NORM"}}}}
        });
        let plain = compile(&mat);
        assert!(!plain.wgsl.contains("vec4<f32>(2.0, 2.0, 2.0, 1.0)"), "{}", plain.wgsl);
        let c = compile_with(&mat, &|t| TexInfo { unpack: (t == "Pkg.Bumpy_NORM").then_some(([-1.0; 3], [1.0; 3])), pwl: false, raw: false });
        assert!(c.wgsl.contains("* vec4<f32>(2.0, 2.0, 2.0, 1.0) + vec4<f32>(-1.0, -1.0, -1.0, 0.0))"), "{}", c.wgsl);
        assert_eq!(c.textures, vec!["Pkg.Bumpy_NORM".to_string()]);
        let g = compile_with(&mat, &|_| TexInfo { unpack: None, pwl: true, raw: false });
        assert!(g.wgsl.contains("xe_pwl(textureSample(tex0"), "{}", g.wgsl);
    }

    #[test]
    fn subuv_decodes_each_fetch() {
        // B-236: both sub-image fetches go through the PWL decode before the interp blend.
        let mat = serde_json::json!({
            "props": {"BlendMode": "BLEND_Translucent", "EmissiveColor": {"Expression": {"obj": 1}}},
            "expressions": {"1": {"class": "MaterialExpressionParticleSubUV", "props": {"Texture": {"path": "Pkg.Fire"}}}}
        });
        let g = compile_with(&mat, &|_| TexInfo { unpack: None, pwl: true, raw: false });
        assert!(
            g.wgsl.contains("mix(xe_pwl(textureSample(tex0, samp, i.uv)), xe_pwl(textureSample(tex0, samp, i.uv2)), i.interp)"),
            "{}",
            g.wgsl
        );
        let r = compile_with(&mat, &|_| TexInfo { unpack: None, pwl: true, raw: true });
        assert!(
            r.wgsl.contains("mix(xe_pwl_raw(textureSample(tex0, samp, i.uv)), xe_pwl_raw(textureSample(tex0, samp, i.uv2)), i.interp)"),
            "{}",
            r.wgsl
        );
    }

    #[test]
    fn clamp2_bounds() {
        // B-229: Clamp2 keeps its constant bounds in ClampMin / ClampMax.
        let mat = serde_json::json!({
            "props": {"BlendMode": "BLEND_Translucent", "Opacity": {"Expression": {"obj": 1}}},
            "expressions": {
                "1": {"class": "MaterialExpressionClamp2", "props": {"Input": {"Expression": {"obj": 2}}, "ClampMin": 0.1, "ClampMax": 0.3}},
                "2": {"class": "MaterialExpressionVertexColor", "props": {}}
            }
        });
        let c = compile(&mat);
        assert!(c.wgsl.contains("vec4<f32>(0.1)") && c.wgsl.contains("vec4<f32>(0.3)"), "{}", c.wgsl);
    }

    fn one(class: &str, props: serde_json::Value) -> Compiled {
        let mat = serde_json::json!({
            "props": {"BlendMode": "BLEND_Additive", "EmissiveColor": {"Expression": {"obj": 1}}},
            "expressions": {
                "1": {"class": class, "props": props},
                "2": {"class": "MaterialExpressionVertexColor", "props": {}},
                "3": {"class": "MaterialExpressionPixelDepth", "props": {}},
                "4": {"class": "MaterialExpressionConstant", "props": {"R": 400.0}}
            }
        });
        compile(&mat)
    }

    #[test]
    fn abs_is_supported() {
        let c = one("MaterialExpressionAbs", serde_json::json!({"Input": {"Expression": {"obj": 2}}}));
        assert!(c.unknown.is_empty(), "{:?}", c.unknown);
        assert!(c.wgsl.contains("= abs(v0)") && c.wgsl.contains("= i.vcol"), "{}", c.wgsl);
    }

    #[test]
    fn divide_is_supported() {
        // The lens flare fade: PixelDepth / 400, with a guard against a zero divisor.
        let c = one("MaterialExpressionDivide", serde_json::json!({"A": {"Expression": {"obj": 3}, "Mask": 1, "MaskR": 1}, "B": {"Expression": {"obj": 4}}}));
        assert!(c.unknown.is_empty(), "{:?}", c.unknown);
        assert!(c.wgsl.contains(" / select(") && c.wgsl.contains("vec4<f32>(400.0)") && c.wgsl.contains("i.pixel_w"), "{}", c.wgsl);
        // An unconnected B divides by ConstB (default 1.0).
        let d = one("MaterialExpressionDivide", serde_json::json!({"A": {"Expression": {"obj": 2}}, "ConstB": 2.0}));
        assert!(d.unknown.is_empty() && d.wgsl.contains("vec4<f32>(2.0)"), "{}", d.wgsl);
    }

    #[test]
    fn dest_depth_reads_scene_depth() {
        let c = one("MaterialExpressionDestDepth", serde_json::json!({}));
        assert!(c.unknown.is_empty() && c.soft && c.wgsl.contains("i.scene_w"), "{}", c.wgsl);
    }

    #[test]
    fn camera_vector_is_supported() {
        let c = one("MaterialExpressionCameraVector", serde_json::json!({}));
        assert!(c.unknown.is_empty(), "{:?}", c.unknown);
        assert!(c.wgsl.contains("-u.fwd.x"), "{}", c.wgsl);
    }

    #[test]
    fn constant_clamp_is_supported() {
        let mat = serde_json::json!({
            "props": {"BlendMode": "BLEND_Translucent", "Opacity": {"Expression": {"obj": 1}}},
            "expressions": {
                "1": {"class": "MaterialExpressionConstantClamp", "props": {"Input": {"Expression": {"obj": 2}}, "Min": 0.2, "Max": 0.9}},
                "2": {"class": "MaterialExpressionVertexColor", "props": {}}
            }
        });
        let c = compile(&mat);
        assert!(c.unknown.is_empty(), "{:?}", c.unknown);
        assert!(c.wgsl.contains("clamp(") && c.wgsl.contains("vec4<f32>(0.2)") && c.wgsl.contains("vec4<f32>(0.9)"), "{}", c.wgsl);
    }
}
