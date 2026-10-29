//! Runtime shader assembly (N12): N8's per-material scene shader (`matdef`), re-bound for the ash renderer, plus a
//! skinned vertex entry point and the post/resolve shader. naga compiles WGSL to SPIR-V at runtime (pure Rust, so
//! the static library still cross-compiles for MSVC).
//!
//! Set layout (the matdef source declares material = group 1, lightmap = group 2; they are swapped here so the
//! fixed-layout lightmap set comes before the per-ntex material set and stays bound across pipeline layouts):
//! - set 0: globals, instances, light words, dynamic lights, per-instance light ids, light environments, bones,
//!   B-189 shadows (7), shadow depth maps (8) and their comparison sampler (9)
//! - set 1: lightmap coefficient textures 0..2 + sampler (binding 3)
//! - set 2: material sampler (0), parameters (1), textures (2..)

use anyhow::{Result, anyhow};
use matdef::codegen::Compiled;
use matdef::shader::{ShaderFlags, assemble};

/// Skinned vertex stage: the bone palette gives a UE3 world position; swizzled to the shader's Y-up. Skinned
/// instances live in the same instance array (flags 0 = lit by sun/ambient/environment, p1 = first bone).
const SKIN_WGSL: &str = r#"
@group(0) @binding(6) var<storage, read> bones: array<mat4x4<f32>>;

@vertex
fn vs_skin(@location(0) pos: vec3<f32>, @location(1) nrm: vec3<f32>, @location(2) uv: vec2<f32>,
           @location(3) bi: vec4<u32>, @location(4) bw: vec4<f32>, @builtin(instance_index) ii: u32) -> VOut {
    let inst = instances[ii];
    let base = inst.p1;
    let m = bones[base + bi.x] * bw.x + bones[base + bi.y] * bw.y + bones[base + bi.z] * bw.z + bones[base + bi.w] * bw.w;
    let pu = (m * vec4<f32>(pos, 1.0)).xyz;
    let nu = (m * vec4<f32>(nrm, 0.0)).xyz;
    var o: VOut;
    o.wpos = vec3<f32>(pu.x, pu.z, pu.y);
    o.pos = g.view_proj * vec4<f32>(o.wpos, 1.0);
    let n = normalize(vec3<f32>(nu.x, nu.z, nu.y) + vec3<f32>(0.0, 1e-6, 0.0));
    o.nrm = n;
    let a = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(n.x) > 0.9);
    o.tan = vec4<f32>(normalize(cross(a, n)), 1.0);
    o.uv = vec4<f32>(uv, uv);
    o.vcol = vec4<f32>(1.0);
    o.l0 = vec3<f32>(0.0); o.l1 = vec3<f32>(0.0); o.l2 = vec3<f32>(0.0);
    o.inst = ii;
    return o;
}
"#;

/// B-189: the port's modulated light-environment shadows, replacing matdef's `shadow_mod` stub. Each shadow is a
/// perspective depth map of one object's casters seen from its environment's shadow light (`vkr/shadow.rs`). The port
/// (capture f1938, draws 792-809) multiplies the scene colour by `lerp(m, 1, lit^ModShadowFadeoutExponent)`, with
/// `m = 1 - (1 - ModShadowColor) * fade` and `fade = max(1 - x^2, 1e-4)^ShadowFalloffExponent`,
/// `x = max(d * scale + bias, 0)`, `d` = distance to the light / its radius. `misc.y` is the screen-size fade
/// (UE3's MinShadowResolution..ShadowFadeResolution), `misc.z` the part of the layer the map uses, `misc.w` 1/layer
/// size, `par.w` the world size of a map texel per unit of distance from the light (for the receiver offset).
pub const SHADOW_WGSL: &str = r#"
struct Shadow { vp: mat4x4<f32>, pos: vec4<f32>, col: vec4<f32>, par: vec4<f32>, misc: vec4<f32>, rect: vec4<f32> };
@group(0) @binding(7) var<storage, read> shadows: array<Shadow>;
@group(0) @binding(8) var shadow_map: texture_depth_2d_array;
@group(0) @binding(9) var shadow_cmp: sampler_comparison;

