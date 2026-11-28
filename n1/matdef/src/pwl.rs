//! Xenos's piecewise-linear gamma-to-linear curve (B-235, B-392): the reference the scene shader's `xe_pwl3`
//! (`scene.wgsl`) has to equal, and the tests that pin it to the ReXGlue SDK's `PWLGammaToLinear`
//! (`src/graphics/xenos.cpp`; the shader translators emit the same arithmetic).

/// The SDK's `PWLGammaToLinear`, line for line. `gamma` is the stored value 0..1 (a filtered 8-bit texel).
pub fn sdk_pwl(gamma: f32) -> f32 {
    let gamma = if gamma.is_nan() { 0.0 } else { gamma.clamp(0.0, 1.0) };
    let (scale, offset) = if gamma >= 96.0 / 255.0 {
        if gamma >= 192.0 / 255.0 { (8.0 / 1024.0, -1024.0) } else { (4.0 / 1024.0, -256.0) }
    } else if gamma >= 64.0 / 255.0 {
        (2.0 / 1024.0, -64.0)
    } else {
        (1.0 / 1024.0, 0.0)
    };
    let mut linear = gamma * (255.0 * 1024.0 * scale) + offset;
    linear += (linear * scale).trunc();
    linear * (1.0 / 1023.0)
}

/// What `scene.wgsl`'s `xe_pwl3` computes for one channel (kept in step with the WGSL by `wgsl_matches_mirror`): the
/// largest of four lines on the rounded g = 255 c, plus the SDK's extra round-down term. Bit-identical to `segments_pwl`.
pub fn wgsl_pwl(c: f32) -> f32 {
    let g = (c.clamp(0.0, 1.0) * 255.0).min(255.0);
    let l = g.max(g * 2.0 - 64.0).max((g * 4.0 - 256.0).max(g * 8.0 - 1024.0));
    let t = (l * if l >= 512.0 { 1.0 / 128.0 } else { 1.0 / 256.0 }).floor();
    (l + t) * (1.0 / 1023.0)
}

/// The select-per-segment form `xe_pwl3` had in 1.15.0 to 1.18.0 (slower on a laptop GPU): the reference the branch-free
/// `wgsl_pwl` must equal bit for bit.
#[cfg(test)]
pub fn segments_pwl(c: f32) -> f32 {
    let g = c.clamp(0.0, 1.0) * 255.0;
    let (k, o) = if g >= 192.0 {
        (8.0, -1024.0)
    } else if g >= 96.0 {
        (4.0, -256.0)
    } else if g >= 64.0 {
        (2.0, -64.0)
    } else {
        (1.0, 0.0)
    };
    let l = g * k + o;
    (l + (l * k * (1.0 / 1024.0)).trunc()) * (1.0 / 1023.0)
}

/// The curve n1 used before B-392: the largest of four lines (stored value x 255 / 1024 folded in), without the
/// `trunc` term and divided by 1024 instead of 1023, so white came out at 0.992 and the top segment ran up to 0.8% low.
pub fn old_pwl(c: f32) -> f32 {
    (c * 0.2490234375).max(c * 0.498046875 - 0.0625).max((c * 0.99609375 - 0.25).max(c * 1.9921875 - 1.0))
}

#[cfg(test)]
mod tests {
    use super::*;

    const SCENE: &str = include_str!("scene.wgsl");

    #[test]
    fn endpoints_and_joins() {
        assert_eq!(sdk_pwl(0.0), 0.0);
        assert!((sdk_pwl(1.0) - 1.0).abs() < 1e-6, "white maps to 1.0 (1023 / 1023), got {}", sdk_pwl(1.0));
        // The four segments of the SDK (stored values): 0..64 -> g, 64..96 -> 2g - 64, 96..192 -> 4g - 256, 192..255 ->
        // 8g - 1024, out of 1023 plus the trunc term. Known stored values:
        for (g, lin) in [(0.0, 0.0), (32.0, 32.0), (64.0, 64.0), (80.0, 96.0), (96.0, 128.0), (128.0, 256.0 + 1.0), (192.0, 512.0 + 4.0), (255.0, 1016.0 + 7.0)] {
            let got = sdk_pwl(g / 255.0) * 1023.0;
            assert!((got - lin).abs() < 1e-2, "stored {g}: {got} vs {lin}");
        }
    }

    #[test]
    fn monotonic_over_the_whole_range() {
        let mut last = -1.0;
        for k in 0..=8160 {
            let v = sdk_pwl(k as f32 / 8160.0);
            assert!(v >= last, "k {k}: {v} < {last}");
            last = v;
        }
    }

