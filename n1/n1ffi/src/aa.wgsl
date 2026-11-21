// B-054: anti-aliasing and sharpening for the final image (n1ffi/src/vkr/aa.rs records the passes).
//
// The post chain's last pass writes the display-encoded scene into an intermediate target the size of the view; these
// passes read it and write the result into the host's target, so the HUD and menus (drawn by the host afterwards)
// are never filtered. Colour here is whatever the target holds: gamma-encoded in a UNORM or HDR target, linear in an
// sRGB one (the hardware decodes on read and encodes on write). `perc` gives the perceptual value the edge detectors
// judge contrast on, so the three cases look at the same numbers.
//
// Passes (one entry point each):
//   fs_fxaa         FXAA: one pass. An edge-direction and end-of-edge search along the edge, then a sub-pixel shift
//                   of the sample. Written from the published algorithm description (Timothy Lottes, "FXAA", 2009
//                   and the 3.11 notes), not from the reference source.
//   fs_smaa_edge    SMAA 1x, pass 1: luma edge detection (threshold 0.1, local contrast adaptation) into RG8.
//   fs_smaa_weights SMAA 1x, pass 2: blending weights from the edges, with the diagonal and corner handling and the
//                   precomputed area and search textures, into RGBA8.
//   fs_smaa_blend   SMAA 1x, pass 3: the neighborhood blend, a bilinear fetch shifted by the weights.
//   fs_cas          Contrast adaptive sharpening (the published CAS algorithm: per channel min/max of a plus-shaped
//                   neighborhood sets the amount, so flat areas and already-contrasty edges barely change).
//
// SMAA 1x (the passes, the search/area logic and the textures) is a port of SMAA by Jorge Jimenez, Jose I.
// Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez, MIT license: see aa/LICENSE-SMAA.txt. The preset
// is HIGH (threshold 0.1, 16 search steps, 8 diagonal steps, corner rounding 25).

struct Pc {
    a: vec4<f32>,   // x/y 1 / size, z/w size (the intermediate targets, pixels)
    b: vec4<f32>,   // x/y rectangle offset of the destination (pixels), z flags (1 sRGB source, 2 HDR), w CAS peak
    c: vec4<f32>,   // x FXAA sub-pixel amount, y FXAA edge threshold, z FXAA edge threshold floor
};
var<immediate> pc: Pc;

@group(0) @binding(0) var smp: sampler;
@group(0) @binding(1) var tex0: texture_2d<f32>;   // the colour (fxaa, cas, edge, blend), the edges (weights)
@group(0) @binding(2) var tex1: texture_2d<f32>;   // the blend weights (blend)
@group(0) @binding(3) var area_tex: texture_2d<f32>;
@group(0) @binding(4) var search_tex: texture_2d<f32>;

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

fn texel() -> vec2<f32> { return pc.a.xy; }
fn size() -> vec2<f32> { return pc.a.zw; }
fn is_srgb() -> bool { return (u32(pc.b.z) & 1u) != 0u; }
fn is_hdr() -> bool { return (u32(pc.b.z) & 2u) != 0u; }

// The pixel position in the intermediate targets (they start at 0; the destination may sit at an offset).
fn pix(i: VOut) -> vec2<f32> { return i.pos.xy - pc.b.xy; }

fn to_srgb(c: vec3<f32>) -> vec3<f32> {
    return select(1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - 0.055, c * 12.92, c <= vec3<f32>(0.0031308));
}
fn from_srgb(c: vec3<f32>) -> vec3<f32> {
    return select(pow((c + 0.055) / 1.055, vec3<f32>(2.4)), c / 12.92, c <= vec3<f32>(0.04045));
}

// What the eye judges contrast on: the encoded value. HDR targets hold values above 1; compress them so a bright
// edge still registers, identity up to 1.
fn perc(c0: vec3<f32>) -> vec3<f32> {
    var c = max(c0, vec3<f32>(0.0));
    if (is_hdr()) {
        c = min(c, vec3<f32>(1.0)) + 0.25 * log2(1.0 + max(c - 1.0, vec3<f32>(0.0)));
    }
    if (is_srgb()) {
        c = to_srgb(min(c, vec3<f32>(1.0)));
    }
    return c;
}

