// Scene shader: vertex stage, fragment lighting, and (spliced in) the per-material `eval_material`.
// Plain WGSL, no wgpu-only features: naga compiles it to SPIR-V for the ash path.

struct Globals {
    view_proj: mat4x4<f32>,
    cam_pos: vec4<f32>,   // xyz world (Y-up), w = time in seconds
    sun_dir: vec4<f32>,   // xyz = direction TO the sun, w = 1 if present
    sun_col: vec4<f32>,   // rgb = colour * brightness
    amb_up: vec4<f32>,    // w: B-244 dynamic-light flags (bit 0 after the modulated shadows, bit 1 per-light LightVector, bit 2 specular); B-245 bit 3: env key specular as the port's
    amb_dn: vec4<f32>,
    fog_col: vec4<f32>,   // rgb, w = enabled
    fog_par: vec4<f32>,   // x density, y fog height (world Y), z start distance, w extinction distance (<= 0: none)
    tune: vec4<f32>,      // x lightmap gain, y spec gain, z vertex-light gain, w debug mode
    tune2: vec4<f32>,     // x vertex colour channel map, y sun-visibility luma, z normal-map strength, w unused
    tune3: vec4<f32>,     // N10: x light-environment gain, y ambient kept under an environment, z dynamic light gain, w vertex-lightmap gamma (B-189)
};

struct Instance {
    model: mat4x4<f32>,
    light_base: i32,
    flags: u32,           // 0 none (dynamic), 1 directional vertex light, 2 texture lightmap
    p1: u32,
    p2: u32,
    sc0: vec4<f32>,       // RGB scale per light coefficient (vertex light or lightmap)
    sc1: vec4<f32>,
    sc2: vec4<f32>,
    lm_xf: vec4<f32>,     // lightmap uv = uv1 * xy + zw
};

@group(0) @binding(0) var<uniform> g: Globals;
@group(0) @binding(1) var<storage, read> instances: array<Instance>;
@group(0) @binding(2) var<storage, read> light: array<u32>;
// N10: dynamic lights. See lightfeed/src/pack.rs for the layouts.
@group(0) @binding(3) var<storage, read> dyn_lights: array<vec4<f32>>;     // 4 vec4 per light
@group(0) @binding(4) var<storage, read> inst_dyn: array<vec2<u32>>;       // per instance: 4x8-bit light ids, env + 1
@group(0) @binding(5) var<storage, read> envs: array<vec4<f32>>;           // 13 vec4 per light environment

struct MatParams { c: array<vec4<f32>, {{MP_N}}> };
@group(1) @binding(0) var smp: sampler;
@group(1) @binding(1) var<uniform> mp: MatParams;
{{TEXTURES}}

@group(2) @binding(0) var lm0: texture_2d<f32>;
@group(2) @binding(1) var lm1: texture_2d<f32>;
@group(2) @binding(2) var lm2: texture_2d<f32>;
@group(2) @binding(3) var lms: sampler;

struct VOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) wpos: vec3<f32>,
    @location(1) nrm: vec3<f32>,
    @location(2) tan: vec4<f32>,
    @location(3) uv: vec4<f32>,
    @location(4) vcol: vec4<f32>,
    @location(5) l0: vec3<f32>,
    @location(6) l1: vec3<f32>,
    @location(7) l2: vec3<f32>,
    @location(8) @interpolate(flat) inst: u32,
};

// B-235: Xenos's piecewise-linear gamma-to-linear curve (the SDK's PWLGammaToLinear) on a raw (UNORM) sample, for
// textures the original fetches with the gamma sign. Four segments over the stored 0..255 value, out of 1024.
fn xe_pwl3(c: vec3<f32>) -> vec3<f32> {
    // G6: the four segments (slopes 1, 2, 4, 8 in stored units, joined at 64, 96, 192) form a convex curve, so it is
    // the largest of the four lines (stored value * 255 / 1024 folded in). The UNORM sample is already 0..1.
    return max(max(c * 0.2490234375, c * 0.498046875 - 0.0625), max(c * 0.99609375 - 0.25, c * 1.9921875 - 1.0));
}

fn xe_pwl(c: vec4<f32>) -> vec4<f32> {
    return vec4<f32>(xe_pwl3(c.rgb), c.a);
}

fn lm_sample(t: texture_2d<f32>, uv: vec2<f32>) -> vec3<f32> {
    let c = textureSample(t, lms, uv).rgb;
    if (LM_PWL) { return xe_pwl3(c); }
    return c;
}

