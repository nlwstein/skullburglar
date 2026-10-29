//! Shader assembly: splices a compiled material into the scene shader.
use crate::codegen::Compiled;

pub const SCENE_WGSL: &str = include_str!("scene.wgsl");
pub const ENVLIGHT_WGSL: &str = include_str!("envlight.wgsl");
pub const POST_WGSL: &str = include_str!("post.wgsl");
/// B-189: the scene shader's shadow hook. The ash renderer (n1ffi) replaces this stub with real modulated shadows.
pub const SHADOW_STUB: &str = "fn shadow_mod(ii: u32, wpos: vec3<f32>, n: vec3<f32>, fp: vec2<f32>) -> vec3<f32> { return vec3<f32>(1.0); }\n";

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct ShaderFlags {
    pub masked: bool,
    pub unlit: bool,
    pub translucent: bool,
    pub clip: u32, // clip value * 1000
    /// Feature toggles compiled into the shader (for per-step cost measurement and cheap targets).
    pub lm: bool,
    pub nmap: bool,
    pub spec: bool,
    pub fog: bool,
    /// N10: dynamic lights and light environments.
    pub dynl: bool,
    /// B-235: the lightmaps are uploaded raw (UNORM) and take Xenos's piecewise-linear gamma curve in the shader.
    pub lm_pwl: bool,
    /// What distance fog fades the surface toward: 0 the fog colour (opaque and alpha-blended draws), 1 black
    /// (additive: fog only dims what is added), 2 white (modulate: fog fades the darkening out). B-273.
    pub fog_mode: u8,
}

impl ShaderFlags {
    pub fn new(masked: bool, unlit: bool, translucent: bool, clip: u32) -> Self {
        ShaderFlags { masked, unlit, translucent, clip, lm: true, nmap: true, spec: true, fog: true, dynl: true, lm_pwl: false, fog_mode: 0 }
    }
}

pub fn assemble(c: &Compiled, f: ShaderFlags) -> String {
    let mut textures = String::new();
    let mut body = String::new();
    for l in c.body.lines() {
        if l.starts_with("@group(1) @binding(") {
            textures.push_str(l);
            textures.push('\n');
        } else {
            body.push_str(l);
            body.push('\n');
        }
    }
    SCENE_WGSL
        .replace("{{MP_N}}", &c.params.len().max(1).to_string())
        .replace("{{TEXTURES}}", &textures)
        .replace("{{ENVLIGHT}}", ENVLIGHT_WGSL)
        .replace("{{SHADOW}}", SHADOW_STUB)
        .replace("SHADOW_RX", if f.translucent { "false" } else { "true" })
        .replace("{{BODY}}", &body)
        .replace("LM_PWL", if f.lm_pwl { "true" } else { "false" })
        .replace("LM_ON", if f.lm { "true" } else { "false" })
        .replace("NMAP_ON", if f.nmap { "true" } else { "false" })
        .replace("SPEC_ON", if f.spec { "true" } else { "false" })
        .replace("DYN_ON", if f.dynl { "true" } else { "false" })
        .replace("USES_LIGHTVEC", if body.contains("fi.light_ts") { "true" } else { "false" })
        .replace("FOG_TARGET", match f.fog_mode { 1 => "vec3<f32>(0.0)", 2 => "vec3<f32>(1.0)", _ => "g.fog_col.rgb" })
        .replace("FOG_ON", if f.fog { "true" } else { "false" })
        .replace("MASKED", if f.masked { "true" } else { "false" })
        .replace("UNLIT", if f.unlit { "true" } else { "false" })
        .replace("CLIP", &format!("{:?}", f.clip as f32 / 1000.0))
        .replace("OPACITY_EXPR", if f.translucent { "clamp(m.opacity, 0.0, 1.0)" } else { "1.0" })
}