fn luma(c: vec3<f32>) -> f32 {
    return dot(perc(c), vec3<f32>(0.299, 0.587, 0.114));
}

fn ld(t: texture_2d<f32>, p: vec2<i32>) -> vec4<f32> {
    let hi = vec2<i32>(size()) - vec2<i32>(1);
    return textureLoad(t, clamp(p, vec2<i32>(0), hi), 0);
}

// ---------------------------------------------------------------------------------------------------------------
// FXAA

fn fx_luma(uv: vec2<f32>) -> f32 {
    return luma(textureSampleLevel(tex0, smp, uv, 0.0).rgb);
}

const FX_STEPS = array<f32, 12>(1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0);

@fragment
fn fs_fxaa(i: VOut) -> @location(0) vec4<f32> {
    let t = texel();
    let uv = pix(i) * t;
    let p = vec2<i32>(pix(i));
    let cm = ld(tex0, p);
    let lm = luma(cm.rgb);
    let ln = luma(ld(tex0, p + vec2<i32>(0, -1)).rgb);
    let ls = luma(ld(tex0, p + vec2<i32>(0, 1)).rgb);
    let lw = luma(ld(tex0, p + vec2<i32>(-1, 0)).rgb);
    let le = luma(ld(tex0, p + vec2<i32>(1, 0)).rgb);
    let hi = max(max(max(ln, ls), max(lw, le)), lm);
    let lo = min(min(min(ln, ls), min(lw, le)), lm);
    let range = hi - lo;
    if (range < max(pc.c.z, hi * pc.c.y)) {
        return cm;
    }
    let lnw = luma(ld(tex0, p + vec2<i32>(-1, -1)).rgb);
    let lne = luma(ld(tex0, p + vec2<i32>(1, -1)).rgb);
    let lsw = luma(ld(tex0, p + vec2<i32>(-1, 1)).rgb);
    let lse = luma(ld(tex0, p + vec2<i32>(1, 1)).rgb);

    // Which way the edge runs: compare the second derivative across a row and down a column.
    let e_h = abs(-2.0 * ln + lnw + lne) + 2.0 * abs(-2.0 * lm + lw + le) + abs(-2.0 * ls + lsw + lse);
    let e_v = abs(-2.0 * lw + lnw + lsw) + 2.0 * abs(-2.0 * lm + ln + ls) + abs(-2.0 * le + lne + lse);
    let horiz = e_h >= e_v;

    // The side of the pixel across the edge with the steeper gradient.
    let l1 = select(lw, ln, horiz);
    let l2 = select(le, ls, horiz);
    let g1 = l1 - lm;
    let g2 = l2 - lm;
    let first = abs(g1) >= abs(g2);
    let grad = 0.25 * max(abs(g1), abs(g2));
    var step_len = select(t.x, t.y, horiz);
    var avg = 0.5 * (l2 + lm);
    if (first) {
        step_len = -step_len;
        avg = 0.5 * (l1 + lm);
    }
    var cur = uv;
    if (horiz) { cur.y += step_len * 0.5; } else { cur.x += step_len * 0.5; }
    let along = select(vec2<f32>(0.0, t.y), vec2<f32>(t.x, 0.0), horiz);
    var uv1 = cur - along;
    var uv2 = cur + along;
    var end1 = fx_luma(uv1) - avg;
    var end2 = fx_luma(uv2) - avg;
    var done1 = abs(end1) >= grad;
    var done2 = abs(end2) >= grad;
    for (var k = 0u; k < 12u; k++) {
        if (done1 && done2) { break; }
        let s = FX_STEPS[k];
        if (!done1) {
            uv1 -= along * s;
            end1 = fx_luma(uv1) - avg;
            done1 = abs(end1) >= grad;
        }
        if (!done2) {
            uv2 += along * s;
            end2 = fx_luma(uv2) - avg;
            done2 = abs(end2) >= grad;
        }
    }
    let d1 = select(uv.y - uv1.y, uv.x - uv1.x, horiz);
    let d2 = select(uv2.y - uv.y, uv2.x - uv.x, horiz);
    let near1 = d1 < d2;
    let dist = min(d1, d2);
    let len = d1 + d2;
    // Shift toward the edge's far side only when the end of the edge agrees with the pixel's side.
    let center_dark = lm < avg;
    let end = select(end2, end1, near1);
    let right = (end < 0.0) != center_dark;
    var shift = select(0.0, 0.5 - dist / len, right);

    // Sub-pixel aliasing (isolated lines, specks): how far the pixel is from its 3x3 neighborhood's average.
    let avg3 = (2.0 * (ln + ls + lw + le) + (lnw + lsw) + (lne + lse)) * (1.0 / 12.0);
    let sub = clamp(abs(avg3 - lm) / max(range, 1e-5), 0.0, 1.0);
    let sub2 = (-2.0 * sub + 3.0) * sub * sub;
    shift = max(shift, sub2 * sub2 * pc.c.x);

    var fin = uv;
    if (horiz) { fin.y += shift * step_len; } else { fin.x += shift * step_len; }
    return vec4<f32>(textureSampleLevel(tex0, smp, fin, 0.0).rgb, cm.a);
}

