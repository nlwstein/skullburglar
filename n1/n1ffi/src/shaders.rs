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
           @location(3) bi: vec4<u32>, @location(4) bw: vec4<f32>, @location(5) tn: vec4<f32>,
           @builtin(instance_index) ii: u32) -> VOut {
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
    // B-293: the mesh's own tangent, skinned like the normal. `BOTS_N1_SKIN_TANGENT=1` uses this; the default is still the invented one
    // (a fixed axis crossed with the normal).
    let tu = (m * vec4<f32>(tn.xyz, 0.0)).xyz;
    let a = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), abs(n.x) > 0.9);
    o.tan = SKIN_TANGENT;
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
    // Opt-in (B-293): no visible difference was measured on a desktop GPU, so the invented tangent stays the default.
    let skin_tan = if std::env::var("BOTS_N1_SKIN_TANGENT").is_ok_and(|v| v == "1") {
        "vec4<f32>(normalize(vec3<f32>(tu.x, tu.z, tu.y) + vec3<f32>(0.0, 1e-6, 0.0)), tn.w)"
    } else {
        "vec4<f32>(normalize(cross(a, n)), 1.0)"
    };
    let mut s = assemble(c, f)
        .replace(matdef::shader::SHADOW_STUB, SHADOW_WGSL)
        .replace("@group(1)", "@group(9)")
        .replace("@group(2)", "@group(1)")
        .replace("@group(9)", "@group(2)");
    s.push_str(&SKIN_WGSL.replace("SKIN_TANGENT", skin_tan));
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
    let module = naga::front::wgsl::parse_str(src).map_err(|e| anyhow!("wgsl: {}", e.emit_to_string(src)))?;
    let info = naga::valid::Validator::new(naga::valid::ValidationFlags::all(), caps)
        .validate(&module)
        .map_err(|e| anyhow!("validate: {e:?}"))?;
    let mut opts = naga::back::spv::Options::default();
    opts.flags.remove(naga::back::spv::WriterFlags::DEBUG);
    naga::back::spv::write_vec(&module, &info, &opts, None).map_err(|e| anyhow!("spv: {e}"))
}

/// B-332: the Adreno 740 driver (Quest 3) fails vkCreateGraphicsPipelines with VK_ERROR_UNKNOWN for a stage whose
/// module also declares other entry points (valid SPIR-V, and fine on every other GPU we test). On those GPUs each
/// stage gets its own copy of the module with only its entry point declared (`keep_entry`); elsewhere nothing changes.
static SPLIT_ENTRIES: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

pub fn set_split_entries(on: bool) {
    SPLIT_ENTRIES.store(on, std::sync::atomic::Ordering::Relaxed);
}

pub fn split_entries() -> bool {
    SPLIT_ENTRIES.load(std::sync::atomic::Ordering::Relaxed)
}

/// The instructions of a SPIR-V module after its header: (word index, word count, opcode).
fn insts(words: &[u32]) -> impl Iterator<Item = (usize, usize, u32)> + '_ {
    let mut i = 5;
    std::iter::from_fn(move || {
        let (n, op) = ((*words.get(i)? >> 16) as usize, words[i] & 0xffff);
        if n == 0 || i + n > words.len() {
            return None;
        }
        i += n;
        Some((i - n, n, op))
    })
}

/// The entry points of a SPIR-V module: (function id, name, interface ids).
fn entry_points(words: &[u32]) -> Vec<(u32, String, Vec<u32>)> {
    let mut out = Vec::new();
    for (i, n, op) in insts(words) {
        if op == 15 && n >= 4 {
            let name = &words[i + 3..i + n];
            // The name is a nul-terminated string padded to whole words; the interface ids follow it.
            let len = name.iter().position(|w| w.to_le_bytes().contains(&0)).map_or(name.len(), |k| k + 1);
            let bytes: Vec<u8> = name[..len].iter().flat_map(|w| w.to_le_bytes()).take_while(|&b| b != 0).collect();
            out.push((words[i + 2], String::from_utf8_lossy(&bytes).into_owned(), name[len..].to_vec()));
        }
    }
    out
}

