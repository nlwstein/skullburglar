//! Same-frame A/B parity: the renderer records one frame twice, with variant A and then variant B of a feature, into
//! two offscreen targets in one process, and compares them (`vkr/ab.rs`). Both renders use the identical frame state, so
//! it works at any point in normal play; a shader or pass rewrite is proven identical (or its difference measured)
//! without a deterministic scene, which live pose tours can't give (same-build runs differ by up to 253/255).
//!
//! This file is the part that needs no GPU: the environment switches, the shader hooks, the image diff and a PNG writer.
//!
//! Switches (read once, at the first use):
//! - `BOTS_N1_AB=<feature>[,<feature>...]` turns the mechanism on. A renders with the feature's alternative path (the
//!   old code), B with the shipping path. Features: see `FEATURES`. `none` is the self-test A vs A (must be exactly 0).
//! - `BOTS_N1_AB_EVERY=<n>` runs it every n-th scene frame (default 0: only on request).
//! - On request: `hybrid n1debug 16777216` (debug bit 24) captures one frame on its rising edge; set the bits back to 0
//!   to arm it again.
//! - `BOTS_N1_AB_DIR=<dir>` writes `ab.csv` (one row per compared frame) and, for a frame that differs (or every frame
//!   with `BOTS_N1_AB_SAVE=all`), `abNNNN-a.png`, `-b.png` and `-diff.png` (|A-B| x 64 per channel, so 1/255 shows).
//!
//! Off (no `BOTS_N1_AB`), nothing here changes a shader, a pipeline layout or a frame: the hook tokens in the WGSL
//! become empty text, no push-constant range is added and `record` takes its single-pass path.
//!
//! To add a pair: give the feature a bit in `FEATURES`, add the alternative code to `scene_decls`/`post_decls`, put a
//! hook token where the shader should branch (`{{AB_...}}` in scene.wgsl or post.wgsl, replaced in `scene_text` or
//! `post_text`), and read `ab_pc.flags` there. A is rendered with the feature's bit set, B with it clear.

use std::path::PathBuf;
use std::sync::OnceLock;

/// Debug bit (`hybrid n1debug <bits>`): a rising edge captures one A/B frame.
pub const TRIGGER_BIT: u32 = 1 << 24;

/// (name, flag bit). Bit set = the alternative (old / probe) path; clear = the shipping path.
pub const FEATURES: &[(&str, u32)] = &[
    // !189: the branch-free PWL gamma decode (B) against the per-segment select form it replaced (A).
    ("pwl", 1),
    // Self-test: A adds one 8-bit step to the red output channel. Must be detected, as exactly 1 where it shows.
    ("lsb", 2),
    // Self-test: A's decode is the old form plus 4/1023, so the hook is proven to reach the scene's pixels.
    ("pwlprobe", 4),
    // Self-test: A runs a verbatim copy of the shipping decode (`xe_pwl3_copy`), in its own arm of the hook. Must be 0: if it
    // isn't, a difference between A and B can come from the hook's arms and not from the code under test.
    ("pwlcopy", 8),
    // Diagnostic: A shows where, in the real shader, the 1.18.0 decode and the shipping one differ at all: such a texel comes out
    // magenta (its G channel 0, R and B far above 1). B is the normal picture.
    ("pwlmark", 16),
    // Self-test: a verbatim copy of the 1.18.0 decode in its own arm; with BOTS_N1_AB=pwl BOTS_N1_AB_B=pwllegacy2 it is the old
    // decode against itself through two arms (any difference is the compiler treating the arms differently, not the decode).
    ("pwllegacy2", 32),
    // gate2: the decode of !189 (before the bit-exact change), to time it against the shipping one in one process.
    ("pwl189", 64),
    // gate2 timing self-test: A adds a known ALU loop (`BOTS_N1_AB_ALU` iterations per decode call) to the decode, output
    // unchanged. The loop count rides in the flag word's upper 16 bits (`ALU_SHIFT`).
    ("alu", 128),
    // t720e: A draws the post chain with the previous post.wgsl (`post_legacy.wgsl`: native HDR code in the SDR shaders, the
    // god-ray pass before its rewrite) through its own pipelines; B is the shipping post.wgsl. Pipeline-level, not a hook.
    ("post", 256),
    // Self-test for the pipeline-level features: A adds one 8-bit step to red in the PREVIOUS post.wgsl only (`BOTS_N1_AB=post,postprobe`),
    // so it shows exactly when A really draws with the previous post.wgsl's pipelines (the shipping text has no such hook).
    ("postprobe", 1024),
];

