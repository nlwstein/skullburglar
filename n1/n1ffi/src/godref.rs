//! B-313: the game's god-rays pass on the CPU, a reference for the `fs_god` shader in post.wgsl.
//!
//! This is the port's own ray shader (PS 1F26E2C0, decoded from a capture of chapter 73575) written out as plain
//! arithmetic, in the order the microcode does it. n1's shader reproduces it with one difference in where the mask comes
//! from (a texel of the 128x128 sky mask is the depth at the texel's centre; the port draws the mask into a target).
//! The tests check the reference against closed forms; `tests/godrays_gpu.rs` runs the real shader on a GPU and compares.
//!
//! The decoded program, per pixel of the 128x128 target (uv = the texel's centre):
//! ```text
//! L = sat(c3.xy)                       light point
//! d = uv - L;  m = max(|d.x|, |d.y|);  step = d / m * (1/128);  rx = |d.x|;  w = 1;  acc = 0
//! loop 80 times (i31 = 0x50):
//!     s = 0;  if rx >= 0 { s = bilinear(mask, uv).a > 0 ? 1 : 0 }      // the tfetch is predicated on rx
//!     acc += s * w;  w = sat(w - decay);  uv -= step;  rx -= |step.x|
//! out.rgb = sat(acc * magnitude * (1 - |d|^2) * colour) * exposure
//! ```

/// The side of the ray target and of the mask.
pub const GRID: usize = 128;
/// The mask test is "the bilinear value is not 0"; the hardware filters with an 8-bit fraction, so a texel weighing under
/// half a step contributes nothing (same constant as `GOD_MASK_EPS` in post.wgsl).
pub const MASK_EPS: f32 = 1.0 / 512.0;
/// The ray loop's iteration count (loop constant i31 = 0x50).
pub const STEPS: usize = 80;

/// B-320: how much of the ray texture the composite adds at a pixel whose scene is `z` UE units from the camera.
/// The game adds the rays to every pixel, so a wall next to the camera is veiled as much as a far hill; here the veil
/// fades in with distance (a smoothstep from `near` to `far`), like haze that needs air to scatter in. `far <= near`
/// turns the fade off (1 everywhere). The composite in post.wgsl (`god_depth_fade`) is this function.
pub fn depth_fade(z: f32, near: f32, far: f32) -> f32 {
    if far <= near {
        return 1.0;
    }
    let t = ((z - near) / (far - near)).clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

/// The pass's constants: c3.xy the light point, c4.x magnitude, c4.y decay per step, c4.z exposure, c5 the ray colour
/// (the ModulateRayColour already raised to 2.2).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Params {
    pub light: [f32; 2],
    pub magnitude: f32,
    pub decay: f32,
    pub color: [f32; 3],
    pub exposure: f32,
}

/// A 128x128 mask, row-major, alpha in 0..=1 (1 = sky).
#[derive(Clone)]
pub struct Mask(pub Vec<f32>);

impl Mask {
    pub fn new(f: impl Fn(usize, usize) -> f32) -> Self {
        Mask((0..GRID * GRID).map(|i| f(i % GRID, i / GRID)).collect())
    }

    fn texel(&self, x: i32, y: i32) -> f32 {
        let c = |v: i32| v.clamp(0, GRID as i32 - 1) as usize;
        self.0[c(y) * GRID + c(x)]
    }

    /// The bilinear fetch with clamp to edge (the ray pass's fetch constant: mag/min filter linear, clamp mode 2).
    pub fn bilinear(&self, u: f32, v: f32) -> f32 {
        let fx = u * GRID as f32 - 0.5;
        let fy = v * GRID as f32 - 0.5;
        let (ix, iy) = (fx.floor(), fy.floor());
        let (tx, ty) = (fx - ix, fy - iy);
        let (x, y) = (ix as i32, iy as i32);
        let mix = |a: f32, b: f32, t: f32| a * (1.0 - t) + b * t;
        let a = mix(self.texel(x, y), self.texel(x + 1, y), tx);
        let b = mix(self.texel(x, y + 1), self.texel(x + 1, y + 1), tx);
        mix(a, b, ty)
    }
}

fn sat(x: f32) -> f32 {
    x.clamp(0.0, 1.0)
}

/// One pixel of the ray pass at `uv`.
pub fn ray(mask: &Mask, uv: [f32; 2], p: &Params) -> [f32; 3] {
    let l = [sat(p.light[0]), sat(p.light[1])];
    let d = [uv[0] - l[0], uv[1] - l[1]];
    let m = d[0].abs().max(d[1].abs()).max(1e-8);
    let step = [d[0] / (m * GRID as f32), d[1] / (m * GRID as f32)];
    let (mut u, mut v) = (uv[0], uv[1]);
    let mut rx = d[0].abs();
    let mut w = 1.0f32;
    let mut acc = 0.0f32;
    for _ in 0..STEPS {
        if rx >= 0.0 && mask.bilinear(u, v) > MASK_EPS {
            acc += w;
        }
        w = sat(w - p.decay);
        if w <= 0.0 {
            break;
        }
        u -= step[0];
        v -= step[1];
        rx -= step[0].abs();
    }
    let fall = 1.0 - (d[0] * d[0] + d[1] * d[1]);
    p.color.map(|c| sat(acc * p.magnitude * fall * c) * p.exposure)
}