fn shadow_mod(ii: u32, wpos: vec3<f32>, n: vec3<f32>, fp: vec2<f32>) -> vec3<f32> {
    var m = vec3<f32>(1.0);
    let cnt = u32(g.tune2.w);
    // B-231: bit k of the instance's receiver mask is set when shadow k can reach the instance's bounds
    // (`vkr/shadow.rs` `shadow_receivers`); the loop visits only those.
    // The loop stays the uniform `k < cnt` loop (the per-draw mask is a cheap coherent branch inside it).
    let bits = instances[ii].p2;
    if (bits == 0u) { return m; }
    for (var k = 0u; k < cnt; k++) {
        if ((bits & (1u << k)) == 0u) { continue; }
        // B-205: fields are read one at a time, each just before its first use, so a pixel that a shadow skips
        // early doesn't load the whole 144-byte record.
        // Outside the screen rectangle around shadow k's projected volume (`vkr/shadow.rs` `select_shadows`) it
        // would take one of the `continue`s below anyway, so the result is unchanged.
        let rc = shadows[k].rect;
        if (fp.x < rc.x || fp.y < rc.y || fp.x > rc.z || fp.y > rc.w) { continue; }
        let pos = shadows[k].pos;
        let dist = length(pos.xyz - wpos);
        let d = dist * pos.w;
        if (d >= 1.0) { continue; }
        // The most this shadow can darken the pixel; below half a display step the taps can't change it.
        let par = shadows[k].par;
        let col = shadows[k].col;
        let misc = shadows[k].misc;
        let x = max(d * par.x + par.y, 0.0);
        let fade = pow(max(clamp(1.0 - x * x, 0.0, 1.0), 1e-4), par.z);
        let dark = (vec3<f32>(1.0) - col.rgb) * (fade * misc.y);
        if (max(dark.r, max(dark.g, dark.b)) < 0.002) { continue; }
        let p = shadows[k].vp * vec4<f32>(wpos + n * (1.5 * par.w * dist), 1.0);
        if (p.w <= 0.0) { continue; }
        let ndc = p.xyz / p.w;
        // Outside the map, or nearer the light than every caster (depth 0 is the casters' near side): lit.
        if (abs(ndc.x) >= 1.0 || abs(ndc.y) >= 1.0 || ndc.z <= 0.0) { continue; }
        let sc = misc.z;
        let rt = misc.w;
        let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * sc;
        let r = min(ndc.z, 0.99999);
        let layer = i32(misc.x);
        let lo = vec2<f32>(0.5 * rt);
        let hi = vec2<f32>(sc - 0.5 * rt);
        var lit = textureSampleCompareLevel(shadow_map, shadow_cmp, clamp(uv + vec2<f32>(-rt, -rt), lo, hi), layer, r);
        lit = lit + textureSampleCompareLevel(shadow_map, shadow_cmp, clamp(uv + vec2<f32>(rt, -rt), lo, hi), layer, r);
        lit = lit + textureSampleCompareLevel(shadow_map, shadow_cmp, clamp(uv + vec2<f32>(-rt, rt), lo, hi), layer, r);
        lit = lit + textureSampleCompareLevel(shadow_map, shadow_cmp, clamp(uv + vec2<f32>(rt, rt), lo, hi), layer, r);
        lit = lit * 0.25;
        let mm = vec3<f32>(1.0) - dark;
        m = m * mix(mm, vec3<f32>(1.0), pow(lit, col.w));
    }
    return m;
}
"#;

