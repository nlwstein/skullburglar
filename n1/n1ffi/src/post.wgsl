// The game's own post chain, decoded from the port's draws.
// Plain WGSL (naga -> SPIR-V). Colour here is display-linear: the port's scene texture value, which its composite
// turns into the output with pow(saturate(c), 1/2.2). No exposure, no filmic curve.
//
// Passes:
//   fs_grade  scene + depth -> graded scene (GenericOutdoor_MAT01's foreground / background grade; skipped when
//             the level's grade is the identity)
//   fs_down   graded scene + depth -> quarter res: rgb = sum((u + BloomScale * bright) * c) / 16, a = sum(u) / 16,
//             u the per-pixel blur amount (DOF), bright = any channel over BloomThreshold
//   fs_blur   quarter res gaussian, bilinear pairs, taps from the uniform (BlurKernelSize, then BloomKernelSize)
//   fs_comp   DOF lerp, the 15% mix of the bloom-kernel blur, saturate, gamma 2.2
//   fs_copy   low-end path: gamma only
//   fs_comp_lin  fs_comp without the gamma, into a full-res target, when post materials follow (B-266)
//   fs_gangsta, fs_death   post materials (B-266, n1ffi/src/postmat.rs): read the linear scene, write linear or (the last one)
//             gamma_out into the target. Their block: g0 the material's parameters, g1.x time (the game's world
//             time, the clock the materials' parameters use), g1.w real time (seconds, any origin), g1.y/z 1 = tex0/tex1
//             take Xenos's gamma curve (raw upload), comp.w 1 = last pass. tex0/tex1 sit in the bloom bindings.

struct P {
    a: vec4<f32>,       // x bloom scale, y bloom threshold, z/w texel size of the pass's source
    dof: vec4<f32>,     // x focus distance, y 1 / focus inner radius, z falloff exponent, w target x offset
    dof2: vec4<f32>,    // x max near blur, y max far blur, z near plane (reverse-Z infinite depth), w target y offset
    c: vec4<f32>,       // x/y blur direction, z 1 = UNORM target (write gamma), 0 = sRGB target, w tap count
    comp: vec4<f32>,    // x mix of the bloom-kernel blur, y 1 / gamma, z 1 = DOF/bloom on
    g0: vec4<f32>,      // x fg desaturate, y fg contrast, z bk brightness, w bk desaturate
    g1: vec4<f32>,      // x bk contrast, y fg-bg blend, z fg-bg blend offset, w sky clamp distance
    fg_tint: vec4<f32>,
    bk_tint: vec4<f32>,
    inv_hi: vec4<f32>,  // composite multiplier before saturate (1 / Scene_HighLights in UE3)
    taps: array<vec4<f32>, 6>,  // x offset (texels along c.xy), y weight; composite/copy: [4] fade scale, [5] overlay (B-240)
};
@group(0) @binding(0) var<uniform> p: P;
@group(0) @binding(1) var smp: sampler;
@group(0) @binding(2) var src: texture_2d<f32>;
@group(0) @binding(3) var bloom: texture_2d<f32>;
@group(0) @binding(4) var bloom2: texture_2d<f32>;
@group(0) @binding(5) var depth: texture_depth_2d;
@group(0) @binding(6) var wrap: sampler;   // repeat addressing, for post-material textures

struct VOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };

@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VOut {
    var o: VOut;
    let x = f32((vi << 1u) & 2u);
    let y = f32(vi & 2u);
    o.pos = vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
    o.uv = vec2<f32>(x, y);
    return o;
}

// View distance (UE units) at a full-res pixel; sky (depth 0) is far.
fn view_z(px: vec2<i32>) -> f32 {
    let d = textureLoad(depth, px, 0);
    return select(1.0e7, p.dof2.z / d, d > 0.0);
}

// The uber effect's blur amount at distance z.
fn blur_amount(z: f32) -> f32 {
    let dz = z - p.dof.x;
    let m = select(p.dof2.y, p.dof2.x, dz < 0.0);
    let f = pow(max(clamp(abs(dz) * p.dof.y, 0.0, 1.0), 1e-4), p.dof.z);
    return min(m, f);
}

fn desat(c: vec3<f32>, pct: f32, w: vec3<f32>) -> vec3<f32> {
    return mix(c, vec3<f32>(dot(c, w)), pct);
}

@fragment
fn fs_grade(i: VOut) -> @location(0) vec4<f32> {
    let px = vec2<i32>(i.pos.xy);
    let s = textureLoad(src, px, 0).rgb;
    let z = view_z(px);
    // foreground: desaturate, contrast toward grey, screen with the tint
    let fc = mix(desat(s, p.g0.x, vec3<f32>(0.3, 0.59, 0.11)), vec3<f32>(0.5), p.g0.y);
    let fg = 1.0 - (1.0 - fc) * (1.0 - p.fg_tint.rgb);
    // background: brightness, desaturate (weights 1,1,1 as the material has them), contrast, tint; only up to the
    // sky clamp distance
    let bd = desat(s * p.g0.z, p.g0.w, vec3<f32>(1.0));
    let bk = p.bk_tint.rgb * mix(bd, vec3<f32>(0.5), p.g1.x);
    let zb = z * p.g1.y;
    let bg = select(bk, s, zb > p.g1.w);
    return vec4<f32>(mix(fg, bg, clamp(zb + p.g1.z, 0.0, 1.0)), 1.0);
}

