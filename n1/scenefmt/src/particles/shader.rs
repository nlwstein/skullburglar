//! WGSL for particle sprites and mesh particles plus the per-material fragment stage. Shared by the wgpu renderer
//! (`n1gfx`) and the ash renderer in the port (`n1ffi`), which turn it into SPIR-V with naga.

use super::matprog::{Blend, Compiled};

pub const PARTICLE_COMMON: &str = r#"
struct Globals { vp: mat4x4<f32>, right: vec4<f32>, up: vec4<f32>, fwd: vec4<f32>, cam: vec4<f32>, time: f32, near: f32, vw: f32, vh: f32, lit: array<vec4<f32>, 20> };
@group(0) @binding(0) var<uniform> u: Globals;
@group(0) @binding(1) var scene_depth: texture_depth_2d;
@group(1) @binding(0) var tex0: texture_2d<f32>;
@group(1) @binding(1) var tex1: texture_2d<f32>;
@group(1) @binding(2) var tex2: texture_2d<f32>;
@group(1) @binding(3) var tex3: texture_2d<f32>;
@group(1) @binding(4) var samp: sampler;

struct MatIn { uv: vec2<f32>, uv2: vec2<f32>, interp: f32, vcol: vec4<f32>, pixel_w: f32, scene_w: f32, screen: vec2<f32>, ndv: f32, wpos: vec3<f32>, nrm: vec3<f32> };

struct VOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) uv2: vec2<f32>,
    @location(2) col: vec4<f32>,
    @location(3) interp: f32,
    @location(4) w: f32,
    @location(5) ndv: f32,   // dot(normal, direction to the camera); sprites face the camera
//MESH_VOUT
};