/// Flag bit of feature `postprobe`, read only by the previous post.wgsl (`post_legacy_text`).
pub const POST_PROBE: u32 = 1024;

/// Flag bit of feature `post`: the variant uses the previous post.wgsl's pipelines.
pub const POST_LEGACY: u32 = 256;

/// The ALU loop count of feature `alu` sits in the flag word above this bit.
pub const ALU_SHIFT: u32 = 16;

pub struct Config {
    pub names: String,
    /// Flags of variant A (the alternatives); B is always 0.
    pub flags_a: u32,
    /// Flags of variant B (default 0, the shipping path): `BOTS_N1_AB_B=<feature>[,..]` puts a feature's bit on B too, to
    /// compare two alternatives with each other (`pwl` against `pwllegacy2` is the old decode against a verbatim copy of it).
    pub flags_b: u32,
    pub every: u64,
    pub dir: Option<PathBuf>,
    pub save_all: bool,
    /// `BOTS_N1_AB_TIME=1`: time A and B on the GPU every frame (timestamps around each variant's scene+post), no readback and
    /// no compare, and log the pairs (read by `ops/bench/gate2/abtime.py`). `every` then defaults to 1.
    pub time: bool,
    /// `BOTS_N1_AB_SHIP=<feature>[,..]`: flags of the ordinary (shipping) passes, the ones the player sees and the GPU profile times.
    /// With `alu` (and `BOTS_N1_AB_ALU`) every frame carries a known extra cost, the gate's +3 % self-test; 0 (default) is the shipping path.
    pub flags_ship: u32,
}

static CONFIG: OnceLock<Option<Config>> = OnceLock::new();

/// The parsed switches, or `None` when `BOTS_N1_AB` is unset or names nothing valid.
pub fn config() -> Option<&'static Config> {
    CONFIG.get_or_init(|| parse(std::env::var("BOTS_N1_AB").ok().as_deref(), |k| std::env::var(k).ok())).as_ref()
}

pub fn enabled() -> bool {
    config().is_some()
}

fn parse(spec: Option<&str>, env: impl Fn(&str) -> Option<String>) -> Option<Config> {
    let spec = spec?.trim();
    if spec.is_empty() || spec == "0" {
        return None;
    }
    let mut flags = 0;
    for name in spec.split(',').map(str::trim).filter(|n| !n.is_empty()) {
        if name == "none" {
            continue;
        }
        match FEATURES.iter().find(|f| f.0 == name) {
            Some(f) => flags |= f.1,
            None => {
                eprintln!("n1ffi: BOTS_N1_AB: unknown feature {name:?} (have: none, {}); A/B off", FEATURES.iter().map(|f| f.0).collect::<Vec<_>>().join(", "));
                return None;
            }
        }
    }
    let flags_b = env("BOTS_N1_AB_B").map_or(0, |v| {
        v.split(',').map(str::trim).filter_map(|n| FEATURES.iter().find(|f| f.0 == n)).fold(0, |a, f| a | f.1)
    });
    let time = env("BOTS_N1_AB_TIME").is_some_and(|v| !v.is_empty() && v != "0");
    let alu: u32 = env("BOTS_N1_AB_ALU").and_then(|v| v.trim().parse().ok()).unwrap_or(0).min(0xFFFF);
    let with_alu = |f: u32| if f & 128 != 0 { f | (alu << ALU_SHIFT) } else { f };
    Some(Config {
        names: spec.to_string(),
        flags_a: with_alu(flags),
        flags_b: with_alu(flags_b),
        flags_ship: with_alu(env("BOTS_N1_AB_SHIP").map_or(0, |v| {
            v.split(',').map(str::trim).filter_map(|n| FEATURES.iter().find(|f| f.0 == n)).fold(0, |a, f| a | f.1)
        })),
        time,
        every: env("BOTS_N1_AB_EVERY").and_then(|v| v.trim().parse().ok()).unwrap_or(u64::from(time)),
        dir: env("BOTS_N1_AB_DIR").filter(|d| !d.is_empty()).map(PathBuf::from),
        save_all: env("BOTS_N1_AB_SAVE").is_some_and(|v| v == "all"),
    })
}