/// `words` with only `entry` left: the other entry points go, with their execution modes, their functions and the
/// Input/Output variables only they use (and those ids' names and decorations). Types, constants and helper functions
/// are kept as they are (unused ones are still valid SPIR-V). B-332: Adreno 740 rejects a pipeline whose stage module
/// declares more than one entry point, or still holds the other entries' functions and interface variables.
pub fn keep_entry(words: &[u32], entry: &str) -> Vec<u32> {
    let eps = entry_points(words);
    let keep_iface: Vec<u32> = eps.iter().filter(|e| e.1 == entry).flat_map(|e| e.2.clone()).collect();
    let mut gone: std::collections::HashSet<u32> = eps.iter().filter(|e| e.1 != entry).map(|e| e.0).collect();
    let fns = gone.clone();
    for (i, n, op) in insts(words) {
        // OpVariable: result type, id, storage class (Input 1, Output 3).
        if op == 59 && n >= 4 && matches!(words[i + 3], 1 | 3) && !keep_iface.contains(&words[i + 2]) {
            gone.insert(words[i + 2]);
        }
    }
    let mut out = words[..5.min(words.len())].to_vec();
    let mut in_dropped_fn = false;
    let mut end = 5;
    for (i, n, op) in insts(words) {
        end = i + n;
        let skip = match op {
            54 => {
                // OpFunction: result type, id.
                in_dropped_fn = fns.contains(&words[i + 2]);
                in_dropped_fn
            }
            56 => std::mem::replace(&mut in_dropped_fn, false),
            _ if in_dropped_fn => true,
            15 => n >= 3 && gone.contains(&words[i + 2]),
            59 => n >= 3 && gone.contains(&words[i + 2]),
            // OpName, OpMemberName, OpExecutionMode, OpDecorate, OpMemberDecorate, OpExecutionModeId, OpDecorateId,
            // OpDecorateString, OpMemberDecorateString: the target is word 1.
            5 | 6 | 16 | 71 | 72 | 331 | 332 | 5632 | 5633 => n >= 2 && gone.contains(&words[i + 1]),
            _ => false,
        };
        if !skip {
            out.extend_from_slice(&words[i..i + n]);
        }
    }
    out.extend_from_slice(&words[end.min(words.len())..]);
    out
}

