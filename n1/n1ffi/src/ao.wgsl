// Small, soft contact shading on the opaque scene, before particles and the post chain.
const AO_FADE_START: f32 = 4000.0;
const AO_FADE_END: f32 = 4500.0;

struct AoParams {
    inverse_view_proj: mat4x4<f32>,
    size: vec4<f32>,   // full-resolution width, height, inverse width, inverse height
    params: vec4<f32>, // near plane, world radius, strength, fog enabled
};
@group(0) @binding(1) var<uniform> a: AoParams;
@group(0) @binding(2) var scene_depth: texture_depth_2d;
@group(0) @binding(3) var ao: texture_2d<f32>; // visibility, linear view depth

struct VOut { @builtin(position) pos: vec4<f32> };

@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VOut {
    var o: VOut;
    let x = f32((vi << 1u) & 2u);
    let y = f32(vi & 2u);
    o.pos = vec4<f32>(x * 2.0 - 1.0, 1.0 - y * 2.0, 0.0, 1.0);
    return o;
}

fn ao_depth(p: vec2<i32>) -> f32 {
    return textureLoad(scene_depth, clamp(p, vec2<i32>(0), vec2<i32>(a.size.xy) - 1), 0);
}

struct AoNearest { pixel: vec2<i32>, depth: f32 };

// The nearest full-resolution pixel of an AO texel's 2x2 block: the one the texel is computed at. Keeps thin foreground
// geometry. The full-resolution depth guides the final upsample.
fn ao_nearest(texel: vec2<i32>) -> AoNearest {
    let base = texel * 2;
    let limit = vec2<i32>(a.size.xy) - 1;
    var nearest = AoNearest(min(base, limit), 0.0);
    for (var y = 0; y < 2; y++) {
        for (var x = 0; x < 2; x++) {
            let p = min(base + vec2<i32>(x, y), limit);
            let d = ao_depth(p);
            if (d > nearest.depth) { nearest = AoNearest(p, d); }
        }
    }
    return nearest;
}

// Reverse-Z depth is near / z, which is linear across the screen on a flat surface. Its change per pixel along a
// direction, taken from the neighbor on the same surface (the smaller difference) rather than across a silhouette.
fn ao_depth_slope(p: vec2<i32>, depth: f32, along: vec2<i32>) -> f32 {
    let before = p - along;
    let after = p + along;
    var backward = depth - ao_depth(before);
    var forward = ao_depth(after) - depth;
    // At the border, use two differences from the available side so a depth edge isn't taken for a slope.
    if (any(before < vec2<i32>(0))) {
        backward = forward;
        forward = ao_depth(after + along) - (depth + forward);
    } else if (any(after > vec2<i32>(a.size.xy) - 1)) {
        forward = backward;
        backward = (depth - backward) - ao_depth(before - along);
    }
    // Opposing differences do not establish a slope through this pixel.
    if (backward * forward < 0.0) { return 0.0; }
    return select(forward, backward, abs(backward) < abs(forward));
}

// An AO texel: visibility and linear view depth, both positive, and the full-resolution pixel of its 2x2 block it was
// computed at. That pixel is stored in the two signs (x in the visibility's, y in the depth's), so the filters compare
// depths where the texel really is instead of assuming a corner.
struct AoTexel { visibility: f32, z: f32, pixel: vec2<i32> };

fn ao_texel(texel: vec2<i32>) -> AoTexel {
    let t = textureLoad(ao, texel, 0).xy;
    let corner = select(vec2<i32>(0), vec2<i32>(1), t < vec2<f32>(0.0));
    return AoTexel(abs(t.x), abs(t.y), min(texel * 2 + corner, vec2<i32>(a.size.xy) - 1));
}

fn ao_pack(visibility: f32, z: f32, texel: vec2<i32>, pixel: vec2<i32>) -> vec2<f32> {
    return vec2<f32>(visibility, z) * select(vec2<f32>(1.0), vec2<f32>(-1.0), pixel != texel * 2);
}