// ---- shader hooks

const PC_DECL: &str = "struct AbPc { flags: u32 }\nvar<immediate> ab_pc: AbPc;\n";

/// The per-segment select decode !189 replaced (kept here, not in the shader, so a normal build doesn't carry it).
pub const PWL_LEGACY: &str = "fn xe_pwl3_legacy(c: vec3<f32>) -> vec3<f32> {
    let g = clamp(c, vec3<f32>(0.0), vec3<f32>(1.0)) * 255.0;
    let hi = g >= vec3<f32>(96.0);
    let k = select(select(vec3<f32>(1.0), vec3<f32>(2.0), g >= vec3<f32>(64.0)), select(vec3<f32>(4.0), vec3<f32>(8.0), g >= vec3<f32>(192.0)), hi);
    let o = select(select(vec3<f32>(0.0), vec3<f32>(-64.0), g >= vec3<f32>(64.0)), select(vec3<f32>(-256.0), vec3<f32>(-1024.0), g >= vec3<f32>(192.0)), hi);
    let l = g * k + o;
    return (l + trunc(l * k * (1.0 / 1024.0))) * (1.0 / 1023.0);
}
";

/// !189's decode (branch-free, before the bit-exact change), for timing against the shipping one.
pub const PWL_189: &str = "fn xe_pwl3_189(c: vec3<f32>) -> vec3<f32> {
    let s = saturate(c);
    let l = max(max(s * 255.0, s * 510.0 - 64.0), max(s * 1020.0 - 256.0, s * 2040.0 - 1024.0));
    let t = floor(l * select(vec3<f32>(1.0 / 256.0), vec3<f32>(1.0 / 128.0), l >= vec3<f32>(512.0)));
    return (l + t) * (1.0 / 1023.0);
}
";

/// The shipping decode plus a dependent ALU loop of `flags >> 16` iterations; the branch on its result never fires, so the
/// loop can't be removed and the picture doesn't change.
pub const PWL_ALU: &str = "fn xe_pwl3_alu(c: vec3<f32>) -> vec3<f32> {
    var x = c.x + 0.5;
    let n = ab_pc.flags >> 16u;
    for (var i = 0u; i < n; i = i + 1u) { x = fma(x, 0.999, 0.0013); x = x * 1.0007 + 0.0001; }
    if (x == 123456.0) { return vec3<f32>(1.0); }
    return xe_pwl3_copy(c);
}
";

/// scene.wgsl's tokens, filled in: empty when A/B is off.
pub fn scene_text(src: &str) -> String {
    scene_text_with(src, enabled())
}

pub fn scene_text_with(src: &str, on: bool) -> String {
    if !on {
        return src.replace("{{AB_DECLS}}", "").replace("{{AB_PWL}}", "");
    }
    let copy = src
        .split("fn xe_pwl3(")
        .nth(1)
        .and_then(|s| s.split("\n}\n").next())
        .map(|f| format!("fn xe_pwl3_copy({f}\n}}\n").replace("{{AB_PWL}}", ""))
        .unwrap_or_default();
    src.replace("{{AB_DECLS}}", &format!("{PC_DECL}{PWL_LEGACY}{}{copy}{PWL_189}{PWL_ALU}", PWL_LEGACY.replace("xe_pwl3_legacy", "xe_pwl3_legacy2"))).replace(
        "{{AB_PWL}}",
        "if ((ab_pc.flags & 1u) != 0u) { return xe_pwl3_legacy(c); }\n    if ((ab_pc.flags & 4u) != 0u) { return xe_pwl3_legacy(c) + vec3<f32>(4.0 / 1023.0); }\n    if ((ab_pc.flags & 8u) != 0u) { return xe_pwl3_copy(c); }\n    if ((ab_pc.flags & 16u) != 0u) { let l = xe_pwl3_legacy(c); if (any(bitcast<vec3<u32>>(l) != bitcast<vec3<u32>>(xe_pwl3_copy(c)))) { return vec3<f32>(100.0, 0.0, 100.0); } return l; }\n    if ((ab_pc.flags & 32u) != 0u) { return xe_pwl3_legacy2(c); }\n    if ((ab_pc.flags & 64u) != 0u) { return xe_pwl3_189(c); }\n    if ((ab_pc.flags & 128u) != 0u) { return xe_pwl3_alu(c); }",
    )
}