/// B-189: the shadow depth pass. Depth only (no fragment stage); `pc.k` picks the shadow. The layouts match set 0.
pub const SHADOW_DEPTH_WGSL: &str = r#"
struct Instance {
    model: mat4x4<f32>,
    light_base: i32,
    flags: u32,
    p1: u32,
    p2: u32,
    sc0: vec4<f32>,
    sc1: vec4<f32>,
    sc2: vec4<f32>,
    lm_xf: vec4<f32>,
};
struct Shadow { vp: mat4x4<f32>, pos: vec4<f32>, col: vec4<f32>, par: vec4<f32>, misc: vec4<f32>, rect: vec4<f32> };
struct Pc { k: u32 };
@group(0) @binding(1) var<storage, read> instances: array<Instance>;
@group(0) @binding(6) var<storage, read> bones: array<mat4x4<f32>>;
@group(0) @binding(7) var<storage, read> shadows: array<Shadow>;
var<immediate> pc: Pc;

@vertex
fn vs_static(@location(0) pos: vec3<f32>, @builtin(instance_index) ii: u32) -> @builtin(position) vec4<f32> {
    return shadows[pc.k].vp * (instances[ii].model * vec4<f32>(pos, 1.0));
}

@vertex
fn vs_skin(@location(0) pos: vec3<f32>, @location(3) bi: vec4<u32>, @location(4) bw: vec4<f32>,
           @builtin(instance_index) ii: u32) -> @builtin(position) vec4<f32> {
    let base = instances[ii].p1;
    let m = bones[base + bi.x] * bw.x + bones[base + bi.y] * bw.y + bones[base + bi.z] * bw.z + bones[base + bi.w] * bw.w;
    let pu = (m * vec4<f32>(pos, 1.0)).xyz;
    return shadows[pc.k].vp * vec4<f32>(pu.x, pu.z, pu.y, 1.0);
}
"#;

pub fn scene_source(c: &Compiled, f: ShaderFlags) -> String {
    let mut s = assemble(c, f)
        .replace(matdef::shader::SHADOW_STUB, SHADOW_WGSL)
        .replace("@group(1)", "@group(9)")
        .replace("@group(2)", "@group(1)")
        .replace("@group(9)", "@group(2)");
    s.push_str(SKIN_WGSL);
    s
}

/// W4: the game's post chain (grade, DOF + bloom, gamma 2.2) and the gamma-only copy for the low-end path.
pub fn post_source() -> String {
    include_str!("post.wgsl").to_string()
}

/// WGSL -> SPIR-V words. Same writer options as the build-time shaders: clip-space Y flipped as wgpu does.
pub fn spirv(src: &str) -> Result<Vec<u32>> {
    spirv_caps(src, naga::valid::Capabilities::empty())
}