/// The whole 128x128 ray target (row-major).
pub fn render(mask: &Mask, p: &Params) -> Vec<[f32; 3]> {
    (0..GRID * GRID)
        .map(|i| {
            let uv = [((i % GRID) as f32 + 0.5) / GRID as f32, ((i / GRID) as f32 + 0.5) / GRID as f32];
            ray(mask, uv, p)
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn params() -> Params {
        Params { light: [0.93, 0.87], magnitude: 0.025, decay: 0.025, color: [0.97, 0.69, 0.31], exposure: 0.5 }
    }

    fn at(img: &[[f32; 3]], x: usize, y: usize) -> [f32; 3] {
        img[y * GRID + x]
    }

    /// B-320: the distance fade is 0 up to `near`, 1 from `far`, smooth between, monotonic, and off when `far <= near`.
    #[test]
    fn depth_fade_ramp() {
        assert_eq!(depth_fade(0.0, 1000.0, 5000.0), 0.0);
        assert_eq!(depth_fade(1000.0, 1000.0, 5000.0), 0.0);
        assert_eq!(depth_fade(5000.0, 1000.0, 5000.0), 1.0);
        assert_eq!(depth_fade(1.0e7, 1000.0, 5000.0), 1.0);
        assert!((depth_fade(3000.0, 1000.0, 5000.0) - 0.5).abs() < 1e-6);
        let mut last = 0.0;
        for k in 0..=100 {
            let f = depth_fade(1000.0 + 40.0 * k as f32, 1000.0, 5000.0);
            assert!(f >= last);
            last = f;
        }
        assert_eq!(depth_fade(10.0, 0.0, 0.0), 1.0);
        assert_eq!(depth_fade(10.0, 5000.0, 1000.0), 1.0);
    }

    #[test]
    fn empty_mask_is_black() {
        let img = render(&Mask::new(|_, _| 0.0), &params());
        assert!(img.iter().all(|p| *p == [0.0; 3]));
    }

    /// With the whole mask sky the sum is the weights of the steps that run: k = 0..=min(floor(128 m), 39) (w hits 0 at
    /// step 40, and the fetch stops once the march passes L), so each pixel has a closed form.
    #[test]
    fn full_sky_matches_closed_form() {
        let p = params();
        let img = render(&Mask::new(|_, _| 1.0), &p);
        let mut checked = 0;
        for (x, y) in [(5, 5), (20, 100), (64, 64), (100, 10), (10, 60), (70, 120), (120, 120), (3, 90)] {
            let uv = [(x as f32 + 0.5) / 128.0, (y as f32 + 0.5) / 128.0];
            let d = [uv[0] - 0.93, uv[1] - 0.87];
            let m = d[0].abs().max(d[1].abs());
            let last = ((128.0 * m).floor() as usize).min(STEPS - 1);
            // Skip pixels whose last step sits on the predicate's edge, where f32 rounding decides it.
            let frac = 128.0 * m - (128.0 * m).floor();
            if frac < 0.02 || frac > 0.98 {
                continue;
            }
            let acc: f32 = (0..=last).map(|k| (1.0 - 0.025 * k as f32).max(0.0)).sum();
            let want = (acc * 0.025 * (1.0 - (d[0] * d[0] + d[1] * d[1]))).clamp(0.0, 1.0);
            let got = at(&img, x, y);
            assert!((got[0] - (want * 0.97).clamp(0.0, 1.0) * 0.5).abs() < 2e-4, "({x},{y}) {got:?} vs {want}");
            checked += 1;
        }
        assert!(checked >= 5, "only {checked} pixels checked");
    }

    /// A light point outside the frame is clamped to its edge before anything else uses it.
    #[test]
    fn light_point_is_clamped() {
        let mask = Mask::new(|x, y| if (x / 8 + y / 8) % 2 == 0 { 1.0 } else { 0.0 });
        let out = Params { light: [1.6, -0.4], ..params() };
        let edge = Params { light: [1.0, 0.0], ..params() };
        assert_eq!(render(&mask, &out), render(&mask, &edge));
    }

    /// The march stops at the light point: sky only on the far side of L adds nothing, though 80 steps would reach it.
    #[test]
    fn march_stops_at_the_light() {
        let p = Params { light: [0.5, 0.5], ..params() };
        let far = Mask::new(|x, _| if x > 80 { 1.0 } else { 0.0 });
        let img = render(&far, &p);
        // Pixel (10, 64) is left of L (x = 0.5); the sky starts at x = 80/128 = 0.625, past L (0.5).
        assert_eq!(at(&img, 10, 64), [0.0; 3]);
        // From the right side the same sky is on the way to L and does count.
        assert!(at(&img, 120, 64)[0] > 0.0);
    }

    /// A pixel at 60 steps from L gets at most 40 weighted steps (the weight is 0 from the 40th): sky only in the
    /// 41st to 60th cell on the way adds nothing.
    #[test]
    fn weight_runs_out() {
        let p = Params { light: [0.0, 0.5], ..params() };
        // Pixel x = 100 marches left; cells 100 - 45 = 55 and below are sky: all beyond step 40.
        let m = Mask::new(|x, _| if x < 56 { 1.0 } else { 0.0 });
        assert_eq!(at(&render(&m, &p), 100, 64), [0.0; 3]);
        let m = Mask::new(|x, _| if x < 70 { 1.0 } else { 0.0 });
        assert!(at(&render(&m, &p), 100, 64)[0] > 0.0);
    }

    /// Colour, exposure and magnitude scale the sum; the falloff is 1 - |uv - L|^2.
    #[test]
    fn scaling() {
        let mask = Mask::new(|_, _| 1.0);
        let a = at(&render(&mask, &params()), 40, 70);
        let b = at(&render(&mask, &Params { exposure: 0.25, ..params() }), 40, 70);
        assert!((a[0] - 2.0 * b[0]).abs() < 1e-6);
        let c = at(&render(&mask, &Params { magnitude: 0.0125, ..params() }), 40, 70);
        assert!((a[1] - 2.0 * c[1]).abs() < 1e-6);
    }
}
