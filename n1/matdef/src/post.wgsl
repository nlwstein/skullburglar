// Optional post pass: threshold + quarter-res bloom, then colour balance + tonemap composite.
// Plain WGSL (naga -> SPIR-V for the ash path). Runs on the HDR scene colour, before the HUD.
//
// Pass order: fs_bright (scene -> bloom A, quarter res), fs_blur (A -> B, horizontal), fs_blur (B -> A, vertical),
// fs_comp (scene + bloom A -> output).

struct P {
    a: vec4<f32>,       // x exposure, y contrast (gamma on the tone curve), z saturation, w bloom scale
    b: vec4<f32>,       // x bloom threshold, y white point, z/w texel size of the source texture
    c: vec4<f32>,       // x/y blur direction in texels
    shadow: vec4<f32>,  // colour balance multipliers (rgb) for shadows / midtones / highlights
    mid: vec4<f32>,
    high: vec4<f32>,
};
@group(0) @binding(0) var<uniform> p: P;
@group(0) @binding(1) var smp: sampler;
@group(0) @binding(2) var src: texture_2d<f32>;
@group(0) @binding(3) var bloom: texture_2d<f32>;

struct VOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };

@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VOut {
    // one oversized triangle
    var o: VOut;
    let x = f32((vi << 1u) & 2u);
    let y = f32(vi & 2u);
    o.pos = vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
    o.uv = vec2<f32>(x, y);
    return o;
}

fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.2126, 0.7152, 0.0722)); }

@fragment
fn fs_bright(i: VOut) -> @location(0) vec4<f32> {
    // 4 bilinear taps = 4x4 box of the source at quarter resolution
    let t = p.b.zw;
    var c = vec3<f32>(0.0);
    c += textureSample(src, smp, i.uv + vec2<f32>(-1.0, -1.0) * t).rgb;
    c += textureSample(src, smp, i.uv + vec2<f32>( 1.0, -1.0) * t).rgb;
    c += textureSample(src, smp, i.uv + vec2<f32>(-1.0,  1.0) * t).rgb;
    c += textureSample(src, smp, i.uv + vec2<f32>( 1.0,  1.0) * t).rgb;
    c = c * 0.25;
    let l = luma(c);
    let k = max(l - p.b.x, 0.0) / max(l, 1e-4);
    return vec4<f32>(c * k, 1.0);
}

@fragment
fn fs_blur(i: VOut) -> @location(0) vec4<f32> {
    // 9-tap gaussian as 5 bilinear taps
    let d = p.c.xy * p.b.zw;
    var c = textureSample(src, smp, i.uv).rgb * 0.2270270270;
    c += textureSample(src, smp, i.uv + d * 1.3846153846).rgb * 0.3162162162;
    c += textureSample(src, smp, i.uv - d * 1.3846153846).rgb * 0.3162162162;
    c += textureSample(src, smp, i.uv + d * 3.2307692308).rgb * 0.0702702703;
    c += textureSample(src, smp, i.uv - d * 3.2307692308).rgb * 0.0702702703;
    return vec4<f32>(c, 1.0);
}

@fragment
fn fs_comp(i: VOut) -> @location(0) vec4<f32> {
    var c = textureSample(src, smp, i.uv).rgb * p.a.x;
    c += textureSample(bloom, smp, i.uv).rgb * p.a.w;
    let l = luma(c);
    c = mix(vec3<f32>(l), c, p.a.z);
    // three-way colour balance weighted by luminance
    let ws = 1.0 - smoothstep(0.0, 0.35, l);
    let wh = smoothstep(0.35, 1.0, l);
    let wm = 1.0 - ws - wh;
    c = c * (p.shadow.rgb * ws + p.mid.rgb * wm + p.high.rgb * wh);
    // tone curve: gamma-style contrast on the toe, Reinhard shoulder to the white point
    c = pow(max(c, vec3<f32>(0.0)), vec3<f32>(p.a.y));
    let w2 = p.b.y * p.b.y;
    c = c * (1.0 + c / w2) / (1.0 + c);
    return vec4<f32>(c, 1.0);
}