/// The previous post.wgsl (the A arm of feature `post`): the same hooks, plus the `postprobe` self-test hook that only it has.
pub fn post_legacy_text(src: &str) -> String {
    let t = post_text_with(src, enabled());
    if enabled() {
        t.replacen("    if (p.c.z < 0.5) {\n        // sRGB targets encode on write", "    if ((ab_pc.flags & 1024u) != 0u) { g = min(g + vec3<f32>(1.0 / 255.0, 0.0, 0.0), vec3<f32>(1.0)); }\n    if (p.c.z < 0.5) {\n        // sRGB targets encode on write", 1)
    } else {
        t
    }
}

/// post.wgsl's tokens, filled in: empty when A/B is off.
pub fn post_text(src: &str) -> String {
    post_text_with(src, enabled())
}

pub fn post_text_with(src: &str, on: bool) -> String {
    if !on {
        return src.replace("{{AB_POST_DECLS}}", "").replace("{{AB_POST_OUT}}", "");
    }
    src.replace("{{AB_POST_DECLS}}", PC_DECL).replace(
        "{{AB_POST_OUT}}",
        "if ((ab_pc.flags & 2u) != 0u) { g = min(g + vec3<f32>(1.0 / 255.0, 0.0, 0.0), vec3<f32>(1.0)); }",
    )
}

// ---- the diff

/// How a target's pixels are laid out in a readback buffer.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Layout {
    /// Four bytes per pixel, any channel order (the diff is per byte; the alpha byte is skipped on request).
    Bytes4,
    /// Four half floats per pixel.
    Half4,
}

#[derive(Clone, Copy, Default, Debug, PartialEq)]
pub struct Stats {
    /// Largest per-channel difference, in 8-bit steps (1.0 = one step).
    pub max: f32,
    pub mean: f64,
    /// Pixels where any colour channel differs at all.
    pub differ: u64,
    pub pixels: u64,
    /// Mean of B's colour channels in 8-bit steps: says the frame is not blank.
    pub mean_b: f64,
}

fn half_to_f32(h: u16) -> f32 {
    let (s, e, m) = ((h >> 15) as u32, ((h >> 10) & 31) as i32, (h & 1023) as u32);
    let v = match e {
        0 => m as f32 * (1.0 / 16777216.0),
        31 => if m == 0 { f32::INFINITY } else { f32::NAN },
        _ => (1.0 + m as f32 / 1024.0) * 2f32.powi(e - 15),
    };
    if s == 1 { -v } else { v }
}

/// Compares two tightly packed images of `pixels` pixels (colour channels 0..3 of each; alpha ignored). 8-bit steps.
pub fn compare(layout: Layout, a: &[u8], b: &[u8], pixels: usize) -> Stats {
    let mut st = Stats { pixels: pixels as u64, ..Default::default() };
    let (mut sum, mut sum_b) = (0f64, 0f64);
    let bpp = match layout {
        Layout::Bytes4 => 4,
        Layout::Half4 => 8,
    };
    for px in 0..pixels.min(a.len() / bpp).min(b.len() / bpp) {
        let mut any = false;
        for c in 0..3 {
            let (x, y) = match layout {
                Layout::Bytes4 => (a[px * 4 + c] as f32, b[px * 4 + c] as f32),
                Layout::Half4 => {
                    let at = px * 8 + c * 2;
                    (
                        half_to_f32(u16::from_le_bytes([a[at], a[at + 1]])) * 255.0,
                        half_to_f32(u16::from_le_bytes([b[at], b[at + 1]])) * 255.0,
                    )
                }
            };
            let d = (x - y).abs();
            if d > 0.0 || d.is_nan() {
                any = true;
            }
            if d.is_finite() {
                st.max = st.max.max(d);
                sum += d as f64;
            }
            if y.is_finite() {
                sum_b += y as f64;
            }
        }
        st.differ += any as u64;
    }
    if pixels > 0 {
        st.mean = sum / (pixels as f64 * 3.0);
        st.mean_b = sum_b / (pixels as f64 * 3.0);
    }
    st
}