@fragment
fn fs_down(i: VOut) -> @location(0) vec4<f32> {
    let base = vec2<i32>(i.pos.xy) * 4;
    let lim = vec2<i32>(textureDimensions(src)) - 1;
    var acc = vec4<f32>(0.0);
    for (var y = 0; y < 4; y++) {
        for (var x = 0; x < 4; x++) {
            let q = min(base + vec2<i32>(x, y), lim);
            let c = textureLoad(src, q, 0).rgb;
            let u = blur_amount(view_z(q));
            let b = select(0.0, 1.0, any(c > vec3<f32>(p.a.y)));
            acc += vec4<f32>((u + p.a.x * b) * c, u);
        }
    }
    return acc / 16.0;
}

@fragment
fn fs_blur(i: VOut) -> @location(0) vec4<f32> {
    let d = p.c.xy * p.a.zw;
    var acc = vec4<f32>(0.0);
    let n = i32(p.c.w);
    for (var k = 0; k < n; k++) {
        let t = p.taps[k];
        acc += textureSample(src, smp, i.uv + d * t.x) * t.y;
    }
    return acc;
}

fn gamma_out(c: vec3<f32>) -> vec4<f32> {
    // B-240: the view's screen fade (camera and cinematic fades). The port's composite folds it into its pre-gamma
    // multiplier (c5.rgb = ColorScale * (1 - OverlayColor.a): pow(saturate(c * c5.rgb), 1 / 2.2)); the overlay
    // colour (OverlayColor.rgb * a) is added after gamma. Composite and copy carry them in tap slots 4 and 5.
    var g = pow(clamp(c * p.taps[4].xyz, vec3<f32>(0.0), vec3<f32>(1.0)), vec3<f32>(p.comp.y));
    g = min(g + p.taps[5].xyz, vec3<f32>(1.0));
    if (p.c.z < 0.5) {
        // sRGB target: the hardware encodes, so hand it the linear value whose encoding is g
        g = select(pow((g + 0.055) / 1.055, vec3<f32>(2.4)), g / 12.92, g <= vec3<f32>(0.04045));
    }
    return vec4<f32>(g, 1.0);
}

fn composite(px: vec2<i32>, uv: vec2<f32>) -> vec3<f32> {
    var c = textureLoad(src, px, 0).rgb;
    if (p.comp.z > 0.5) {
        let u = blur_amount(view_z(px));
        let a = textureSample(bloom, smp, uv);
        c = (c * (1.0 - u) + a.rgb) / max(a.a + (1.0 - u), 1e-4);
        c = mix(c, textureSample(bloom2, smp, uv).rgb, p.comp.x);
    }
    return c * p.inv_hi.rgb;
}

@fragment
fn fs_comp(i: VOut) -> @location(0) vec4<f32> {
    return gamma_out(composite(vec2<i32>(i.pos.xy - vec2<f32>(p.dof.w, p.dof2.w)), i.uv));
}

@fragment
fn fs_comp_lin(i: VOut) -> @location(0) vec4<f32> {
    return vec4<f32>(composite(vec2<i32>(i.pos.xy), i.uv), 1.0);
}

// ---- post materials (B-266 / B-267)

// Xenos's piecewise-linear gamma for a texture uploaded raw (as scenefmt's particle `xe_pwl_raw`).
fn xe_pwl_raw(c: vec4<f32>) -> vec4<f32> {
    let g = c.rgb;
    let s = max(max(g * 0.2490234375, g * 0.498046875 - 0.0625), max(g * 0.99609375 - 0.25, g * 1.9921875 - 1.0));
    return vec4<f32>(s, c.a);
}

fn mat_tex0(uv: vec2<f32>) -> vec4<f32> {
    let c = textureSample(bloom, wrap, uv);
    return select(c, xe_pwl_raw(c), p.g1.y > 0.5);
}

fn mat_tex1(uv: vec2<f32>) -> vec4<f32> {
    let c = textureSample(bloom2, wrap, uv);
    return select(c, xe_pwl_raw(c), p.g1.z > 0.5);
}

fn mat_out(c: vec3<f32>) -> vec4<f32> {
    if (p.comp.w > 0.5) {
        return gamma_out(c);
    }
    return vec4<f32>(c, 1.0);
}

// UE3 Rotator about (0.5, 0.5).
fn rotate_uv(uv: vec2<f32>, a: f32) -> vec2<f32> {
    let d = uv - vec2<f32>(0.5);
    let cs = cos(a);
    let sn = sin(a);
    return vec2<f32>(0.5) + vec2<f32>(d.x * cs - d.y * sn, d.x * sn + d.y * cs);
}