// A colour sampled through an sRGB view, re-decoded with Xenos's piecewise-linear gamma curve (the original fetches
// these textures with the gamma sign): sRGB-encode back to the stored value, then the hardware's four segments.
fn xe_pwl(c: vec4<f32>) -> vec4<f32> {
    let l = clamp(c.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let g = select(1.055 * pow(l, vec3<f32>(1.0 / 2.4)) - 0.055, l * 12.92, l <= vec3<f32>(0.0031308)) * 255.0;
    let s = select(select(select(8.0 * g - 1024.0, 4.0 * g - 256.0, g < vec3<f32>(192.0)), 2.0 * g - 64.0, g < vec3<f32>(96.0)), g, g < vec3<f32>(64.0));
    return vec4<f32>(s / 1024.0, c.a);
}

// The same curve for a texture uploaded raw (UNORM view, `TexInfo::raw`): the sample is the stored value already, in
// 0..1. The four segments (slopes 1, 2, 4 and 8 in stored units, joined at 64, 96 and 192) form a convex curve, so it
// is the largest of the four lines: four multiply-adds and three max per channel instead of compares and selects.
fn xe_pwl_raw(c: vec4<f32>) -> vec4<f32> {
    let g = c.rgb;
    let s = max(max(g * 0.2490234375, g * 0.498046875 - 0.0625), max(g * 0.99609375 - 0.25, g * 1.9921875 - 1.0));
    return vec4<f32>(s, c.a);
}

fn scene_view_w(frag: vec2<f32>) -> f32 {
    let d = textureLoad(scene_depth, vec2<i32>(frag), 0);
    // reverse-Z infinite projection: z/w = near / w
    return select(1.0e9, u.near / d, d > 0.0);
}
"#;

/// B-250: lighting of lit mesh particles. `u.lit`: [0] sun direction (to the sun), w = 0 no lighting data (flat), 1 lit
/// without a sun, 2 lit with it; [1] sun colour; [2], [3] sky hemisphere up and down; [4..20] up to four dynamic
/// lights, 4 vec4 each as `lightfeed::pack::pack_light` (position and 1/radius, colour and falloff, cone axis and
/// outer cosine, kind and 1/(inner - outer cosine)); a light with 1/radius 0 is unused. The port draws a lit mesh
/// particle as a base pass (sky hemisphere, weights ((1 +- N.up) / 2)^2) plus one pass per light (colour x
/// (diffuse x N.L + SpecularColor x pow(R.L, SpecularPower)), no cast shadow), all unshadowed.
pub const LIT_MESH: &str = r#"
fn lit_normal(i: MatIn) -> vec3<f32> {
    let n = normalize(i.nrm);
    return select(n, -n, i.ndv < 0.0);
}

fn lit_dyn(k: u32, p: vec3<f32>, n: vec3<f32>, v: vec3<f32>, power: f32, spec: bool) -> vec3<f32> {
    let a = u.lit[4u + 4u * k];
    if (a.w <= 0.0) { return vec3<f32>(0.0); }
    let b = u.lit[5u + 4u * k];
    let c = u.lit[6u + 4u * k];
    let e = u.lit[7u + 4u * k];
    let d = a.xyz - p;
    let x = dot(d, d) * a.w * a.w;
    if (x >= 1.0) { return vec3<f32>(0.0); }
    let l = d * inverseSqrt(max(dot(d, d), 1.0e-6));
    let w = 1.0 - x;
    var att = w * w;
    if (b.w > 0.0) { att = pow(w, b.w); }
    if (e.x > 0.5) {
        let r = clamp((dot(-l, c.xyz) - c.w) * e.y, 0.0, 1.0);
        att = att * r * r;
    }
    if (spec) { return b.rgb * att * pow(max(dot(reflect(-v, n), l), 1.0e-4), max(power, 1.0)); }
    return b.rgb * att * max(dot(n, l), 0.0);
}

fn lit_irr(i: MatIn) -> vec3<f32> {
    if (u.lit[0].w < 0.5) { return vec3<f32>(0.7); }
    let n = lit_normal(i);
    let wu = n.y * 0.5 + 0.5;
    var irr = u.lit[2].rgb * (wu * wu) + u.lit[3].rgb * ((1.0 - wu) * (1.0 - wu));
    if (u.lit[0].w > 1.5) { irr = irr + u.lit[1].rgb * max(dot(n, u.lit[0].xyz), 0.0); }
    for (var k = 0u; k < 4u; k = k + 1u) { irr = irr + lit_dyn(k, i.wpos, n, vec3<f32>(0.0), 1.0, false); }
    return irr;
}

fn lit_spec(i: MatIn, power: f32) -> vec3<f32> {
    if (u.lit[0].w < 0.5) { return vec3<f32>(0.0); }
    let n = lit_normal(i);
    let v = normalize(u.cam.xyz - i.wpos);
    var s = vec3<f32>(0.0);
    if (u.lit[0].w > 1.5) { s = u.lit[1].rgb * pow(max(dot(reflect(-v, n), u.lit[0].xyz), 1.0e-4), max(power, 1.0)); }
    for (var k = 0u; k < 4u; k = k + 1u) { s = s + lit_dyn(k, i.wpos, n, v, power, true); }
    return s;
}
"#;

/// Sprites keep the flat estimate (UE3 lights them from the light environment and fake-lit normal maps).
pub const LIT_SPRITE: &str = r#"
fn lit_irr(i: MatIn) -> vec3<f32> { return vec3<f32>(0.7); }
fn lit_spec(i: MatIn, power: f32) -> vec3<f32> { return vec3<f32>(0.0); }
"#;

pub const SPRITE_VS: &str = r#"
struct SIn {
    @builtin(vertex_index) vi: u32,
    @location(0) a: vec4<f32>,   // position.xyz, rotation
    @location(1) b: vec4<f32>,   // old position.xyz, sub-UV interpolation
    @location(2) c: vec4<f32>,   // size.xy, uv1.xy
    @location(3) d: vec4<f32>,   // uv2.xy, uv size.xy
    @location(4) col: vec4<f32>,
};

fn sprite(i: SIn, velocity: bool) -> VOut {
    var cs = array<vec2<f32>, 6>(vec2<f32>(0.0, 0.0), vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0), vec2<f32>(1.0, 0.0));
    let k = cs[i.vi];
    let pos = i.a.xyz;
    var r = u.right.xyz;
    var up = u.up.xyz;
    var interp = i.b.w;
    let vel = pos - i.b.xyz;
    if (interp >= 3.5) {
        // EPAL_ROTATE_*: b.xyz is the axis; face the camera turning only about it (scenefmt::particles::lock_sprite)
        interp -= 4.0;
        up = normalize(i.b.xyz);
        r = normalize(cross(up, normalize(pos - u.cam.xyz)));
    } else if (interp >= 1.5) {
        // EPAL_X..EPAL_NEGATIVE_Z: b.xyz is the quad's normal. UE3's up is world X for the Z locks and world Z
        // otherwise (Y-up here: (1,0,0) and (0,1,0)), right = up x normal; the particle's rotation still turns it in the plane.
        interp -= 2.0;
        let n = normalize(i.b.xyz);
        let u0 = select(vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(1.0, 0.0, 0.0), abs(n.y) > 0.9);
        up = normalize(u0 - n * dot(u0, n));
        r = cross(n, up);
        let s = sin(i.a.w);
        let c = cos(i.a.w);
        let r0 = r * c + up * s;
        up = up * c - r * s;
        r = r0;
    } else if (velocity && dot(vel, vel) > 1.0e-8) {
        up = normalize(vel);
        let v = normalize(pos - u.cam.xyz);
        r = normalize(cross(up, v));
    } else {
        // Camera-facing roll turns the quad clockwise on screen for a positive rotation, as the port's vertex shader
        // does (right = cos * R - sin * U, up = sin * R + cos * U; B-236). The old sense here was mirrored.
        let s = sin(i.a.w);
        let c = cos(i.a.w);
        let r0 = r * c - up * s;
        up = up * c + r * s;
        r = r0;
    }
    let o = vec2<f32>((k.x - 0.5) * i.c.x, (0.5 - k.y) * i.c.y);
    let world = pos + r * o.x + up * o.y;
    var out: VOut;
    out.pos = u.vp * vec4<f32>(world, 1.0);
    out.uv = i.c.zw + k * i.d.zw;
    out.uv2 = i.d.xy + k * i.d.zw;
    out.col = i.col;
    out.interp = interp;
    out.w = out.pos.w;
    out.ndv = 1.0;
    return out;
}