/// As `spirv`, with extra validator capabilities (the shadow depth pass uses an immediate, i.e. a push constant).
pub fn spirv_caps(src: &str, caps: naga::valid::Capabilities) -> Result<Vec<u32>> {
    let module = naga::front::wgsl::parse_str(src)
        .map_err(|e| anyhow!("wgsl: {}", e.emit_to_string(src)))?;
    let info = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), caps)
        .validate(&module)
        .map_err(|e| anyhow!("validate: {e:?}"))?;
    let mut opts = naga::back::spv::Options::default();
    opts.flags.remove(naga::back::spv::WriterFlags::DEBUG);
    naga::back::spv::write_vec(&module, &info, &opts, None).map_err(|e| anyhow!("spv: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use matdef::{MatDef, Node, TexRef};

    /// G6: a sub-UV particle material whose texture is uploaded raw compiles with `xe_pwl_raw` on both fetches, for the
    /// sprite and the mesh variant, and the B-229 sRGB-view variant still compiles.
    #[test]
    fn particle_pwl_raw_compiles() {
        use scenefmt::particles::{matprog, shader};
        let mat = serde_json::json!({
            "props": {"BlendMode": "BLEND_Translucent", "EmissiveColor": {"Expression": {"obj": 1}}},
            "expressions": {"1": {"class": "MaterialExpressionParticleSubUV", "props": {"Texture": {"path": "Pkg.Fire"}}}}
        });
        for raw in [true, false] {
            let c = matprog::compile_with(&mat, &|_| matprog::TexInfo {
                unpack: None,
                pwl: true,
                raw,
            });
            for mesh in [false, true] {
                let src = shader::module_source(&c, mesh);
                assert_eq!(src.contains("xe_pwl_raw(textureSample"), raw);
                spirv(&src).unwrap();
            }
        }
    }

    #[test]
    fn fallback_and_textured_compile() {
        let grey = MatDef {
            diffuse_const: Some([0.6, 0.6, 0.6, 1.0]),
            ..Default::default()
        };
        let c = matdef::codegen::compile(&grey, &[]);
        spirv(&scene_source(
            &c,
            ShaderFlags::new(false, false, false, 330),
        ))
        .unwrap();
        let tex = MatDef {
            textures: vec![TexRef {
                path: "a".into(),
                normal: false,
            }],
            nodes: vec![
                Node::Tex { tex: 0, uv: None },
                Node::Mask {
                    x: 0,
                    m: [false, false, false, true],
                },
            ],
            diffuse: Some(0),
            opacity_mask: Some(1),
            ..Default::default()
        };
        let c = matdef::codegen::compile(&tex, &[None]);
        spirv(&scene_source(&c, ShaderFlags::new(true, false, false, 330))).unwrap();
        spirv(&post_source()).unwrap();
        spirv_caps(SHADOW_DEPTH_WGSL, naga::valid::Capabilities::IMMEDIATES).unwrap();
        assert!(scene_source(&c, ShaderFlags::new(true, false, false, 330)).contains("shadow_map"));
    }

    /// B-244: a material whose normal reads LightVector compiles with the per-light re-evaluation (texture samples in
    /// uniform control flow), and one that doesn't leaves it out.
    #[test]
    fn lightvec_normal_compiles() {
        let m = MatDef {
            textures: vec![TexRef {
                path: "n".into(),
                normal: true,
            }],
            nodes: vec![Node::Tex { tex: 0, uv: None }, Node::LightVec],
            diffuse: Some(0),
            normal: Some(1),
            ..Default::default()
        };
        let c = matdef::codegen::compile(&m, &[None]);
        let src = scene_source(&c, ShaderFlags::new(false, false, false, 330));
        assert!(
            src.contains("if (true && true)"),
            "LightVector material should take the per-light path"
        );
        spirv(&src).unwrap();
        let grey = MatDef {
            diffuse_const: Some([0.6, 0.6, 0.6, 1.0]),
            ..Default::default()
        };
        let src = scene_source(
            &matdef::codegen::compile(&grey, &[]),
            ShaderFlags::new(false, false, false, 330),
        );
        assert!(src.contains("if (false && true)"));
    }

    /// B-235: a gamma-fetched texture and PWL lightmaps still make a valid shader, and only the flagged slot is curved.
    #[test]
    fn pwl_gamma_compiles() {
        let tex = MatDef {
            textures: vec![
                TexRef {
                    path: "a".into(),
                    normal: false,
                },
                TexRef {
                    path: "n".into(),
                    normal: true,
                },
            ],
            nodes: vec![
                Node::Tex { tex: 0, uv: None },
                Node::Tex { tex: 1, uv: None },
            ],
            diffuse: Some(0),
            normal: Some(1),
            ..Default::default()
        };
        let c =
            matdef::codegen::compile_gamma(&tex, &[None, None], &[false, false], &[true, false]);
        assert!(c.body.contains("xe_pwl(textureSample(t0"), "{}", c.body);
        assert!(!c.body.contains("xe_pwl(textureSample(t1"), "{}", c.body);
        let plain = matdef::codegen::compile_ex(&tex, &[None, None], &[false, false]);
        assert_ne!(c.key, plain.key);
        let f = ShaderFlags {
            lm_pwl: true,
            ..ShaderFlags::new(false, false, false, 330)
        };
        let src = scene_source(&c, f);
        assert!(src.contains("if (true) { return xe_pwl3(c); }"));
        spirv(&src).unwrap();
    }
}