fn vcol_map(c: vec4<f32>) -> vec4<f32> {
    // c = bytes in file order (A, R, G, B) read as unorm.
    let m = u32(g.tune2.x);
    if (m == 1u) { return c; }                       // as RGBA
    if (m == 2u) { return vec4<f32>(c.w, c.z, c.y, c.x); }  // BGRA from bytes
    return vec4<f32>(c.y, c.z, c.w, c.x);            // ARGB -> RGBA
}

fn light_at(idx: u32) -> vec3<f32> {
    // FColor bytes A,R,G,B -> unpack4x8unorm gives x=A, y=R, z=G, w=B. B-189: the port's vertex shader raises the
    // samples to the power 2.2 before the scales (vc255.x of capture f1938 draw 505), as the gamma fetch does for
    // texture lightmaps. g.tune3.w holds the exponent (BOTS_N1_ENVP=vlgamma=1 restores the old raw use).
    return pow(unpack4x8unorm(light[idx]).yzw, vec3<f32>(g.tune3.w));
}

@vertex
fn vs(@location(0) pos: vec3<f32>, @location(1) nrm: vec3<f32>, @location(2) tan: vec4<f32>,
      @location(3) uv0: vec2<f32>, @location(4) uv1: vec2<f32>, @location(5) col: vec4<f32>,
      @builtin(vertex_index) vi: u32, @builtin(instance_index) ii: u32) -> VOut {
    let inst = instances[ii];
    var o: VOut;
    let wp = inst.model * vec4<f32>(pos, 1.0);
    o.wpos = wp.xyz;
    o.pos = g.view_proj * wp;
    let m3 = mat3x3<f32>(inst.model[0].xyz, inst.model[1].xyz, inst.model[2].xyz);
    o.nrm = normalize(m3 * nrm);
    o.tan = vec4<f32>(normalize(m3 * tan.xyz), tan.w);
    var uv1v = uv1;
    if (inst.flags == 2u) { uv1v = uv1 * inst.lm_xf.xy + inst.lm_xf.zw; }
    o.uv = vec4<f32>(uv0, uv1v);
    o.vcol = vcol_map(col);
    o.l0 = vec3<f32>(0.0); o.l1 = vec3<f32>(0.0); o.l2 = vec3<f32>(0.0);
    if (LM_ON && inst.flags == 1u) {
        let b = 3u * u32(inst.light_base + i32(vi));
        o.l0 = light_at(b) * inst.sc0.xyz;
        o.l1 = light_at(b + 1u) * inst.sc1.xyz;
        o.l2 = light_at(b + 2u) * inst.sc2.xyz;
    }
    o.inst = ii;
    return o;
}

struct FragIn {
    uv0: vec2<f32>,
    uv1: vec2<f32>,
    vcol: vec4<f32>,
    view: vec3<f32>,
    nrm: vec3<f32>,
    wpos: vec3<f32>,
    depth: f32,
    view_ts: vec3<f32>,
    refl_ts: bool,        // B-261: ReflectionVector in tangent space about the material Normal (flag bit 4)
    light_ts: vec3<f32>,  // UE3 LightVector: the key light (sun or light environment directional), tangent space
};

struct MatOut {
    diffuse: vec3<f32>,
    specular: vec3<f32>,
    spec_power: f32,
    has_normal: bool,
    normal: vec3<f32>,
    emissive: vec3<f32>,
    opacity: f32,
    mask: f32,
};

// Two-channel normal map (BC5/DXN): X and Y unpacked to [-1, 1]; Z rebuilt.
fn recon_z(v: vec4<f32>) -> vec4<f32> {
    return vec4<f32>(v.xy, sqrt(clamp(1.0 - dot(v.xy, v.xy), 0.0, 1.0)), v.w);
}

fn rotate_uv(uv: vec2<f32>, center: vec2<f32>, angle: f32) -> vec2<f32> {
    let s = sin(angle);
    let c = cos(angle);
    let d = uv - center;
    return center + vec2<f32>(d.x * c - d.y * s, d.x * s + d.y * c);
}