fn ao_position(p: vec2<i32>, depth: f32) -> vec3<f32> {
    let uv = (vec2<f32>(p) + 0.5) * a.size.zw;
    let h = a.inverse_view_proj * vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), depth, 1.0);
    return h.xyz / h.w;
}

fn ao_normal(p: vec2<i32>, depth: f32, position: vec3<f32>) -> vec3<f32> {
    let limit = vec2<i32>(a.size.xy) - 1;
    let l = max(p - vec2<i32>(1, 0), vec2<i32>(0));
    let r = min(p + vec2<i32>(1, 0), limit);
    let u = max(p - vec2<i32>(0, 1), vec2<i32>(0));
    let d = min(p + vec2<i32>(0, 1), limit);
    let dl = ao_depth(l);
    let dr = ao_depth(r);
    let du = ao_depth(u);
    let dd = ao_depth(d);
    let eye = normalize(g.cam_pos.xyz - position);
    if ((dl == 0.0 && dr == 0.0) || (du == 0.0 && dd == 0.0)) { return eye; }
    // Take each derivative from the closer depth neighbor, rather than crossing a silhouette.
    let use_l = dl > 0.0 && p.x > 0 && (dr == 0.0 || p.x == limit.x || abs(dl - depth) < abs(dr - depth));
    let use_u = du > 0.0 && p.y > 0 && (dd == 0.0 || p.y == limit.y || abs(du - depth) < abs(dd - depth));
    let px = select(r, l, use_l);
    let py = select(d, u, use_u);
    let dx = ao_position(px, max(select(dr, dl, use_l), 1e-8)) - position;
    let dy = ao_position(py, max(select(dd, du, use_u), 1e-8)) - position;
    var n = cross(dx, dy);
    // Test the angle, since the derivative lengths change with resolution.
    if (dot(n, n) <= 1e-8 * dot(dx, dx) * dot(dy, dy)) { return eye; }
    n = normalize(n);
    return select(n, -n, dot(n, eye) < 0.0);
}

// Golden-angle azimuth and hemisphere height (reverse4(i) + 0.5) / 16, where reverse4 reverses
// the four bits of i. Scale the unit direction by 0.15 + 0.85 * ((i + 0.5) / 16)^2 to favor contact samples.
const AO_SAMPLES = array<vec3<f32>, 16>(
    vec3<f32>(0.15075641, 0.00000000, 0.00471344),
    vec3<f32>(-0.09837353, 0.09011821, 0.08365631),
    vec3<f32>(0.01432553, -0.16323206, 0.04802399),
    vec3<f32>(0.07241766, 0.09445602, 0.14896393),
    vec3<f32>(-0.21128813, -0.03737390, 0.03394318),
    vec3<f32>(0.15944230, -0.10142414, 0.16435089),
    vec3<f32>(-0.06885998, 0.25615568, 0.11792755),
    vec3<f32>(-0.06561742, -0.12634235, 0.30519562),
    vec3<f32>(0.36462142, 0.13315916, 0.03655243),
    vec3<f32>(-0.33444420, 0.13805370, 0.26698456),
    vec3<f32>(0.20540260, -0.43893342, 0.17739716),
    vec3<f32>(0.09463280, 0.30170429, 0.49706268),
    vec3<f32>(-0.56463783, -0.32721916, 0.14629974),
    vec3<f32>(0.51277047, -0.11273115, 0.54274750),
    vec3<f32>(-0.43085771, 0.61285075, 0.39754486),
    vec3<f32>(-0.03020875, -0.23311884, 0.91808929),
);