    #[test]
    fn mirror_equals_the_sdk() {
        // 8-bit stored values, their half steps (what a bilinear filter lands on), and a dense sweep.
        let mut cs: Vec<f32> = (0..=255).flat_map(|g| [g as f32 / 255.0, (g as f32 + 0.5) / 255.0]).collect();
        cs.extend((0..=4096).map(|k| k as f32 / 4096.0));
        for c in cs {
            let (a, b) = (sdk_pwl(c), wgsl_pwl(c));
            // The two differ only where a segment edge falls between float roundings of c * 255 and c >= g / 255.
            assert!((a - b).abs() < 2.5e-3, "c {c}: sdk {a} vs mirror {b}");
        }
        for g in 0..=255 {
            let c = g as f32 / 255.0;
            assert!((sdk_pwl(c) - wgsl_pwl(c)).abs() < 3e-3, "stored {g}");
        }
    }

    /// The branch-free form equals the per-segment form bit for bit: every 8-bit value and its neighbours (the joins at
    /// 64, 96, 192 and the integer steps of l), half steps, and a dense stride over every f32 in 0..=1 (the whole range is
    /// `branch_free_is_exact_everywhere`, ignored by default).
    #[test]
    fn branch_free_equals_the_segments() {
        let same = |c: f32| assert_eq!(wgsl_pwl(c).to_bits(), segments_pwl(c).to_bits(), "c = {c:e} ({:#x})", c.to_bits());
        for g in 0..=255u32 {
            for h in [0.0f32, 0.5] {
                let c = (g as f32 + h) / 255.0;
                same(c);
                for d in -3i32..=3 {
                    let n = f32::from_bits((c.to_bits() as i32 + d) as u32);
                    if !n.is_nan() {
                        same(n); // (a NaN never comes out of a texture sample)
                    }
                }
            }
        }
        let one = 1.0f32.to_bits();
        for b in (0..=one).step_by(997) {
            same(f32::from_bits(b));
        }
        // Out-of-range input is clamped the same way.
        same(-0.5);
        same(1.5);
        assert_eq!(wgsl_pwl(-0.5), 0.0);
        assert!((wgsl_pwl(1.5) - 1.0).abs() < 1e-6);
    }

    /// Every f32 from 0 to 1 (1.06 billion): `cargo test -p matdef --release -- --ignored branch_free_is_exact`.
    #[test]
    #[ignore]
    fn branch_free_is_exact_everywhere() {
        for b in 0..=1.0f32.to_bits() {
            let c = f32::from_bits(b);
            assert_eq!(wgsl_pwl(c).to_bits(), segments_pwl(c).to_bits(), "c = {c:e}");
        }
    }

    #[test]
    fn old_curve_was_low_at_the_top() {
        // The B-235 max-of-lines form: exact over the darks it matters in, up to 0.8% under the SDK's at the top (white 0.992).
        let mut worst = 0.0f32;
        for g in 0..=255 {
            let c = g as f32 / 255.0;
            let d = sdk_pwl(c) - old_pwl(c);
            assert!(d > -1e-4, "stored {g}: old curve above the SDK's by {}", -d);
            worst = worst.max(d);
        }
        assert!(worst > 0.005 && worst < 0.009, "worst gap {worst}");
        assert!((old_pwl(1.0) - 0.9921875).abs() < 1e-6);
    }

    /// Dark texels are where the curve differs from sRGB (stored 0.1 -> 0.025 here, 0.010 in sRGB): the reason a PWL
    /// decode lifts shadows, and what the tests above must keep fixed.
    #[test]
    fn darks_are_far_from_srgb() {
        let srgb = |c: f32| if c <= 0.04045 { c / 12.92 } else { ((c + 0.055) / 1.055).powf(2.4) };
        let g = 0.1f32;
        assert!((sdk_pwl(g) - 0.0249).abs() < 1e-3);
        assert!(sdk_pwl(g) > 2.0 * srgb(g));
    }

    /// The WGSL and its mirror stay together: the shader has the SDK's constants and no longer the max-of-lines form.
    #[test]
    fn wgsl_matches_mirror() {
        let f = SCENE.split("fn xe_pwl3").nth(1).and_then(|s| s.split("\n}\n").next()).expect("xe_pwl3 in scene.wgsl");
        for tok in ["255.0", "g * 2.0 - 64.0", "g * 4.0 - 256.0", "g * 8.0 - 1024.0", "512.0", "256.0", "128.0", "floor", "1023.0", "saturate", "min("] {
            assert!(f.contains(tok), "xe_pwl3 lacks {tok}:\n{f}");
        }
        assert!(!f.contains("0.2490234375"), "the pre-B-392 max-of-lines form is still there");
        assert!(!f.contains("trunc"), "the select-per-segment form is back");
    }
}