@vertex fn vs_sprite(i: SIn) -> VOut { return sprite(i, false); }
@vertex fn vs_velocity(i: SIn) -> VOut { return sprite(i, true); }
"#;

pub const MESH_VS: &str = r#"
struct MIn {
    @location(0) pos: vec3<f32>,
    @location(1) nrm: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) m0: vec4<f32>, @location(4) m1: vec4<f32>, @location(5) m2: vec4<f32>, @location(6) m3: vec4<f32>,
    @location(7) col: vec4<f32>,
};
@vertex fn vs_mesh(i: MIn) -> VOut {
    let m = mat4x4<f32>(i.m0, i.m1, i.m2, i.m3);
    let world = m * vec4<f32>(i.pos, 1.0);
    var out: VOut;
    out.pos = u.vp * world;
    let n = (m * vec4<f32>(i.nrm, 0.0)).xyz;
    let v = u.cam.xyz - world.xyz;
    out.ndv = dot(n, v) * inverseSqrt(max(dot(n, n) * dot(v, v), 1.0e-12));
    out.uv = i.uv;
    out.uv2 = i.uv;
    out.col = vec4<f32>(select(i.col.rgb, min(i.col.rgb, vec3<f32>(1.0)), u.lit[1].w > 0.5), i.col.a);
    out.interp = 0.0;
    out.w = out.pos.w;
    out.wpos = world.xyz;
    out.wn = n;
    return out;
}
"#;