@fragment
fn fs_ao(i: VOut) -> @location(0) vec2<f32> {
    let texel = vec2<i32>(i.pos.xy);
    let nearest = ao_nearest(texel);
    let pixel = nearest.pixel;
    let depth = nearest.depth;
    if (depth <= 0.0) { return vec2<f32>(1.0, 65504.0); }
    let z = a.params.x / depth;
    if (z >= AO_FADE_END) { return ao_pack(1.0, min(z, 65504.0), texel, pixel); }
    let p = ao_position(pixel, depth);
    let n = ao_normal(pixel, depth, p);
    let axis = select(vec3<f32>(0.0, 0.0, 1.0), vec3<f32>(1.0, 0.0, 0.0), abs(n.z) > 0.9);
    let tangent = normalize(cross(axis, n));
    let bitangent = cross(n, tangent);
    // Keep the noise footprint at the same apparent size when the render resolution changes.
    let noise_pixel = floor((vec2<f32>(pixel) + 0.5) * (720.0 / a.size.y));
    // Interleaved Gradient Noise, Jorge Jimenez (SIGGRAPH 2014, Advanced Warfare post-processing).
    let angle = 6.2831853 * fract(52.9829189 * fract(dot(noise_pixel, vec2<f32>(0.06711056, 0.00583715))));
    let cs = cos(angle);
    let sn = sin(angle);
    let radius = a.params.y;
    let bias = max(0.1, radius * 0.02);
    var occlusion = 0.0;
    for (var k = 0; k < 16; k++) {
        let s = AO_SAMPLES[k];
        let r = vec2<f32>(cs * s.x - sn * s.y, sn * s.x + cs * s.y);
        let sample_position = p + radius * (tangent * r.x + bitangent * r.y + n * s.z);
        let clip = g.view_proj * vec4<f32>(sample_position, 1.0);
        if (clip.w <= 0.0) { continue; }
        let uv = clip.xy / clip.w * vec2<f32>(0.5, -0.5) + 0.5;
        if (any(uv < vec2<f32>(0.0)) || any(uv >= vec2<f32>(1.0))) { continue; }
        let d = ao_depth(vec2<i32>(uv * a.size.xy));
        if (d <= 0.0) { continue; }
        let sample_z = a.params.x / d;
        let range = 1.0 - smoothstep(radius * 0.5, radius, abs(z - sample_z));
        occlusion += smoothstep(bias, bias + radius * 0.1, clip.w - sample_z) * range;
    }
    let fade = 1.0 - smoothstep(AO_FADE_START, AO_FADE_END, z);
    let visibility = pow(clamp(1.0 - occlusion / 16.0, 0.0, 1.0), a.params.z);
    return ao_pack(mix(1.0, max(visibility, 0.15), fade), z, texel, pixel);
}

fn ao_filter(pixel: vec2<i32>, axis: vec2<i32>) -> vec2<f32> {
    let limit = vec2<i32>(textureDimensions(ao)) - 1;
    let center = ao_texel(pixel);
    // Nothing at or past the fade-out distance is shaded.
    if (center.z >= AO_FADE_END) { return ao_pack(center.visibility, center.z, pixel, center.pixel); }
    let sigma = max(1.0, center.z * 0.002);
    let spatial = array<f32, 4>(1.0, 0.8007374, 0.4111123, 0.1353353);
    let scale = a.size.y / 720.0;
    // Each tap is compared with the depth this surface would have at the tap's pixel, not with the center's: against
    // the center's, a floor or wall seen at an angle rejected every tap along its slope and kept its noise as streaks.
    let depth = ao_depth(center.pixel);
    let slope = vec2<f32>(ao_depth_slope(center.pixel, depth, vec2<i32>(1, 0)),
                          ao_depth_slope(center.pixel, depth, vec2<i32>(0, 1))) / a.params.x; // of 1 / z, per pixel
    var sum = center.visibility;
    var weight = 1.0;
    for (var k = -3; k <= 3; k++) {
        if (k == 0) { continue; }
        let offset = f32(k) * scale;
        let base = i32(floor(offset));
        let f = fract(offset);
        // Reject depth edges before interpolating the scaled tap, so a blended depth cannot hide a silhouette.
        for (var j = 0; j < 2; j++) {
            let linear = select(1.0 - f, f, j != 0);
            if (linear == 0.0) { continue; }
            // A tap clamped to the border is compared where the texel read is, not where the tap was asked for.
            let tap = ao_texel(clamp(pixel + axis * (base + j), vec2<i32>(0), limit));
            let expected = 1.0 / max(depth / a.params.x + dot(slope, vec2<f32>(tap.pixel - center.pixel)), 1e-6);
            let dz = (tap.z - expected) / sigma;
            let w = spatial[abs(k)] * linear * exp(-0.5 * dz * dz);
            sum += tap.visibility * w;
            weight += w;
        }
    }
    return ao_pack(sum / weight, center.z, pixel, center.pixel);
}