/// 8-bit RGB of a readback buffer, for the PNGs. `swap`: the target stores blue first.
pub fn to_rgb8(layout: Layout, raw: &[u8], pixels: usize, swap: bool) -> Vec<u8> {
    let mut out = Vec::with_capacity(pixels * 3);
    for px in 0..pixels {
        for c in 0..3 {
            let v = match layout {
                Layout::Bytes4 => raw[px * 4 + if swap { 2 - c } else { c }],
                Layout::Half4 => {
                    let at = px * 8 + c * 2;
                    (half_to_f32(u16::from_le_bytes([raw[at], raw[at + 1]])).clamp(0.0, 1.0) * 255.0 + 0.5) as u8
                }
            };
            out.push(v);
        }
    }
    out
}

/// |A - B| x 64 per channel (clamped), from two RGB8 images.
pub fn diff_rgb8(a: &[u8], b: &[u8]) -> Vec<u8> {
    a.iter().zip(b).map(|(x, y)| (x.abs_diff(*y) as u32 * 64).min(255) as u8).collect()
}

// ---- PNG (stored deflate blocks: no compression, no dependency)

fn crc32(data: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for (n, t) in table.iter_mut().enumerate() {
        let mut c = n as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
        }
        *t = c;
    }
    let mut c = 0xFFFF_FFFFu32;
    for &b in data {
        c = table[((c ^ b as u32) & 0xFF) as usize] ^ (c >> 8);
    }
    c ^ 0xFFFF_FFFF
}

fn adler32(data: &[u8]) -> u32 {
    let (mut a, mut b) = (1u32, 0u32);
    for &x in data {
        a = (a + x as u32) % 65521;
        b = (b + a) % 65521;
    }
    (b << 16) | a
}