// ---------------------------------------------------------------------------------------------------------------
// CAS

@fragment
fn fs_cas(i: VOut) -> @location(0) vec4<f32> {
    let p = vec2<i32>(pix(i));
    let hdr = is_hdr();
    let sr = is_srgb();
    // Work on encoded values: the sRGB case converts on the way in and out.
    var a = ld(tex0, p + vec2<i32>(-1, -1)).rgb;
    var b = ld(tex0, p + vec2<i32>(0, -1)).rgb;
    var c = ld(tex0, p + vec2<i32>(1, -1)).rgb;
    var d = ld(tex0, p + vec2<i32>(-1, 0)).rgb;
    let e0 = ld(tex0, p);
    var e = e0.rgb;
    var f = ld(tex0, p + vec2<i32>(1, 0)).rgb;
    var g = ld(tex0, p + vec2<i32>(-1, 1)).rgb;
    var h = ld(tex0, p + vec2<i32>(0, 1)).rgb;
    var k = ld(tex0, p + vec2<i32>(1, 1)).rgb;
    if (sr) {
        a = to_srgb(a); b = to_srgb(b); c = to_srgb(c); d = to_srgb(d); e = to_srgb(e);
        f = to_srgb(f); g = to_srgb(g); h = to_srgb(h); k = to_srgb(k);
    }
    // The plus-shaped neighborhood sets the range; the corners widen it (the "soft" min/max of the published CAS).
    var mn = min(min(min(d, e), min(f, b)), h);
    var mx = max(max(max(d, e), max(f, b)), h);
    let mn2 = min(min(a, c), min(g, k));
    let mx2 = max(max(a, c), max(g, k));
    mn = mn + min(mn, mn2);
    mx = mx + max(mx, mx2);
    // amount = sqrt(headroom to the nearer of 0 and 1, relative to the peak); 0 where there is no headroom.
    let rcp = 1.0 / max(mx, vec3<f32>(1e-5));
    let amp = sqrt(clamp(min(mn, vec3<f32>(2.0) - mx) * rcp, vec3<f32>(0.0), vec3<f32>(1.0)));
    let w = amp * pc.b.w;
    var o = ((b + d + f + h) * w + e) / (vec3<f32>(1.0) + 4.0 * w);
    if (!hdr) {
        o = clamp(o, vec3<f32>(0.0), vec3<f32>(1.0));
    }
    if (sr) {
        o = from_srgb(o);
    }
    return vec4<f32>(o, e0.a);
}

// ---------------------------------------------------------------------------------------------------------------
// SMAA 1x