/// A module for one stage: `module` itself, or on `split_entries` GPUs a new module from `words` with only `entry`.
/// The caller destroys what `Some` holds.
///
/// # Safety
/// `device` must be valid.
pub unsafe fn stage_module(
    device: &ash::Device,
    module: ash::vk::ShaderModule,
    words: &[u32],
    entry: &str,
) -> Result<(ash::vk::ShaderModule, Option<ash::vk::ShaderModule>)> {
    if !split_entries() || words.is_empty() {
        return Ok((module, None));
    }
    let w = keep_entry(words, entry);
    let m = unsafe { device.create_shader_module(&ash::vk::ShaderModuleCreateInfo::default().code(&w), None)? };
    Ok((m, Some(m)))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// B-332: `keep_entry` leaves exactly the named entry point and drops only the other entry points' declarations.
    #[test]
    fn keep_entry_leaves_one_entry_point() {
        let w = spirv(&post_source()).unwrap();
        let all = entry_points(&w);
        assert!(all.len() > 2);
        let s = spirv_caps(SHADOW_DEPTH_WGSL, naga::valid::Capabilities::IMMEDIATES).unwrap();
        let dir = std::env::var_os("N1_SPV_DUMP").map(std::path::PathBuf::from);
        for (src, w) in [("post", &w), ("shadow", &s)] {
            for (_, name, iface) in entry_points(w) {
                let k = keep_entry(w, &name);
                let left = entry_points(&k);
                assert_eq!(left.len(), 1);
                assert_eq!((&left[0].1, &left[0].2), (&name, &iface));
                assert!(k.len() < w.len() && k[..5] == w[..5]);
                if let Some(d) = &dir {
                    let bytes: Vec<u8> = k.iter().flat_map(|w| w.to_le_bytes()).collect();
                    std::fs::write(d.join(format!("{src}-{name}.spv")), bytes).unwrap();
                }
            }
        }
    }
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
            let c = matprog::compile_with(&mat, &|_| matprog::TexInfo { unpack: None, pwl: true, raw });
            for mesh in [false, true] {
                let src = shader::module_source(&c, mesh);
                assert_eq!(src.contains("xe_pwl_raw(textureSample"), raw);
                spirv(&src).unwrap();
            }
        }
    }

    #[test]
    fn fallback_and_textured_compile() {
        let grey = MatDef { diffuse_const: Some([0.6, 0.6, 0.6, 1.0]), ..Default::default() };
        let c = matdef::codegen::compile(&grey, &[]);
        spirv(&scene_source(&c, ShaderFlags::new(false, false, false, 330))).unwrap();
        let tex = MatDef {
            textures: vec![TexRef { path: "a".into(), normal: false }],
            nodes: vec![Node::Tex { tex: 0, uv: None }, Node::Mask { x: 0, m: [false, false, false, true] }],
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
            textures: vec![TexRef { path: "n".into(), normal: true }],
            nodes: vec![Node::Tex { tex: 0, uv: None }, Node::LightVec],
            diffuse: Some(0),
            normal: Some(1),
            ..Default::default()
        };
        let c = matdef::codegen::compile(&m, &[None]);
        let src = scene_source(&c, ShaderFlags::new(false, false, false, 330));
        assert!(src.contains("if (true && true)"), "LightVector material should take the per-light path");
        spirv(&src).unwrap();
        let grey = MatDef { diffuse_const: Some([0.6, 0.6, 0.6, 1.0]), ..Default::default() };
        let src = scene_source(&matdef::codegen::compile(&grey, &[]), ShaderFlags::new(false, false, false, 330));
        assert!(src.contains("if (false && true)"));
    }

    /// B-235: a gamma-fetched texture and PWL lightmaps still make a valid shader, and only the flagged slot is curved.
    #[test]
    fn pwl_gamma_compiles() {
        let tex = MatDef {
            textures: vec![TexRef { path: "a".into(), normal: false }, TexRef { path: "n".into(), normal: true }],
            nodes: vec![Node::Tex { tex: 0, uv: None }, Node::Tex { tex: 1, uv: None }],
            diffuse: Some(0),
            normal: Some(1),
            ..Default::default()
        };
        let c = matdef::codegen::compile_gamma(&tex, &[None, None], &[false, false], &[true, false]);
        assert!(c.body.contains("xe_pwl(textureSample(t0"), "{}", c.body);
        assert!(!c.body.contains("xe_pwl(textureSample(t1"), "{}", c.body);
        let plain = matdef::codegen::compile_ex(&tex, &[None, None], &[false, false]);
        assert_ne!(c.key, plain.key);
        let f = ShaderFlags { lm_pwl: true, ..ShaderFlags::new(false, false, false, 330) };
        let src = scene_source(&c, f);
        assert!(src.contains("if (true) { return xe_pwl3(c); }"));
        spirv(&src).unwrap();
    }

    /// B-324: a clamp-addressed texture makes a valid shader that clamps its coordinate, the structure key changes with
    /// the addressing, and a repeating texture is untouched.
    #[test]
    fn clamp_addressing_compiles() {
        let tex = MatDef {
            textures: vec![TexRef { path: "a".into(), normal: false }, TexRef { path: "b".into(), normal: false }],
            nodes: vec![Node::Tex { tex: 0, uv: None }, Node::Tex { tex: 1, uv: None }],
            diffuse: Some(0),
            emissive: Some(1),
            ..Default::default()
        };
        let plain = matdef::codegen::compile(&tex, &[None, None]);
        let c = matdef::codegen::compile_addr(&tex, &[None, None], &[false, false], &[false, false], &[3, 1]);
        assert!(c.body.contains("textureSampleGrad(t0, smp, clamp("), "{}", c.body);
        assert!(c.body.contains("select(") && c.body.contains("textureSampleGrad(t1"), "{}", c.body);
        assert_ne!(c.key, plain.key);
        let none = matdef::codegen::compile_addr(&tex, &[None, None], &[false, false], &[false, false], &[0, 0]);
        assert_eq!(none.key, plain.key);
        spirv(&scene_source(&c, ShaderFlags::new(false, false, false, 330))).unwrap();
    }

    /// B-273: additive draws fade toward black in fog and modulate draws toward white; the rest toward the fog colour.
    #[test]
    fn fog_target_follows_the_blend() {
        let grey = MatDef { diffuse_const: Some([0.6, 0.6, 0.6, 1.0]), ..Default::default() };
        let c = matdef::codegen::compile(&grey, &[]);
        for (mode, want) in [(0u8, "mix(g.fog_col.rgb, col, t)"), (1, "mix(vec3<f32>(0.0), col, t)"), (2, "mix(vec3<f32>(1.0), col, t)")] {
            let f = ShaderFlags { fog_mode: mode, ..ShaderFlags::new(false, false, true, 330) };
            let src = scene_source(&c, f);
            assert!(src.contains(want), "mode {mode}");
            spirv(&src).unwrap();
        }
        assert_eq!(crate::plan::fog_mode(scenekit::Blend::Opaque), 0);
        assert_eq!(crate::plan::fog_mode(scenekit::Blend::Translucent), 0);
    }
    /// B-327: a material with a DepthBiasedAlpha opacity reads the scene depth (set 0, binding 10), fades by the planar
    /// depth gap, and still makes a valid shader; one without it declares no depth binding.
    #[test]
    fn soft_alpha_reads_the_scene_depth() {
        let m = MatDef {
            nodes: vec![
                Node::Const { v: [1.0, 0.7, 0.4, 0.0], n: 3 },
                Node::Const { v: [0.7, 0.0, 0.0, 0.0], n: 1 },
                Node::SoftAlpha { alpha: None, bias: Some(1), scale: 500.0 },
            ],
            emissive: Some(0),
            opacity: Some(2),
            blend: "BLEND_Additive".into(),
            ..Default::default()
        };
        let c = matdef::codegen::compile(&m, &[]);
        assert!(c.soft);
        let f = ShaderFlags::new(false, true, true, 330);
        let src = scene_source(&c, f);
        assert!(src.contains("@group(0) @binding(10) var scene_depth: texture_depth_2d;"));
        assert!(src.contains("fi.scene_w - fi.view_w"));
        spirv(&src).unwrap();
        let grey = MatDef { diffuse_const: Some([0.6, 0.6, 0.6, 1.0]), ..Default::default() };
        let c = matdef::codegen::compile(&grey, &[]);
        assert!(!c.soft);
        let src = scene_source(&c, f);
        assert!(!src.contains("scene_depth"));
        spirv(&src).unwrap();
    }
}
