// The live scene shader (livegfx on wgpu, n1ffi on ash via naga -> SPIR-V). Same shading as n1gfx: unlit diffuse x
// baked vertex light (unorm8x4 x 4), ambient when a primitive has none, alpha test for masked materials.
// Matrices are UE3 row-major row-vector FMatrix uploaded as-is: as WGSL column-major they are transposed, so M * v
// here is UE3's v * M.
struct Globals { view_proj: mat4x4<f32>, ambient: vec4<f32>, flags: vec4<f32> };
struct Inst { model: mat4x4<f32>, light_base: i32, has_light: i32, p0: i32, p1: i32 };
@group(0) @binding(0) var<uniform> g: Globals;
@group(0) @binding(1) var<storage, read> instances: array<Inst>;
@group(0) @binding(2) var<storage, read> light: array<u32>;
@group(1) @binding(0) var t_diffuse: texture_2d<f32>;
@group(1) @binding(1) var s_diffuse: sampler;

struct VOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32>, @location(1) col: vec3<f32> };

@vertex
fn vs(@location(0) pos: vec3<f32>, @location(1) uv: vec2<f32>, @builtin(vertex_index) vi: u32,
      @builtin(instance_index) ii: u32) -> VOut {
    let inst = instances[ii];
    var o: VOut;
    o.pos = g.view_proj * (inst.model * vec4<f32>(pos, 1.0));
    o.uv = uv;
    if (inst.has_light == 0) {
        o.col = g.ambient.rgb;
    } else {
        o.col = unpack4x8unorm(light[u32(inst.light_base + i32(vi))]).rgb * 4.0;
    }
    return o;
}

// Linear -> sRGB when the target is UNORM (g.flags.x = 1); an sRGB target encodes by itself.
fn encode(c: vec3<f32>) -> vec3<f32> {
    if (g.flags.x == 0.0) { return c; }
    let x = clamp(c, vec3<f32>(0.0), vec3<f32>(1.0));
    return select(1.055 * pow(x, vec3<f32>(1.0 / 2.4)) - 0.055, x * 12.92, x <= vec3<f32>(0.0031308));
}

@fragment
fn fs_opaque(i: VOut) -> @location(0) vec4<f32> {
    let t = textureSample(t_diffuse, s_diffuse, i.uv);
    return vec4<f32>(encode(t.rgb * i.col), 1.0);
}

@fragment
fn fs_masked(i: VOut) -> @location(0) vec4<f32> {
    let t = textureSample(t_diffuse, s_diffuse, i.uv);
    if (t.a < 0.33) { discard; }
    return vec4<f32>(encode(t.rgb * i.col), 1.0);
}

@fragment
fn fs_blend(i: VOut) -> @location(0) vec4<f32> {
    let t = textureSample(t_diffuse, s_diffuse, i.uv);
    return vec4<f32>(encode(t.rgb * i.col), t.a);
}