const SMAA_THRESHOLD = 0.1;
const SMAA_LCA = 2.0;
const SMAA_STEPS = 16.0;
const SMAA_DIAG_STEPS = 8.0;
const SMAA_CORNER = 0.25;
const AREA_MAX_DIST = 16.0;
const AREA_MAX_DIST_DIAG = 20.0;
const AREA_PIXEL = vec2<f32>(1.0 / 160.0, 1.0 / 560.0);
const AREA_SUBTEX = 1.0 / 7.0;
const SEARCH_SIZE = vec2<f32>(66.0, 33.0);
const SEARCH_PACKED = vec2<f32>(64.0, 16.0);

fn sm_luma(p: vec2<i32>) -> f32 {
    return dot(perc(ld(tex0, p).rgb), vec3<f32>(0.2126, 0.7152, 0.0722));
}

@fragment
fn fs_smaa_edge(i: VOut) -> @location(0) vec4<f32> {
    let p = vec2<i32>(pix(i));
    let l = sm_luma(p);
    let l_left = sm_luma(p + vec2<i32>(-1, 0));
    let l_top = sm_luma(p + vec2<i32>(0, -1));
    var delta = vec4<f32>(0.0);
    delta.x = abs(l - l_left);
    delta.y = abs(l - l_top);
    var edges = step(vec2<f32>(SMAA_THRESHOLD), delta.xy);
    if (dot(edges, vec2<f32>(1.0)) == 0.0) {
        discard;
    }
    let l_right = sm_luma(p + vec2<i32>(1, 0));
    let l_bottom = sm_luma(p + vec2<i32>(0, 1));
    delta.z = abs(l - l_right);
    delta.w = abs(l - l_bottom);
    var max_delta = max(delta.xy, delta.zw);
    let l_leftleft = sm_luma(p + vec2<i32>(-2, 0));
    let l_toptop = sm_luma(p + vec2<i32>(0, -2));
    delta.z = abs(l_left - l_leftleft);
    delta.w = abs(l_top - l_toptop);
    max_delta = max(max_delta, delta.zw);
    let final_delta = max(max_delta.x, max_delta.y);
    edges = edges * step(vec2<f32>(final_delta), SMAA_LCA * delta.xy);
    return vec4<f32>(edges, 0.0, 0.0);
}

// Edges texture, bilinear (the search reads two pixels per fetch). `o` is a texel offset.
fn se(uv: vec2<f32>) -> vec2<f32> {
    return textureSampleLevel(tex0, smp, uv, 0.0).rg;
}
fn seo(uv: vec2<f32>, o: vec2<i32>) -> vec2<f32> {
    return se(uv + vec2<f32>(o) * texel());
}

fn decode_diag2(e: vec2<f32>) -> vec2<f32> {
    return round(vec2<f32>(e.r * abs(5.0 * e.r - 5.0 * 0.75), e.g));
}
fn decode_diag4(e: vec4<f32>) -> vec4<f32> {
    return round(vec4<f32>(e.r * abs(5.0 * e.r - 5.0 * 0.75), e.g, e.b * abs(5.0 * e.b - 5.0 * 0.75), e.a));
}

// Returns (distance, edge flag, e.r, e.g) in .xy / .zw: coord.z, coord.w and the last edges read.
struct Diag { z: f32, w: f32, e: vec2<f32> };

fn search_diag1(tc: vec2<f32>, dir: vec2<f32>) -> Diag {
    var coord = vec4<f32>(tc, -1.0, 1.0);
    var e = vec2<f32>(0.0);
    loop {
        if (!(coord.z < SMAA_DIAG_STEPS - 1.0 && coord.w > 0.9)) { break; }
        let n = coord.xyz + vec3<f32>(texel() * dir, 1.0);
        coord = vec4<f32>(n, coord.w);
        e = se(coord.xy);
        coord.w = dot(e, vec2<f32>(0.5));
    }
    return Diag(coord.z, coord.w, e);
}

fn search_diag2(tc: vec2<f32>, dir: vec2<f32>) -> Diag {
    var coord = vec4<f32>(tc, -1.0, 1.0);
    coord.x += 0.25 * texel().x;
    var e = vec2<f32>(0.0);
    loop {
        if (!(coord.z < SMAA_DIAG_STEPS - 1.0 && coord.w > 0.9)) { break; }
        let n = coord.xyz + vec3<f32>(texel() * dir, 1.0);
        coord = vec4<f32>(n, coord.w);
        e = decode_diag2(se(coord.xy));
        coord.w = dot(e, vec2<f32>(0.5));
    }
    return Diag(coord.z, coord.w, e);
}