@fragment
fn fs_blur_h(i: VOut) -> @location(0) vec2<f32> {
    return ao_filter(vec2<i32>(i.pos.xy), vec2<i32>(1, 0));
}

@fragment
fn fs_blur_v(i: VOut) -> @location(0) vec2<f32> {
    return ao_filter(vec2<i32>(i.pos.xy), vec2<i32>(0, 1));
}

@fragment
fn fs_apply(i: VOut) -> @location(0) vec4<f32> {
    let pixel = vec2<i32>(i.pos.xy);
    let depth = ao_depth(pixel);
    if (depth <= 0.0) { return vec4<f32>(0.0, 0.0, 0.0, 1.0); }
    let z = a.params.x / depth;
    if (z >= AO_FADE_END) { return vec4<f32>(0.0, 0.0, 0.0, 1.0); }
    let q = i.pos.xy * 0.5 - 0.5;
    let base = vec2<i32>(floor(q));
    let f = fract(q);
    let limit = vec2<i32>(textureDimensions(ao)) - 1;
    var taps: array<AoTexel, 4>;
    var lowest = 1.0;
    for (var k = 0; k < 4; k++) {
        taps[k] = ao_texel(clamp(base + vec2<i32>(k & 1, k >> 1u), vec2<i32>(0), limit));
        lowest = min(lowest, taps[k].visibility);
    }
    if (lowest >= 0.9999) { return vec4<f32>(0.0, 0.0, 0.0, 1.0); }
    // On a surface seen at an angle a texel's depth differs from this pixel's by the slope between the two pixels.
    // Compare it with this surface's depth at the texel's pixel.
    let slope = vec2<f32>(ao_depth_slope(pixel, depth, vec2<i32>(1, 0)),
                          ao_depth_slope(pixel, depth, vec2<i32>(0, 1))) / a.params.x;
    let sigma = max(1.0, z * 0.002);
    var sum = 0.0;
    var weight = 0.0;
    var closest = 1.0;
    var closest_error = 1.0e30;
    for (var k = 0; k < 4; k++) {
        let expected = 1.0 / max(depth / a.params.x + dot(slope, vec2<f32>(taps[k].pixel - pixel)), 1e-6);
        let error = abs(taps[k].z - expected);
        let dz = error / sigma;
        let spatial = select(1.0 - f, f, vec2<bool>((k & 1) != 0, k >= 2));
        let w = spatial.x * spatial.y * exp(-0.5 * dz * dz);
        sum += taps[k].visibility * w;
        weight += w;
        if (error < closest_error) { closest_error = error; closest = taps[k].visibility; }
    }
    var visibility = sum / max(weight, 1e-4);
    if (weight < 1e-4) {
        // Fill only the missing weight with a range-limited stand-in so visibility stays continuous at the cutoff.
        let radius = a.params.y;
        let stand_in = mix(1.0, closest, 1.0 - smoothstep(radius * 0.5, radius, closest_error));
        visibility += stand_in * (1.0 - weight / 1e-4);
    }
    visibility = clamp(visibility, 0.0, 1.0);
    if (visibility >= 0.9999) { return vec4<f32>(0.0, 0.0, 0.0, 1.0); }
    var scatter = vec3<f32>(0.0);
    // The opaque scene is already fogged. Restore its in-scattering rather than darkening the fog with it.
    if (a.params.w > 0.5) { scatter = apply_fog(vec3<f32>(0.0), ao_position(pixel, depth), 0u); }
    return vec4<f32>(scatter * (1.0 - visibility), visibility);
}