pub fn png_rgb8(w: u32, h: u32, rgb: &[u8]) -> Vec<u8> {
    let mut raw = Vec::with_capacity((w as usize * 3 + 1) * h as usize);
    for row in rgb.chunks(w as usize * 3) {
        raw.push(0);
        raw.extend_from_slice(row);
    }
    let mut z = vec![0x78, 0x01];
    let mut chunks = raw.chunks(65535).peekable();
    while let Some(c) = chunks.next() {
        z.push(chunks.peek().is_none() as u8);
        z.extend_from_slice(&(c.len() as u16).to_le_bytes());
        z.extend_from_slice(&(!(c.len() as u16)).to_le_bytes());
        z.extend_from_slice(c);
    }
    z.extend_from_slice(&adler32(&raw).to_be_bytes());
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut chunk = |kind: &[u8; 4], data: &[u8]| {
        out.extend_from_slice(&(data.len() as u32).to_be_bytes());
        let mut body = kind.to_vec();
        body.extend_from_slice(data);
        out.extend_from_slice(&body);
        out.extend_from_slice(&crc32(&body).to_be_bytes());
    };
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&w.to_be_bytes());
    ihdr.extend_from_slice(&h.to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    chunk(b"IHDR", &ihdr);
    chunk(b"IDAT", &z);
    chunk(b"IEND", &[]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn none(_: &str) -> Option<String> {
        None
    }

    #[test]
    fn parses_features_and_rejects_unknown() {
        assert!(parse(None, none).is_none());
        assert!(parse(Some("0"), none).is_none());
        assert!(parse(Some("nonsense"), none).is_none());
        assert_eq!(parse(Some("none"), none).unwrap().flags_a, 0);
        assert_eq!(parse(Some("pwl"), none).unwrap().flags_a, 1);
        assert_eq!(parse(Some("pwl, lsb"), none).unwrap().flags_a, 3);
        let c = parse(Some("pwl"), |k| (k == "BOTS_N1_AB_EVERY").then(|| "90".to_string())).unwrap();
        assert_eq!(c.every, 90);
    }

    #[test]
    fn timing_and_alu_switches() {
        let env = |k: &str| match k {
            "BOTS_N1_AB_TIME" => Some("1".to_string()),
            "BOTS_N1_AB_ALU" => Some("40".to_string()),
            _ => None,
        };
        let c = parse(Some("alu"), env).unwrap();
        assert!(c.time);
        assert_eq!(c.every, 1, "timing runs every frame unless told otherwise");
        assert_eq!(c.flags_a, 128 | (40 << ALU_SHIFT));
        assert_eq!(c.flags_b, 0);
        assert_eq!(c.flags_ship, 0);
        let ship = |k: &str| match k {
            "BOTS_N1_AB_SHIP" => Some("alu".to_string()),
            "BOTS_N1_AB_ALU" => Some("7".to_string()),
            _ => None,
        };
        assert_eq!(parse(Some("alu"), ship).unwrap().flags_ship, 128 | (7 << ALU_SHIFT));
        let c = parse(Some("pwl189"), env).unwrap();
        assert_eq!(c.flags_a, 64, "the loop count only rides with the alu feature");
        assert!(!parse(Some("pwl"), none).unwrap().time);
        assert_eq!(parse(Some("pwl"), none).unwrap().every, 0);
    }

    #[test]
    fn feature_bits_are_distinct_powers_of_two() {
        for (i, a) in FEATURES.iter().enumerate() {
            assert!(a.1.is_power_of_two());
            for b in &FEATURES[i + 1..] {
                assert_ne!(a.1, b.1);
            }
        }
    }

    #[test]
    fn identical_images_diff_zero_and_one_step_is_seen() {
        let a: Vec<u8> = (0..4 * 64).map(|i| (i * 7) as u8).collect();
        let s = compare(Layout::Bytes4, &a, &a, 64);
        assert_eq!((s.max, s.differ), (0.0, 0));
        let mut b = a.clone();
        b[4 * 10 + 1] = b[4 * 10 + 1].wrapping_add(1);
        b[4 * 10 + 3] ^= 0xFF; // alpha is ignored
        let s = compare(Layout::Bytes4, &a, &b, 64);
        assert_eq!((s.max, s.differ), (1.0, 1));
    }

    #[test]
    fn half_floats_compare_in_eight_bit_steps() {
        // 0.5 = 0x3800, 0.5 + 1/2048 = 0x3801 (one half ulp = 1/2048 = 0.125 of an 8-bit step)
        let px = |v: u16| [v.to_le_bytes(), v.to_le_bytes(), v.to_le_bytes(), 0x3C00u16.to_le_bytes()].concat();
        let (a, b) = (px(0x3800), px(0x3801));
        let s = compare(Layout::Half4, &a, &a, 1);
        assert_eq!(s.differ, 0);
        let s = compare(Layout::Half4, &a, &b, 1);
        assert_eq!(s.differ, 1);
        assert!((s.max - 0.1245).abs() < 0.01, "{}", s.max);
    }

    #[test]
    fn png_has_the_right_shape() {
        let p = png_rgb8(3, 2, &[7; 18]);
        assert_eq!(&p[..8], b"\x89PNG\r\n\x1a\n");
        assert_eq!(&p[12..16], b"IHDR");
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
        assert_eq!(adler32(b"Wikipedia"), 0x11E6_0398);
    }

    #[test]
    fn hooks_are_empty_text_when_off() {
        let s = scene_text_with("a {{AB_DECLS}} b {{AB_PWL}} c", false);
        assert_eq!(s, "a  b  c");
        assert_eq!(post_text_with("{{AB_POST_DECLS}}x{{AB_POST_OUT}}", false), "x");
        assert!(scene_text_with("{{AB_PWL}}", true).contains("ab_pc.flags"));
    }

    #[test]
    fn legacy_decode_and_hooks_compile() {
        // The A/B text must stay valid WGSL: assemble the hooks as the enabled build would.
        let scene = matdef::shader::SCENE_WGSL;
        assert!(scene.contains("{{AB_DECLS}}") && scene.contains("{{AB_PWL}}"));
        assert!(include_str!("post.wgsl").contains("{{AB_POST_OUT}}"));
        assert!(PWL_LEGACY.contains("trunc"));
        // The copy is the shipping body, verbatim, and the whole thing is valid WGSL (the copy needs the real scene text).
        let on = scene_text_with(scene, true);
        assert!(on.contains("fn xe_pwl3_copy(") && on.contains("xe_pwl3_copy(c)"));
        let body = |name: &str| on.split(name).nth(1).and_then(|s| s.split("\n}\n").next()).unwrap().replace("{{AB_PWL}}", "");
        let shipping = body("fn xe_pwl3(");
        let copy = body("fn xe_pwl3_copy(");
        assert!(shipping.contains("floor") && copy.contains("floor"));
        assert_eq!(copy.rsplit("let g").next(), shipping.rsplit("let g").next());
    }
}