fn area_diag(dist: vec2<f32>, e: vec2<f32>) -> vec2<f32> {
    var tc = AREA_MAX_DIST_DIAG * e + dist;
    tc = AREA_PIXEL * tc + 0.5 * AREA_PIXEL;
    tc.x += 0.5;
    // subsample index 0 (SMAA 1x): no vertical shift
    return textureSampleLevel(area_tex, smp, tc, 0.0).rg;
}

fn diag_weights(tc: vec2<f32>, e: vec2<f32>) -> vec2<f32> {
    var weights = vec2<f32>(0.0);
    var d = vec4<f32>(0.0);
    var end = vec2<f32>(0.0);
    if (e.r > 0.0) {
        let s = search_diag1(tc, vec2<f32>(-1.0, 1.0));
        d.x = s.z + select(0.0, 1.0, s.e.y > 0.9);
        d.z = s.w;
    }
    let s2 = search_diag1(tc, vec2<f32>(1.0, -1.0));
    d.y = s2.z;
    d.w = s2.w;

    if (d.x + d.y > 2.0) {
        let coords = vec4<f32>(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * texel().xyxy + tc.xyxy;
        var c = vec4<f32>(0.0);
        c = vec4<f32>(seo(coords.xy, vec2<i32>(-1, 0)), seo(coords.zw, vec2<i32>(1, 0)));
        c = decode_diag4(c).yxwz;   // c.yxwz = decode(c.xyzw)
        var cc = 2.0 * c.xz + c.yw;
        if (step(0.9, d.z) > 0.5) { cc.x = 0.0; }
        if (step(0.9, d.w) > 0.5) { cc.y = 0.0; }
        weights += area_diag(d.xy, cc);
    }

    let s3 = search_diag2(tc, vec2<f32>(-1.0, -1.0));
    d.x = s3.z;
    d.z = s3.w;
    if (seo(tc, vec2<i32>(1, 0)).r > 0.0) {
        let s4 = search_diag2(tc, vec2<f32>(1.0, 1.0));
        d.y = s4.z + select(0.0, 1.0, s4.e.y > 0.9);
        d.w = s4.w;
    } else {
        d.y = 0.0;
        d.w = 0.0;
    }
    if (d.x + d.y > 2.0) {
        let coords = vec4<f32>(-d.x, -d.x, d.y, d.y) * texel().xyxy + tc.xyxy;
        var c = vec4<f32>(0.0);
        c.x = seo(coords.xy, vec2<i32>(-1, 0)).g;
        c.y = seo(coords.xy, vec2<i32>(0, -1)).r;
        let zw = seo(coords.zw, vec2<i32>(1, 0)).gr;
        c.z = zw.x;
        c.w = zw.y;
        var cc = 2.0 * c.xz + c.yw;
        if (step(0.9, d.z) > 0.5) { cc.x = 0.0; }
        if (step(0.9, d.w) > 0.5) { cc.y = 0.0; }
        weights += area_diag(d.xy, cc).gr;
    }
    return weights;
}

fn search_length(e: vec2<f32>, offset: f32) -> f32 {
    var scale = SEARCH_SIZE * vec2<f32>(0.5, -1.0);
    var bias = SEARCH_SIZE * vec2<f32>(offset, 1.0);
    scale += vec2<f32>(-1.0, 1.0);
    bias += vec2<f32>(0.5, -0.5);
    scale *= 1.0 / SEARCH_PACKED;
    bias *= 1.0 / SEARCH_PACKED;
    return textureSampleLevel(search_tex, smp, scale * e + bias, 0.0).r;
}

fn search_x_left(tc0: vec2<f32>, end: f32) -> f32 {
    var tc = tc0;
    var e = vec2<f32>(0.0, 1.0);
    loop {
        if (!(tc.x > end && e.g > 0.8281 && e.r == 0.0)) { break; }
        e = se(tc);
        tc = tc - vec2<f32>(2.0, 0.0) * texel();
    }
    let offset = -(255.0 / 127.0) * search_length(e, 0.0) + 3.25;
    return texel().x * offset + tc.x;
}

fn search_x_right(tc0: vec2<f32>, end: f32) -> f32 {
    var tc = tc0;
    var e = vec2<f32>(0.0, 1.0);
    loop {
        if (!(tc.x < end && e.g > 0.8281 && e.r == 0.0)) { break; }
        e = se(tc);
        tc = tc + vec2<f32>(2.0, 0.0) * texel();
    }
    let offset = -(255.0 / 127.0) * search_length(e, 0.5) + 3.25;
    return -texel().x * offset + tc.x;
}

fn search_y_up(tc0: vec2<f32>, end: f32) -> f32 {
    var tc = tc0;
    var e = vec2<f32>(1.0, 0.0);
    loop {
        if (!(tc.y > end && e.r > 0.8281 && e.g == 0.0)) { break; }
        e = se(tc);
        tc = tc - vec2<f32>(0.0, 2.0) * texel();
    }
    let offset = -(255.0 / 127.0) * search_length(e.gr, 0.0) + 3.25;
    return texel().y * offset + tc.y;
}

fn search_y_down(tc0: vec2<f32>, end: f32) -> f32 {
    var tc = tc0;
    var e = vec2<f32>(1.0, 0.0);
    loop {
        if (!(tc.y < end && e.r > 0.8281 && e.g == 0.0)) { break; }
        e = se(tc);
        tc = tc + vec2<f32>(0.0, 2.0) * texel();
    }
    let offset = -(255.0 / 127.0) * search_length(e.gr, 0.5) + 3.25;
    return -texel().y * offset + tc.y;
}

fn area(dist: vec2<f32>, e1: f32, e2: f32) -> vec2<f32> {
    var tc = AREA_MAX_DIST * round(4.0 * vec2<f32>(e1, e2)) + dist;
    tc = AREA_PIXEL * tc + 0.5 * AREA_PIXEL;
    // subsample index 0 (SMAA 1x): no vertical shift
    return textureSampleLevel(area_tex, smp, tc, 0.0).rg;
}

fn corner_h(w0: vec2<f32>, tc: vec4<f32>, d: vec2<f32>) -> vec2<f32> {
    let left_right = step(d.xy, d.yx);
    var rounding = (1.0 - SMAA_CORNER) * left_right;
    rounding = rounding / (left_right.x + left_right.y);
    var factor = vec2<f32>(1.0);
    factor.x -= rounding.x * seo(tc.xy, vec2<i32>(0, 1)).r;
    factor.x -= rounding.y * seo(tc.zw, vec2<i32>(1, 1)).r;
    factor.y -= rounding.x * seo(tc.xy, vec2<i32>(0, -2)).r;
    factor.y -= rounding.y * seo(tc.zw, vec2<i32>(1, -2)).r;
    return w0 * clamp(factor, vec2<f32>(0.0), vec2<f32>(1.0));
}

fn corner_v(w0: vec2<f32>, tc: vec4<f32>, d: vec2<f32>) -> vec2<f32> {
    let left_right = step(d.xy, d.yx);
    var rounding = (1.0 - SMAA_CORNER) * left_right;
    rounding = rounding / (left_right.x + left_right.y);
    var factor = vec2<f32>(1.0);
    factor.x -= rounding.x * seo(tc.xy, vec2<i32>(1, 0)).g;
    factor.x -= rounding.y * seo(tc.zw, vec2<i32>(1, 1)).g;
    factor.y -= rounding.x * seo(tc.xy, vec2<i32>(-2, 0)).g;
    factor.y -= rounding.y * seo(tc.zw, vec2<i32>(-2, 1)).g;
    return w0 * clamp(factor, vec2<f32>(0.0), vec2<f32>(1.0));
}

@fragment
fn fs_smaa_weights(i: VOut) -> @location(0) vec4<f32> {
    let t = texel();
    let tc = pix(i) * t;
    let pixcoord = tc * size();
    let o0 = tc.xyxy + t.xyxy * vec4<f32>(-0.25, -0.125, 1.25, -0.125);
    let o1 = tc.xyxy + t.xyxy * vec4<f32>(-0.125, -0.25, -0.125, 1.25);
    let o2 = vec4<f32>(o0.x, o0.z, o1.y, o1.w) + vec4<f32>(t.x, t.x, t.y, t.y) * vec4<f32>(-2.0, 2.0, -2.0, 2.0) * SMAA_STEPS;

    var weights = vec4<f32>(0.0);
    var e = se(tc);

    if (e.g > 0.0) {
        // Edge at north.
        let dw = diag_weights(tc, e);
        weights = vec4<f32>(dw, weights.b, weights.a);
        if (weights.r == -weights.g) {
            var d = vec2<f32>(0.0);
            var coords = vec3<f32>(0.0);
            coords.x = search_x_left(o0.xy, o2.x);
            coords.y = o1.y;
            d.x = coords.x;
            let e1 = se(coords.xy).r;
            coords.z = search_x_right(o0.zw, o2.y);
            d.y = coords.z;
            d = abs(round(size().x * d - vec2<f32>(pixcoord.x)));
            let sqrt_d = sqrt(d);
            let e2 = seo(coords.zy, vec2<i32>(1, 0)).r;
            var w = area(sqrt_d, e1, e2);
            coords.y = tc.y;
            w = corner_h(w, vec4<f32>(coords.x, coords.y, coords.z, coords.y), d);
            weights = vec4<f32>(w, weights.b, weights.a);
        } else {
            e.r = 0.0;
        }
    }

    if (e.r > 0.0) {
        // Edge at west.
        var d = vec2<f32>(0.0);
        var coords = vec3<f32>(0.0);
        coords.y = search_y_up(o1.xy, o2.z);
        coords.x = o0.x;
        d.x = coords.y;
        let e1 = se(coords.xy).g;
        coords.z = search_y_down(o1.zw, o2.w);
        d.y = coords.z;
        d = abs(round(size().y * d - vec2<f32>(pixcoord.y)));
        let sqrt_d = sqrt(d);
        let e2 = seo(coords.xz, vec2<i32>(0, 1)).g;
        var w = area(sqrt_d, e1, e2);
        coords.x = tc.x;
        w = corner_v(w, vec4<f32>(coords.x, coords.y, coords.x, coords.z), d);
        weights = vec4<f32>(weights.r, weights.g, w);
    }
    return weights;
}

fn sb(uv: vec2<f32>) -> vec4<f32> {
    return textureSampleLevel(tex1, smp, uv, 0.0);
}

fn sc(uv: vec2<f32>) -> vec4<f32> {
    return textureSampleLevel(tex0, smp, uv, 0.0);
}

@fragment
fn fs_smaa_blend(i: VOut) -> @location(0) vec4<f32> {
    let t = texel();
    let tc = pix(i) * t;
    let offset = tc.xyxy + t.xyxy * vec4<f32>(1.0, 0.0, 0.0, 1.0);
    var a = vec4<f32>(0.0);
    a.x = sb(offset.xy).a;   // right
    a.y = sb(offset.zw).g;   // top
    let bl = sb(tc);
    a.w = bl.x;              // bottom
    a.z = bl.z;              // left
    if (dot(a, vec4<f32>(1.0)) < 1e-5) {
        return sc(tc);
    }
    let h = max(a.x, a.z) > max(a.y, a.w);
    var blending_offset = vec4<f32>(0.0, a.y, 0.0, a.w);
    var blending_weight = a.yw;
    if (h) {
        blending_offset = vec4<f32>(a.x, 0.0, a.z, 0.0);
        blending_weight = a.xz;
    }
    blending_weight = blending_weight / dot(blending_weight, vec2<f32>(1.0));
    let blending_coord = blending_offset * vec4<f32>(t, -t) + tc.xyxy;
    var color = blending_weight.x * sc(blending_coord.xy);
    color += blending_weight.y * sc(blending_coord.zw);
    return color;
}