fn dir_light(c0: vec3<f32>, c1: vec3<f32>, c2: vec3<f32>, n: vec3<f32>) -> vec3<f32> {
    let b0 = vec3<f32>(0.0, 0.81649658, 0.57735027);
    let b1 = vec3<f32>(-0.70710678, -0.40824829, 0.57735027);
    let b2 = vec3<f32>(0.70710678, -0.40824829, 0.57735027);
    var w = vec3<f32>(max(dot(n, b0), 0.0), max(dot(n, b1), 0.0), max(dot(n, b2), 0.0));
    w = w * w;
    return c0 * w.x + c1 * w.y + c2 * w.z;
}

{{ENVLIGHT}}
{{SHADOW}}
fn dyn_light_one(id: u32, wpos: vec3<f32>, N: vec3<f32>) -> vec3<f32> {
    let a = dyn_lights[4u * id];
    let b = dyn_lights[4u * id + 1u];
    let c = dyn_lights[4u * id + 2u];
    let e = dyn_lights[4u * id + 3u];
    var t = point_term(a, b, wpos, N);
    if (e.x > 0.5) {
        let L = normalize(a.xyz - wpos);
        let r = clamp((dot(-L, c.xyz) - c.w) * e.y, 0.0, 1.0);
        t = t * (r * r);
    }
    return t;
}

// B-244: a dynamic light's specular as the port's light pass draws it (PS 2a50cf68 on 50 Cent's body):
// pow(max(R.L, 1e-4), power) with R the view vector reflected about the normal, times the light's colour and distance
// falloff (no N.L) and the cone for spots. `x`: diffuse irradiance, `y`: specular (times the material's specular).
struct DynTerm { d: vec3<f32>, s: vec3<f32> };

fn dyn_light_spec(id: u32, wpos: vec3<f32>, N: vec3<f32>, V: vec3<f32>, power: f32) -> vec3<f32> {
    let a = dyn_lights[4u * id];
    let b = dyn_lights[4u * id + 1u];
    let c = dyn_lights[4u * id + 2u];
    let e = dyn_lights[4u * id + 3u];
    let d = a.xyz - wpos;
    let x = dot(d, d) * a.w * a.w;
    if (x >= 1.0) { return vec3<f32>(0.0); }
    let L = d * inverseSqrt(max(dot(d, d), 1e-6));
    var att = (1.0 - x) * (1.0 - x);
    if (b.w > 0.0) { att = pow(1.0 - x, b.w); }
    if (e.x > 0.5) {
        let r = clamp((dot(-L, c.xyz) - c.w) * e.y, 0.0, 1.0);
        att = att * r * r;
    }
    let R = reflect(-V, N);
    return b.rgb * att * pow(max(dot(R, L), 1e-4), max(power, 1.0));
}

// `first`: the instance's first listed light is left out (B-244: the caller lit it with its own normal).
fn dyn_light_sum(ii: u32, wpos: vec3<f32>, N: vec3<f32>, first: bool, V: vec3<f32>, power: f32, spec: bool) -> DynTerm {
    var acc = DynTerm(vec3<f32>(0.0), vec3<f32>(0.0));
    let ids = inst_dyn[ii].x;
    for (var s = select(1u, 0u, first); s < 4u; s = s + 1u) {
        let id = (ids >> (8u * s)) & 0xFFu;
        if (id == 0xFFu) { break; }
        acc.d = acc.d + dyn_light_one(id, wpos, N);
        if (spec) { acc.s = acc.s + dyn_light_spec(id, wpos, N, V, power); }
    }
    return acc;
}

{{BODY}}