fn fragment_src(c: &Compiled, mesh: bool) -> String {
    let lit_in = if mesh {
        " m.wpos = i.wpos; m.nrm = i.wn;"
    } else {
        " m.wpos = vec3<f32>(0.0); m.nrm = vec3<f32>(0.0, 1.0, 0.0);"
    };
    let scene = if c.soft {
        "scene_view_w(i.pos.xy)"
    } else {
        "1.0e9"
    };
    let tail = match c.blend {
        Blend::Masked => format!(
            "    if (c.a < {:?}) {{ discard; }}\n    return vec4<f32>(c.rgb, 1.0);\n",
            c.clip
        ),
        Blend::Opaque => "    return vec4<f32>(c.rgb, 1.0);\n".to_string(),
        // Opacity is often VertexColor.r * a texture (UE3 casts a float4 Opacity to its first component), and
        // explosion emitters push the particle colour far over 1 (ExplosionA_MAT up to 5.5, FireBall01_MAT up to 9.7).
        // An alpha over 1 in our float target makes SRC_ALPHA/ONE_MINUS_SRC_ALPHA subtract the background. The
        // original's scene target (Xenos 2_10_10_10_FLOAT) keeps alpha in a 2-bit fixed-point channel, so the
        // blender saw it saturated; do the same (B-225).
        Blend::Translucent | Blend::Additive | Blend::Modulate => {
            "    return vec4<f32>(c.rgb, clamp(c.a, 0.0, 1.0));\n".to_string()
        }
    };
    format!(
        "{}\n@fragment fn fs(i: VOut) -> @location(0) vec4<f32> {{\n    var m: MatIn;\n    m.uv = i.uv; m.uv2 = i.uv2; m.interp = i.interp; m.vcol = i.col;\n    m.pixel_w = i.w; m.scene_w = {scene}; m.screen = i.pos.xy / vec2<f32>(u.vw, u.vh); m.ndv = i.ndv;{lit_in}\n    let c = material(m);\n{tail}}}\n",
        c.wgsl
    )
}

/// The WGSL module for one material and one vertex stage (used by tests too).
pub fn module_source(c: &Compiled, mesh: bool) -> String {
    let common = PARTICLE_COMMON.replace(
        "//MESH_VOUT",
        if mesh {
            "    @location(6) wpos: vec3<f32>,\n    @location(7) wn: vec3<f32>,"
        } else {
            ""
        },
    );
    // DestColor materials read the renderer's copy of the scene target (group 0 binding 2, before the sprite pass).
    let dest = if c.dest { "@group(0) @binding(2) var dest_color: texture_2d<f32>;\n" } else { "" };
    format!(
        "{}{}{}{}{}",
        common,
        dest,
        if mesh { LIT_MESH } else { LIT_SPRITE },
        if mesh { MESH_VS } else { SPRITE_VS },
        fragment_src(c, mesh)
    )
}

#[cfg(test)]
mod tests {
    /// G6: `xe_pwl_raw`'s max-of-four-lines form equals the select form of Xenos's curve over the whole 0..1 range.
    #[test]
    fn pwl_max_form_matches_segments() {
        for k in 0..=4096 {
            let c = k as f32 / 4096.0;
            let g = c * 255.0;
            let seg = if g < 64.0 {
                g
            } else if g < 96.0 {
                2.0 * g - 64.0
            } else if g < 192.0 {
                4.0 * g - 256.0
            } else {
                8.0 * g - 1024.0
            } / 1024.0;
            let mx = (c * 0.2490234375)
                .max(c * 0.498046875 - 0.0625)
                .max((c * 0.99609375 - 0.25).max(c * 1.9921875 - 1.0));
            assert!((seg - mx).abs() < 1e-6, "c {c}: {seg} vs {mx}");
        }
    }
}
