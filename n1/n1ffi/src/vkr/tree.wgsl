// SpeedTree foliage (B-187), from n1gfx's speedtree.wgsl. Vertex data is tree-local in UE axes (Z up); the instance
// model is the full UE transform with columns 1 and 2 swapped (the renderer's `swz_model`), so it maps straight to the
// Y-up world. Leaf cards expand along the camera basis exactly like the game's leaf-card vertex shader (c7 right, c8
// up, c9 forward), scaled by the tree scale.

struct G {
    view_proj: mat4x4<f32>,
    cam_pos: vec4<f32>,
    right: vec4<f32>,
    up: vec4<f32>,
    fwd: vec4<f32>,
    sun_dir: vec4<f32>,
    sun_col: vec4<f32>,
    amb_up: vec4<f32>,
    amb_dn: vec4<f32>,
    fog_col: vec4<f32>,
    fog_par: vec4<f32>,
    tune: vec4<f32>,      // light gain, light gamma, alpha clip, -
};

struct Inst {
    model: mat4x4<f32>,
    misc: vec4<f32>,      // x = tree scale
    base: vec4<i32>,      // light-buffer vertex base per slot (branch+frond, leaf card, billboard, leaf mesh)
    sc: array<vec4<f32>, 12>,
};

@group(0) @binding(0) var<uniform> g: G;
@group(0) @binding(1) var<storage, read> insts: array<Inst>;
@group(0) @binding(2) var<storage, read> light: array<u32>;
@group(1) @binding(0) var tex: texture_2d<f32>;
@group(1) @binding(1) var smp: sampler;

struct VOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) irr: vec3<f32>,
    @location(2) wpos: vec3<f32>,
};

fn sample_light(i: u32, gamma: f32) -> vec3<f32> {
    // FColor bytes A,R,G,B -> unpack4x8unorm x=A, y=R, z=G, w=B
    return pow(unpack4x8unorm(light[i]).yzw, vec3<f32>(gamma));
}

@vertex
fn vs(@location(0) pos: vec3<f32>, @location(1) uv: vec2<f32>, @location(2) card: vec3<f32>,
      @location(3) nrm: vec3<f32>, @location(4) slot_vi: vec2<f32>, @builtin(instance_index) ii: u32) -> VOut {
    var wp = (insts[ii].model * vec4<f32>(pos, 1.0)).xyz;
    wp = wp + insts[ii].misc.x * (card.x * g.right.xyz + card.y * g.up.xyz + card.z * g.fwd.xyz);
    var o: VOut;
    o.pos = g.view_proj * vec4<f32>(wp, 1.0);
    o.uv = uv;
    o.wpos = wp;
    let slot = u32(slot_vi.x);
    let b = insts[ii].base[slot];
    if (b >= 0) {
        let i = 3u * u32(b + i32(slot_vi.y));
        let c0 = sample_light(i, g.tune.y) * insts[ii].sc[slot * 3u].xyz;
        let c1 = sample_light(i + 1u, g.tune.y) * insts[ii].sc[slot * 3u + 1u].xyz;
        let c2 = sample_light(i + 2u, g.tune.y) * insts[ii].sc[slot * 3u + 2u].xyz;
        // Directional coefficients weighted for the unperturbed tangent-space normal (0,0,1): 1/3 each.
        o.irr = (c0 + c1 + c2) * (g.tune.x / 3.0);
    } else {
        let m3 = mat3x3<f32>(insts[ii].model[0].xyz, insts[ii].model[1].xyz, insts[ii].model[2].xyz);
        let n = normalize(m3 * nrm);
        let hemi = mix(g.amb_dn.rgb, g.amb_up.rgb, n.y * 0.5 + 0.5);
        // two-sided foliage: wrap the sun so back faces are not black
        o.irr = hemi + g.sun_col.rgb * g.sun_dir.w * (abs(dot(n, g.sun_dir.xyz)) * 0.6 + 0.2);
    }
    return o;
}

fn shade(i: VOut, base: vec3<f32>) -> vec3<f32> {
    var col = base * i.irr;
    if (g.fog_col.w > 0.5) {
        let depth = length(g.cam_pos.xyz - i.wpos);
        let d = max(depth - g.fog_par.z, 0.0);
        let hy = (g.cam_pos.y + i.wpos.y) * 0.5 - g.fog_par.y;
        let f = 1.0 - exp(-g.fog_par.x * d * exp(-max(hy, 0.0) * g.fog_par.w));
        col = mix(col, g.fog_col.rgb, clamp(f, 0.0, 1.0));
    }
    return col;
}

fn dbg(c: vec3<f32>) -> vec3<f32> {
    if (g.tune.w > 0.5) { return c * 0.3 + vec3<f32>(0.0, 2.0, 0.0); }
    return c;
}

@fragment
fn fs_opaque(i: VOut) -> @location(0) vec4<f32> {
    let t = textureSample(tex, smp, i.uv);
    return vec4<f32>(dbg(shade(i, t.rgb)), 1.0);
}

@fragment
fn fs_masked(i: VOut) -> @location(0) vec4<f32> {
    let t = textureSample(tex, smp, i.uv);
    if (t.a < g.tune.z) { discard; }
    return vec4<f32>(dbg(shade(i, t.rgb)), 1.0);
}