@fragment
fn fs(i: VOut, @builtin(front_facing) ff: bool) -> @location(0) vec4<f32> {
    let inst = instances[i.inst];
    var N = normalize(i.nrm);
    if (!ff) { N = -N; }
    let T0 = normalize(i.tan.xyz - N * dot(N, i.tan.xyz));
    let B = cross(N, T0) * i.tan.w;
    let V = normalize(g.cam_pos.xyz - i.wpos);
    var fi: FragIn;
    fi.uv0 = i.uv.xy;
    fi.uv1 = i.uv.zw;
    fi.vcol = i.vcol;
    fi.view = V;
    fi.nrm = N;
    fi.wpos = i.wpos;
    fi.depth = length(g.cam_pos.xyz - i.wpos);
    fi.view_ts = vec3<f32>(dot(V, T0), dot(V, B), dot(V, N));
    // B-189: TemplateBody_Mat and other skin materials bend their normal toward LightVector (a wrap).
    var Lk = g.sun_dir.xyz;
    if (DYN_ON && inst.flags == 0u) {
        let ev0 = inst_dyn[i.inst].y;
        if (ev0 > 0u) { Lk = envs[13u * (ev0 - 1u)].xyz; }
    }
    fi.light_ts = vec3<f32>(dot(Lk, T0), dot(Lk, B), dot(Lk, N));
    fi.refl_ts = (u32(g.amb_up.w) & 16u) != 0u;
    let m = eval_material(fi);
    if (MASKED && m.mask < CLIP) { discard; }

    var nts = vec3<f32>(0.0, 0.0, 1.0);
    if (NMAP_ON && m.has_normal) {
        nts = normalize(mix(vec3<f32>(0.0, 0.0, 1.0), m.normal, g.tune2.z));
    }
    let Nw = normalize(T0 * nts.x + B * nts.y + N * nts.z);

    var irr: vec3<f32>;
    // B-189: light the port adds after its modulated shadows (the light environment's SH pass): not shadowed.
    var irr_post = vec3<f32>(0.0);
    var sun_vis = 1.0;
    // Specular key light: the sun, or the light environment's directional for env-lit objects (N12).
    var key_dir = g.sun_dir.xyz;
    var key_col = g.sun_col.rgb * g.sun_dir.w;
    var key_env = false;  // B-245: the key light is a light environment's directional
    if (!LM_ON) {
        irr = vec3<f32>(0.55);
    } else if (inst.flags == 1u) {
        irr = dir_light(i.l0, i.l1, i.l2, nts) * g.tune.z;
        sun_vis = clamp(dot(irr, vec3<f32>(0.3, 0.59, 0.11)) / max(g.tune2.y, 0.01), 0.0, 1.0);
    } else if (inst.flags == 2u) {
        let c0 = lm_sample(lm0, i.uv.zw) * inst.sc0.xyz;
        let c1 = lm_sample(lm1, i.uv.zw) * inst.sc1.xyz;
        let c2 = lm_sample(lm2, i.uv.zw) * inst.sc2.xyz;
        irr = dir_light(c0, c1, c2, nts) * g.tune.x;
        sun_vis = clamp(dot(irr, vec3<f32>(0.3, 0.59, 0.11)) / max(g.tune2.y, 0.01), 0.0, 1.0);
    } else {
        let hemi = mix(g.amb_dn.rgb, g.amb_up.rgb, Nw.y * 0.5 + 0.5);
        irr = hemi + g.sun_col.rgb * g.sun_dir.w * max(dot(Nw, g.sun_dir.xyz), 0.0);
        if (DYN_ON) {
            let ev = inst_dyn[i.inst].y;
            if (ev > 0u) {
                irr = env_key(ev - 1u, i.wpos, Nw) * g.tune3.x + hemi * g.tune3.y;
                irr_post = env_sh(ev - 1u, Nw) * g.tune3.x;
                let dd = envs[13u * (ev - 1u)];
                key_dir = dd.xyz;
                key_col = envs[13u * (ev - 1u) + 1u].rgb * dd.w * g.tune3.x;
                key_env = true;
            }
        }
    }
    var dyn_s = vec3<f32>(0.0);   // B-244: dynamic lights' specular (flag bit 2)
    var dyn_shadowed = false;
    if (DYN_ON) {
        // B-244: the port draws each dynamic light as its own pass of the material, with LightVector = that light, so
        // a material that bends its normal toward LightVector (TemplateBody_Mat's wrap: the characters' rim light)
        // bends it toward each light. Flag bit 1: the instance's first listed light (its strongest) gets the material
        // normal evaluated for it; the rest keep the key light's. One extra material evaluation, and only for
        // materials that read LightVector (compiled in). Unconditional so the texture samples stay in uniform control flow.
        let dflags = u32(g.amb_up.w);
        let dspec = (dflags & 4u) != 0u && SPEC_ON && !UNLIT;
        var dyn = vec3<f32>(0.0);
        var first = true;
        if (USES_LIGHTVEC && NMAP_ON) {
            let ids = inst_dyn[i.inst].x;
            let id0 = ids & 0xFFu;
            var f1 = fi;
            var L1 = Lk;
            if (id0 != 0xFFu) { L1 = normalize(dyn_lights[4u * id0].xyz - i.wpos); }
            f1.light_ts = vec3<f32>(dot(L1, T0), dot(L1, B), dot(L1, N));
            let m1 = eval_material(f1);
            var n1 = vec3<f32>(0.0, 0.0, 1.0);
            if (m1.has_normal) { n1 = normalize(mix(vec3<f32>(0.0, 0.0, 1.0), m1.normal, g.tune2.z)); }
            let N1 = normalize(T0 * n1.x + B * n1.y + N * n1.z);
            if ((dflags & 2u) != 0u && id0 != 0xFFu) {
                dyn = dyn_light_one(id0, i.wpos, N1);
                if (dspec) { dyn_s = m1.specular * dyn_light_spec(id0, i.wpos, N1, V, m1.spec_power); }
                first = false;
            }
        }
        let rest = dyn_light_sum(i.inst, i.wpos, Nw, first, V, m.spec_power, dspec);
        dyn = (dyn + rest.d) * g.tune3.z;
        dyn_s = (dyn_s + m.specular * rest.s) * g.tune3.z * g.tune.y;
        // Flag bit 0: the port draws the dynamic-light passes after its modulated shadows, so they are not shadowed.
        if ((dflags & 1u) != 0u) { irr_post = irr_post + dyn; } else { irr = irr + dyn; dyn_shadowed = true; }
    }
    if (g.tune.w > 0.5 && g.tune.w < 1.5) { irr = vec3<f32>(0.55); irr_post = vec3<f32>(0.0); sun_vis = 0.0; }  // debug 1: flat light (materials only)
    // B-189: the port's modulated shadows multiply the scene colour drawn so far (opaque and masked only; translucency
    // comes after them). `shadow_mod` is 1 unless the renderer splices shadows in. B-205: it gets the pixel position
    // too, to skip shadows whose screen rectangle doesn't hold the pixel.
    var shm = vec3<f32>(1.0);
    if (SHADOW_RX) { shm = shadow_mod(i.inst, i.wpos, N, i.pos.xy); }
    var col = (m.diffuse * irr + m.emissive) * shm + m.diffuse * irr_post;
    col = col + dyn_s * select(vec3<f32>(1.0), shm, dyn_shadowed);
    if (g.tune.w > 1.5 && g.tune.w < 2.5) { col = (irr * shm + irr_post) * 0.5; }  // debug 2: the lighting alone (no material)
    if (g.tune.w > 2.5 && g.tune.w < 3.5) { col = Nw * 0.5 + 0.5; }  // debug 3: shading normal (Y-up world)
    if (g.tune.w > 3.5 && g.tune.w < 4.5) { col = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), ff); }  // debug 4: front faces green
    if (!UNLIT && SPEC_ON) {
        let H = normalize(key_dir + V);
        var sp = pow(max(dot(Nw, H), 0.0), max(m.spec_power, 1.0)) * max(dot(Nw, key_dir), 0.0);
        // B-245 (flag bit 3): a light environment's directional pass in the port (PS 8a36feff, TemplateBody_Mat) is
        // Phong, pow(max(saturate(R.L), 1e-4), power) with R the view vector reflected about the shading normal, and
        // no N.L. Blinn N.H at the same power is a far wider lobe: a blue-grey sheen over the characters' skin.
        if (key_env && (u32(g.amb_up.w) & 8u) != 0u) {
            sp = pow(max(clamp(dot(reflect(-V, Nw), key_dir), 0.0, 1.0), 1e-4), max(m.spec_power, 1.0));
        }
        col = col + m.specular * key_col * sp * sun_vis * g.tune.y * shm;
    } else if (UNLIT) {
        col = m.emissive;
    }
    // height fog, as UE3's (W4, decoded from the port's fog pass): a layer below the fog height; the fogged length
    // is the part of the camera ray inside it past the start distance, along view depth.
    if (FOG_ON && g.fog_col.w > 0.5) {
        let vz = (g.view_proj * vec4<f32>(i.wpos, 1.0)).w;
        if (vz > g.fog_par.z) {
            var dz = i.wpos.y - g.cam_pos.y;
            if (abs(dz) <= 0.001) { dz = 0.001; }
            let fr = abs(clamp((g.fog_par.y - g.cam_pos.y) / dz, 0.0, 1.0) - clamp((-1.0e30 - g.cam_pos.y) / dz, 0.0, 1.0));
            let dist = (vz - g.fog_par.z) * fr;
            var t = exp(-g.fog_par.x * dist);
            if (g.fog_par.w > 0.0 && dist >= g.fog_par.w) { t = 0.0; }
            col = mix(FOG_TARGET, col, t);
        }
    }
    return vec4<f32>(col, OPACITY_EXPR);
}