// FX_GangsterFire.GangsterFire_MAT02 (C1HUD.SlowmoEffect_MatInst), g0.x = PARAMAmount. tex0 streak, tex1 EyeMask.
@fragment
fn fs_gangsta(i: VOut) -> @location(0) vec4<f32> {
    let a = p.g0.x;
    let t = p.g1.x;
    let uv = i.uv;
    let d = uv - vec2<f32>(0.5);
    let r = length(d);
    // barrel warp toward the centre
    let w = textureSample(src, smp, mix(uv, vec2<f32>(0.5) + d * (1.0 - 0.5 * r * r), a)).rgb;
    // burn: darks and mids pushed up by 3x their colour
    let inner = w + (1.0 - clamp(dot(w, vec3<f32>(0.3, 0.59, 0.11)), 0.0, 1.0)) * (3.0 * w) * a;
    // outside the eye mask: the scene sampled along each ray at a fixed radius (a radial smear)
    let n = select(vec2<f32>(0.0), d / max(r, 1e-6), r > 1e-6);
    let smear = textureSample(src, smp, vec2<f32>(0.5) + 0.32 * n).rgb;
    let euv = d * mix(0.5, 0.9, a) + vec2<f32>(0.5);
    let m = clamp(mat_tex1(rotate_uv(euv, t * 0.25)).r + mat_tex1(rotate_uv(euv, t * -0.25)).r, 0.0, 1.0);
    let col = mix(smear, inner, m);
    // fire streaks panned outward, strongest in the corners
    let h = n.y * 0.5 + 0.5;
    let sx = select(h, 2.0 - h, n.x > 0.0) * 0.5;
    let suv = vec2<f32>(4.0, 0.6) * vec2<f32>(sx, r) + t * vec2<f32>(0.0, -1.0);
    let streak = mat_tex0(suv).rgb * 10.0 * pow(1.5 * r, 8.0);
    return mat_out(mix(vec3<f32>(1.0), vec3<f32>(1.0) + streak, a) * col);
}

// FX_Damage.DamageEffectMaterial (DamageEffectMaterial_MIC, B-292), g0.x = CriticalHealthFadeParameter. The scene fades
// toward a 75% desaturated copy. The material's hit-arrow render target is not drawn (it is black without a hit).
@fragment
fn fs_damage(i: VOut) -> @location(0) vec4<f32> {
    let s = textureSample(src, smp, i.uv).rgb;
    let d = desat(s, 0.75, vec3<f32>(0.3, 0.59, 0.11));
    return mat_out(mix(s, d, clamp(p.g0.x, 0.0, 1.0)));
}

@fragment
fn fs_copy(i: VOut) -> @location(0) vec4<f32> {
    return gamma_out(textureLoad(src, vec2<i32>(i.pos.xy - vec2<f32>(p.dof.w, p.dof2.w)), 0).rgb);
}

// UI_DeathEffect.FaderDeath_MAT (FaderDeath_MIC): g0.x StartTime, g0.y EffectLength. tex0 DeathScreenBackground_Alphas,
// tex1 HeatVisionClouds_Alpha. n = (Time - StartTime) / EffectLength on the world clock (StartTime is world time); the
// clouds and flicker run on real time: the death screen pauses the game and the port's clouds keep moving.
@fragment
fn fs_death(i: VOut) -> @location(0) vec4<f32> {
    let n = (p.g1.x - p.g0.x) / max(p.g0.y, 1e-3);
    let t = p.g1.w;
    let uv = i.uv;
    if (n > 0.5) {
        // dark red clouds, panned, framed by the background's blue channel, flickering between 0.7 and 1
        let r1 = mat_tex0(uv + t * vec2<f32>(0.035, -0.05)).r;
        let off = 0.2 * mat_tex1(uv).rg;
        let r2 = mat_tex0(uv * 1.2 + t * vec2<f32>(0.1, -0.03) + off).r;
        let tau = 6.283185307;
        let cloud = clamp((vec3<f32>(r1, 0.0, 0.0) + vec3<f32>(2.0 * r2, 0.1 * r2, 0.0)) * mat_tex0(uv).b,
                          vec3<f32>(0.0), vec3<f32>(1.0));
        let flicker = clamp(ceil(sin(t * tau / 0.3) * floor(cos(t * tau / 0.4))), 0.7, 1.0);
        return mat_out(mix(vec3<f32>(0.0), cloud * flicker, clamp((n - 0.5) * 2.0, 0.0, 1.0)));
    }
    // first half: a closing vignette that also pulls the scene inward, desaturation up to 80%, then fade to black
    let k = 2.0 * clamp(n, 0.0, 0.5);
    let q = clamp(pow(length(uv - vec2<f32>(0.5)) * mix(0.01, 2.0, k), 4.0), 0.0, 1.0);
    let coord = (1.0 - 0.2 * clamp(1.2 * q, 0.0, 1.0)) * (uv - vec2<f32>(0.5)) + vec2<f32>(0.5);
    let s = textureSample(src, smp, coord).rgb * (1.0 - q);
    let d = desat(s, clamp(k, 0.0, 0.8), vec3<f32>(0.3, 0.59, 0.11));
    let out_max = 1.0 - 2.0 * clamp(k - 0.5, 0.0, 1.0);
    return mat_out(clamp(d, vec3<f32>(0.0), vec3<f32>(1.0)) * out_max);
}
