//! Frame environment (N12): sun, hemisphere ambient, height fog, post curve and dynamic lights, ported from n8gfx
//! (which is wgpu-bound) and fed from the scene feed's light/fog/post JSON (`lightfeed`, N10).

use lightfeed::pack::DynFrame;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub struct Globals {
    pub view_proj: [f32; 16],
    pub cam_pos: [f32; 4],
    pub sun_dir: [f32; 4],
    pub sun_col: [f32; 4],
    pub amb_up: [f32; 4],
    pub amb_dn: [f32; 4],
    pub fog_col: [f32; 4],
    pub fog_par: [f32; 4],
    pub tune: [f32; 4],
    pub tune2: [f32; 4],
    pub tune3: [f32; 4],
}

/// Sun, ambient, fog and tuning. Defaults are N8's mission 6 values (used until a feed frame arrives).
#[derive(Clone, Debug)]
pub struct Env {
    /// Direction TO the sun, Y-up world.
    pub sun_dir: [f32; 3],
    pub sun_col: [f32; 3],
    pub sun_enabled: bool,
    pub amb_up: [f32; 3],
    pub amb_dn: [f32; 3],
    /// (colour, density, height (Y), start distance, height falloff)
    pub fog: Option<([f32; 3], f32, f32, f32, f32)>,
    pub lm_gain: f32,
    pub spec_gain: f32,
    pub vl_gain: f32,
    /// B-189: exponent applied to vertex-lightmap samples before their scales (the port's 2.2; 1 = the old raw use).
    pub vl_gamma: f32,
    pub vcol_map: u32,
    pub nmap_strength: f32,
    pub sun_luma: f32,
    pub dbg: f32,
    pub env_gain: f32,
    pub env_ambient: f32,
    pub dyn_gain: f32,
    /// Per-term gains on the light environments (directional, point, SH): diagnostics (`BOTS_N1_ENVP=envdir=...`).
    pub env_terms: [f32; 3],
    /// Diagnostic: reverse the light environments' directional (`envflip`).
    pub env_flip: bool,
}

impl Default for Env {
    fn default() -> Self {
        Env {
            sun_dir: [-0.1512, 0.5078, 0.8481],
            sun_col: [252.0 / 255.0 * 1.5, 192.0 / 255.0 * 1.5, 152.0 / 255.0 * 1.5],
            sun_enabled: true,
            amb_up: [0.35, 0.38, 0.45],
            amb_dn: [0.20, 0.18, 0.15],
            fog: Some(([0.612, 0.737, 0.983], 4e-6, 6443.0, 4000.0, 1e8)),
            lm_gain: 1.0,
            spec_gain: 1.0,
            vl_gain: 1.0,
            vl_gamma: 2.2,
            vcol_map: 0,
            nmap_strength: 1.0,
            sun_luma: 1.0,
            dbg: 0.0,
            env_gain: 1.0,
            env_ambient: 0.0,
            dyn_gain: 1.0,
            env_terms: [1.0; 3],
            env_flip: false,
        }
    }
}

impl Env {
    /// As n8gfx `Env::from_feed`: the frame's sun, sky hemisphere (+ fills into the lower half) and height fog.
    ///
    /// B-249: `ue3_gamma` takes the sun, the fills and the sky lights in the colours UE3's light passes use,
    /// `pow(LightColor / 255, 2.2) * Brightness` (the port's pixel constants at 21115 and 32225: sun (4.787, 1.945,
    /// 0.680) for LightColor (250, 166, 103), Brightness 5, where the plain colour is (4.902, 3.255, 2.020), whiter
    /// and so a white Humvee and white lockers). `BOTS_N1_SKY_GAMMA=0` keeps the plain colours.
    pub fn from_feed(fr: &lightfeed::Frame, base: &Env, ue3_gamma: bool) -> Env {
        let mut e = base.clone();
        let col = |l: &lightfeed::Light| if ue3_gamma { l.ue3_color() } else { l.color };
        match &fr.sun {
            Some(s) => {
                e.sun_dir = [-s.travel[0], -s.travel[1], -s.travel[2]];
                e.sun_col = col(s);
                e.sun_enabled = s.enabled;
            }
            None => e.sun_enabled = false,
        }
        if fr.sky_up != [0.0; 3] {
            let (up, dn) = if ue3_gamma { (fr.sky_up_ue3, fr.sky_down_ue3) } else { (fr.sky_up, fr.sky_down) };
            e.amb_up = up;
            e.amb_dn = dn;
            for f in &fr.fills {
                let c = col(f);
                for k in 0..3 {
                    e.amb_dn[k] += c[k];
                }
            }
        }
        // W4: as the port's fog pass (draw 911 of capture f4420): colour LightColor^2.2, extinction distance in w.
        e.fog = fr.fog.as_ref().map(|f| (f.light_color.map(|c| c.powf(2.2)), f.density, f.height, f.start, f.extinction));
        e
    }

    /// `key=value,...` overrides, as n8gfx (`BOTS_N1_ENVP`).
    pub fn apply(&mut self, s: &str) {
        for kv in s.split(',').filter(|x| !x.is_empty()) {
            let (k, v) = kv.split_once('=').unwrap_or((kv, "1"));
            let f: f32 = v.parse().unwrap_or(1.0);
            match k {
                "lm" => self.lm_gain = f,
                "spec" => self.spec_gain = f,
                "vl" => self.vl_gain = f,
                "vlgamma" => self.vl_gamma = f,
                "vcol" => self.vcol_map = f as u32,
                "nmap" => self.nmap_strength = f,
                "sunluma" => self.sun_luma = f,
                "nofog" => self.fog = None,
                "flat" => self.dbg = 1.0,
                "irr" => self.dbg = 2.0,
                "nrm" => self.dbg = 3.0,
                "ff" => self.dbg = 4.0,
                "envk" => self.env_gain = f,
                "envamb" => self.env_ambient = f,
                "dynk" => self.dyn_gain = f,
                "envdir" => self.env_terms[0] = f,
                "envpt" => self.env_terms[1] = f,
                "envsh" => self.env_terms[2] = f,
                "envflip" => self.env_flip = f != 0.0,
                "nosun" => self.sun_enabled = false,
                "sunk" => self.sun_col = self.sun_col.map(|c| c * f),
                "ambk" => {
                    self.amb_up = self.amb_up.map(|c| c * f);
                    self.amb_dn = self.amb_dn.map(|c| c * f);
                }
                "fogd" => {
                    if let Some(x) = self.fog.as_mut() {
                        x.1 = f;
                    }
                }
                "fogs" => {
                    if let Some(x) = self.fog.as_mut() {
                        x.3 = f;
                    }
                }
                _ => eprintln!("n1ffi: unknown env key {k}"),
            }
        }
    }

    pub fn globals(&self, view_proj: [f32; 16], eye: [f32; 3], time: f32) -> Globals {
        let f = self.fog;
        Globals {
            view_proj,
            cam_pos: [eye[0], eye[1], eye[2], time],
            sun_dir: [self.sun_dir[0], self.sun_dir[1], self.sun_dir[2], self.sun_enabled as u32 as f32],
            sun_col: [self.sun_col[0], self.sun_col[1], self.sun_col[2], 0.0],
            amb_up: [self.amb_up[0], self.amb_up[1], self.amb_up[2], 0.0],
            amb_dn: [self.amb_dn[0], self.amb_dn[1], self.amb_dn[2], 0.0],
            fog_col: f.map(|f| [f.0[0], f.0[1], f.0[2], 1.0]).unwrap_or([0.0; 4]),
            fog_par: f.map(|f| [f.1, f.2, f.3, f.4]).unwrap_or([0.0; 4]),
            tune: [self.lm_gain, self.spec_gain, self.vl_gain, self.dbg],
            tune2: [self.vcol_map as f32, self.sun_luma, self.nmap_strength, 0.0],
            tune3: [self.env_gain, self.env_ambient, self.dyn_gain, self.vl_gamma],
        }
    }
}

/// W4: the game's post chain (MaterialEffect grade, uber DOF + bloom, gamma 2.2), driven by the feed.
#[derive(Clone, Debug)]
pub struct PostParams {
    pub uber: lightfeed::post::Uber,
    pub grade: Option<lightfeed::post::Grade>,
    /// Output gamma (the port's composite: pow(saturate(c), 1 / 2.2)).
    pub gamma: f32,
    /// Composite multiplier before saturate (1 / Scene_HighLights in UE3; white in every capture so far).
    pub inv_hi: [f32; 3],
    /// Mix of the bloom-kernel blur into the composite when UseDifferentKernelForBloom (the port's c250.x).
    pub bloom_mix: f32,
    /// DOF + bloom passes on (`BOTS_N1_POSTP=nodof` or `BOTS_N1_DOF=0` turn them off: grade + gamma only).
    pub dof: bool,
    /// B-266 / B-267: the active hand-ported post materials (`postmat.rs`), in chain order.
    pub mats: Vec<crate::postmat::Effect>,
}

impl Default for PostParams {
    /// No feed yet: no blur, no bloom, identity grade, gamma 2.2.
    fn default() -> Self {
        PostParams {
            uber: lightfeed::post::Uber {
                bloom_scale: 0.0,
                bloom_threshold: 1.0,
                bloom_kernel: 16.0,
                blur_kernel: 1.0,
                different_bloom_kernel: false,
                falloff: 2.0,
                max_near: 0.0,
                max_far: 0.0,
                focus_radius: 2000.0,
                focus_distance: 0.0,
            },
            grade: None,
            gamma: 2.2,
            inv_hi: [1.0; 3],
            bloom_mix: 0.15,
            dof: env_flag("BOTS_N1_DOF", true),
            mats: Vec::new(),
        }
    }
}

/// UE3's gaussian as bilinear pairs: texels -R..R+1 weighted exp(-2 x^2 / R^2), R = min(kernel size, 4) (the
/// port's 5-tap BloomKernelSize 16 and 2-tap BlurKernelSize 1 kernels both match). Offsets scaled by `scale`.
pub fn kernel_taps(size: f32, scale: f32) -> Vec<(f32, f32)> {
    let r = size.round().clamp(1.0, 4.0) as i32;
    let rf = r as f32;
    let w = |x: i32| (-2.0 * (x * x) as f32 / (rf * rf)).exp();
    let mut taps = Vec::new();
    let mut x = -r;
    while x <= r {
        let (w0, w1) = (w(x), w(x + 1));
        taps.push(((x as f32 + w1 / (w0 + w1)) * scale, w0 + w1));
        x += 2;
    }
    let sum: f32 = taps.iter().map(|t| t.1).sum();
    taps.iter().map(|&(o, k)| (o, k / sum)).collect()
}

/// One post pass's uniform block (`P` in n1ffi/src/post.wgsl): 16 vec4.
pub struct PostBlock<'a> {
    pub texel: [f32; 2],
    pub dir: [f32; 2],
    pub taps: &'a [(f32, f32)],
    pub unorm: bool,
    pub near: f32,
    pub offset: [f32; 2],
    /// B-240: the screen fade (pre-gamma scale, post-gamma overlay); composite and copy only.
    pub fade: [[f32; 3]; 2],
}

impl PostParams {
    pub fn with_feed(mut self, p: &lightfeed::PostFeed) -> PostParams {
        if !p.present {
            return self;
        }
        if let Some(u) = &p.uber {
            self.uber = u.clone();
        }
        self.grade = p.grade.clone().filter(|g| !g.is_identity());
        self.mats = crate::postmat::effects(p);
        self
    }

    /// True when the DOF / bloom passes change the picture.
    pub fn blurs(&self) -> bool {
        let u = &self.uber;
        self.dof && (u.bloom_scale > 0.0 || u.max_near > 0.0 || u.max_far > 0.0)
    }

    /// `key=value,...` (`BOTS_N1_POSTP`): gamma, bloom, thr, near, far, focus, radius, falloff, blur, bloomk, mix,
    /// nograde, nodof
    pub fn apply(&mut self, s: &str) {
        for kv in s.split(',').filter(|x| !x.is_empty()) {
            let (k, v) = kv.split_once('=').unwrap_or((kv, "1"));
            let f: f32 = v.parse().unwrap_or(1.0);
            let u = &mut self.uber;
            match k {
                "gamma" => self.gamma = f,
                "bloom" => u.bloom_scale = f,
                "thr" => u.bloom_threshold = f,
                "near" => u.max_near = f,
                "far" => u.max_far = f,
                "focus" => u.focus_distance = f,
                "radius" => u.focus_radius = f.max(1.0),
                "falloff" => u.falloff = f,
                "blur" => u.blur_kernel = f,
                "bloomk" => u.bloom_kernel = f,
                "mix" => self.bloom_mix = f,
                "nograde" => self.grade = None,
                "nodof" => self.dof = false,
                _ => eprintln!("n1ffi: unknown post key {k}"),
            }
        }
    }

    pub fn block(&self, b: &PostBlock) -> [[f32; 4]; 16] {
        let u = &self.uber;
        let g = self.grade.clone().unwrap_or(lightfeed::post::Grade {
            fg_desaturate: 0.0,
            fg_contrast: 0.0,
            fg_tint: [0.0; 3],
            bk_brightness: 1.0,
            bk_desaturate: 0.0,
            bk_contrast: 0.0,
            bk_tint: [1.0; 3],
            blend: 1e-4,
            blend_offset: -0.05,
            sky_clamp: 2.0,
        });
        let mut o = [[0.0f32; 4]; 16];
        o[0] = [u.bloom_scale, u.bloom_threshold, b.texel[0], b.texel[1]];
        o[1] = [u.focus_distance, 1.0 / u.focus_radius.max(1.0), u.falloff, b.offset[0]];
        o[2] = [u.max_near, u.max_far, b.near, b.offset[1]];
        o[3] = [b.dir[0], b.dir[1], b.unorm as u32 as f32, b.taps.len().min(6) as f32];
        let mix = if u.different_bloom_kernel { self.bloom_mix } else { 0.0 };
        o[4] = [mix, 1.0 / self.gamma.max(0.1), self.blurs() as u32 as f32, 0.0];
        o[5] = [g.fg_desaturate, g.fg_contrast, g.bk_brightness, g.bk_desaturate];
        o[6] = [g.bk_contrast, g.blend, g.blend_offset, g.sky_clamp];
        o[7] = [g.fg_tint[0], g.fg_tint[1], g.fg_tint[2], 0.0];
        o[8] = [g.bk_tint[0], g.bk_tint[1], g.bk_tint[2], 0.0];
        o[9] = [self.inv_hi[0], self.inv_hi[1], self.inv_hi[2], 0.0];
        for (k, t) in b.taps.iter().take(6).enumerate() {
            o[10 + k] = [t.0, t.1, 0.0, 0.0];
        }
        // B-240: the composite and copy passes have no taps, so their last two slots carry the screen fade
        // (`fade_scale`, `fade_overlay` in post.wgsl). The blurs (at most 5 taps) never read them.
        if b.taps.is_empty() {
            let [sc, ov] = b.fade;
            o[14] = [sc[0], sc[1], sc[2], 0.0];
            o[15] = [ov[0], ov[1], ov[2], 0.0];
        }
        o
    }
}

/// Low-end knobs, read once from the environment.
#[derive(Clone, Copy, Debug)]
pub struct Knobs {
    /// Level dynamic lights (`BOTS_N1_DYN=0` turns them off).
    pub dyn_lights: bool,
    /// B-230: level dynamic lights take UE3's colour, `pow(LightColor / 255, 2.2) * Brightness`, as the port's light
    /// pass does (`BOTS_N1_LIGHT_GAMMA=0`: the old plain `LightColor / 255 * Brightness`, up to 8x too bright on the
    /// dark-coloured, high-brightness explosion lights).
    pub light_gamma: bool,
    /// Per-object light environments for characters/movers (`BOTS_N1_ENV=0`).
    pub envs: bool,
    /// Bloom + grade + tone curve (`BOTS_N1_POST=0`: plain copy).
    pub post: bool,
    /// B-266 / B-267: the game's post materials besides the grade, as n1 passes (`BOTS_N1_POST_MATS=0`: grade only).
    pub post_mats: bool,
    /// Shader feature bits as n8gfx (`BOTS_N1_FEATURES`): 1 lightmaps, 2 normal maps, 4 specular, 8 fog, 16 dynamic.
    pub features: u32,
    /// Pipelines created per frame at most (`BOTS_N1_PIPE_BUDGET`); draws without one wait.
    pub pipe_budget: u32,
    /// Lights listed per instance (`BOTS_N1_LISTED`, max 4).
    pub listed: usize,
    /// Particles (`BOTS_N1_PARTICLES=0` turns them off).
    pub particles: bool,
    /// B-189: light-environment modulated shadows (`BOTS_N1_SHADOW=0` turns them off), at most `shadow_max`
    /// a frame (`BOTS_N1_SHADOW_MAX`, 1..16), each up to `shadow_res` texels square (`BOTS_N1_SHADOW_RES`, the port's
    /// MaxShadowResolution 512).
    pub shadows: bool,
    pub shadow_max: u32,
    pub shadow_res: u32,
    /// B-205: draw each shadow only where its projected volume covers the screen (a screen rectangle per shadow), and drop
    /// shadows that darken nothing on screen (`BOTS_N1_SHADOW_MASK=0` turns it off, for A/B).
    pub shadow_mask: bool,
    /// B-231: per-instance shadow receiver masks (`BOTS_N1_SHADOW_IMASK=0` turns them off, for A/B).
    pub shadow_imask: bool,
    /// B-231 measurement only: `BOTS_N1_SHADOW_APPLY=0` draws the shadow maps but applies none (attribution).
    pub shadow_apply: bool,
    /// B-231 experiment: opaque draw order (`BOTS_N1_SORT`: 0 by state, the default; 1 front to back per pipeline;
    /// 2 front to back).
    pub opaque_sort: u32,
    /// B-244: dynamic lights as the port's light passes. `dyn_post`: added after the modulated shadows, which the port
    /// draws before its dynamic-light passes (`BOTS_N1_DYN_POST=0`: shadowed, as before). `dyn_lightvec`: a material
    /// that reads LightVector (the characters' TemplateBody_Mat wrap) bends its normal toward the instance's strongest
    /// dynamic light for that light (`BOTS_N1_DYN_LIGHTVEC=0`: the key light's normal for every light, as before).
    /// `dyn_spec`: each dynamic light adds the port's specular, `pow(R.L, power)` times its colour and falloff
    /// (`BOTS_N1_DYN_SPEC=0`: diffuse only, as before).
    pub dyn_post: bool,
    pub dyn_lightvec: bool,
    pub dyn_spec: bool,
    /// B-245: light environments' directional, point and SH colours as UE3 uses them,
    /// `pow(LightColor / 255, 2.2) * Brightness`, as the port's directional pass gets them (`BOTS_N1_ENV_GAMMA=0`: the
    /// plain `LightColor / 255 * Brightness`, which is up to twice as blue on the warm key light and turns skin blue-grey).
    pub env_gamma: bool,
    /// B-249: the level's sun, fill and sky light colours as UE3 uses them, `pow(LightColor / 255, 2.2) * Brightness`
    /// (`BOTS_N1_SKY_GAMMA=0`: the plain `LightColor / 255 * Brightness`, which is too white and too bright on every
    /// mesh lit by the sun and sky instead of a lightmap).
    pub sky_gamma: bool,
    /// B-245: a light environment's directional specular as the port's directional pass, Phong `pow(R.L, power)`
    /// without N.L (`BOTS_N1_ENV_SPEC=0`: the old Blinn `pow(N.H, power) * N.L`, a much wider lobe).
    pub env_spec: bool,
    /// B-250: lit mesh particles (gibs, debris, casings) are lit by the sky hemisphere, the sun and the strongest
    /// dynamic lights, as the port's mesh passes (`BOTS_N1_MESH_LIT=0`: the old flat 0.7 on the diffuse colour).
    pub mesh_lit: bool,
    /// B-261: a material's ReflectionVector is the tangent-space reflection about its Normal output, as the port's shaders
    /// compute it (`BOTS_N1_REFL_TS=0`: the old world-space reflection about the vertex normal).
    pub refl_ts: bool,
    /// B-277: a Fresnel with a Normal input uses the tangent-space view vector (`BOTS_N1_FRESNEL_TS=0`: the world-space view).
    pub fresnel_ts: bool,
    /// Clamp mesh particles' instance colour rgb to 1 like the original's 8-bit colour (B-246). `BOTS_N1_MESH_VCOL_CLAMP=0` off.
    pub mesh_vcol_clamp: bool,
    /// B-264: the vertex-light buffer grows by a GPU copy of the old words (the CPU writes only the new ones) and syncs on
    /// loading frames too, so a level hand-over never pays a 30+ ms CPU copy into a fresh buffer. `BOTS_N1_LIGHT_GROW=0`: the
    /// old path (the whole array rewritten on the first scene frame after it outgrows the buffer).
    pub light_grow: bool,
    /// B-265: bones outside the skinned primitive's current LOD (`N1SkelInstance::lod`, the game's PredictedLODLevel)
    /// are rebuilt from the reference pose: the game leaves them stale (a killed enemy at LOD 2 kept its standing face and
    /// fingers while the body fell, drawn as spikes). `BOTS_N1_LOD_BONES=0`: the old path (every finite bone as given).
    pub lod_bones: bool,
}

fn env_flag(name: &str, default: bool) -> bool {
    match std::env::var(name) {
        Ok(v) => !(v == "0" || v.eq_ignore_ascii_case("false") || v.eq_ignore_ascii_case("off")),
        Err(_) => default,
    }
}

fn env_num<T: std::str::FromStr>(name: &str, default: T) -> T {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

impl Knobs {
    pub fn from_env() -> Knobs {
        let dyn_lights = env_flag("BOTS_N1_DYN", true);
        let envs = env_flag("BOTS_N1_ENV", true);
        let mut features = env_num("BOTS_N1_FEATURES", 31u32);
        if !dyn_lights && !envs {
            features &= !16; // nothing to light with: compile the dynamic-light code out
        }
        Knobs {
            dyn_lights,
            light_gamma: env_flag("BOTS_N1_LIGHT_GAMMA", true),
            envs,
            post: env_flag("BOTS_N1_POST", true),
            post_mats: env_flag("BOTS_N1_POST_MATS", true),
            features,
            pipe_budget: env_num("BOTS_N1_PIPE_BUDGET", 8u32),
            listed: env_num("BOTS_N1_LISTED", 4usize),
            particles: env_flag("BOTS_N1_PARTICLES", true),
            shadows: env_flag("BOTS_N1_SHADOW", true) && envs && features & 16 != 0,
            shadow_max: env_num("BOTS_N1_SHADOW_MAX", 8u32).clamp(1, 16),
            shadow_res: env_num("BOTS_N1_SHADOW_RES", 512u32).clamp(64, 2048),
            shadow_mask: env_flag("BOTS_N1_SHADOW_MASK", true),
            shadow_imask: env_flag("BOTS_N1_SHADOW_IMASK", true),
            shadow_apply: env_flag("BOTS_N1_SHADOW_APPLY", true),
            opaque_sort: env_num("BOTS_N1_SORT", 0u32),
            dyn_post: env_flag("BOTS_N1_DYN_POST", true),
            dyn_lightvec: env_flag("BOTS_N1_DYN_LIGHTVEC", true),
            dyn_spec: env_flag("BOTS_N1_DYN_SPEC", true),
            env_gamma: env_flag("BOTS_N1_ENV_GAMMA", true),
            sky_gamma: env_flag("BOTS_N1_SKY_GAMMA", true),
            env_spec: env_flag("BOTS_N1_ENV_SPEC", true),
            mesh_lit: env_flag("BOTS_N1_MESH_LIT", true),
            mesh_vcol_clamp: env_flag("BOTS_N1_MESH_VCOL_CLAMP", true),
            refl_ts: env_flag("BOTS_N1_REFL_TS", true),
            fresnel_ts: env_flag("BOTS_N1_FRESNEL_TS", true),
            light_grow: env_flag("BOTS_N1_LIGHT_GROW", true),
            lod_bones: env_flag("BOTS_N1_LOD_BONES", true),
        }
    }
}

/// The latest feed frame, digested.
pub struct Lighting {
    pub base_env: Env,
    pub env: Env,
    /// B-249: `env` with the sun, fill and sky colours in the other mode (`debug_bits` bit 11 swaps it in).
    pub env_alt: Env,
    pub base_post: PostParams,
    pub post: PostParams,
    pub dyn_frame: DynFrame,
    /// B-230 debug A/B: `dyn_frame.lights` packed in the other colour mode (`n1_set_debug_bits` bit 0 swaps them in).
    pub dyn_lights_alt: Vec<[f32; lightfeed::pack::LIGHT_F32]>,
    /// B-245 debug A/B: `dyn_frame.envs` packed in the other environment colour mode (debug bit 8, value 256).
    pub envs_alt: Vec<[f32; lightfeed::pack::ENV_F32]>,
    /// Environment component path -> index into `dyn_frame.envs`.
    pub env_index: std::collections::HashMap<String, usize>,
    pub frames: u64,
}

impl Lighting {
    pub fn new() -> Lighting {
        let mut base_env = Env::default();
        if let Ok(s) = std::env::var("BOTS_N1_ENVP") {
            base_env.apply(&s);
        }
        let mut base_post = PostParams::default();
        if let Ok(s) = std::env::var("BOTS_N1_POSTP") {
            base_post.apply(&s);
        }
        Lighting {
            env: base_env.clone(),
            env_alt: base_env.clone(),
            base_env,
            post: base_post.clone(),
            base_post,
            dyn_frame: DynFrame::default(),
            dyn_lights_alt: vec![],
            envs_alt: vec![],
            env_index: Default::default(),
            frames: 0,
        }
    }

    pub fn set_feed(&mut self, fr: &lightfeed::Frame, k: &Knobs) {
        self.env = Env::from_feed(fr, &self.base_env, k.sky_gamma);
        // B-249 debug A/B (`n1_set_debug_bits` bit 11): the sun, fills and sky in the other colour mode.
        self.env_alt = Env::from_feed(fr, &self.base_env, !k.sky_gamma);
        self.post = self.base_post.clone().with_feed(&fr.post);
        if !k.post_mats {
            self.post.mats.clear();
        }
        // Re-apply explicit overrides on top of the feed.
        if let Ok(s) = std::env::var("BOTS_N1_ENVP") {
            self.env.apply(&s);
            self.env_alt.apply(&s);
        }
        if let Ok(s) = std::env::var("BOTS_N1_POSTP") {
            self.post.apply(&s);
        }
        let max_lights = if k.dyn_lights { lightfeed::pack::MAX_LIGHTS } else { 0 };
        self.dyn_frame = DynFrame::from_frame_env(fr, max_lights, k.envs, k.light_gamma, k.env_gamma);
        self.dyn_lights_alt = DynFrame::from_frame_with(fr, max_lights, false, !k.light_gamma).lights;
        self.envs_alt = if k.envs {
            fr.envs.iter().take(lightfeed::pack::MAX_ENVS).map(|e| lightfeed::pack::pack_env_with(e, !k.env_gamma)).collect()
        } else {
            vec![]
        };
        let [kd, kp, ks] = self.env.env_terms;
        if [kd, kp, ks] != [1.0; 3] || self.env.env_flip {
            for e in self.dyn_frame.envs.iter_mut().chain(self.envs_alt.iter_mut()) {
                if self.env.env_flip {
                    e[0..3].iter_mut().for_each(|c| *c = -*c);
                }
                e[4..7].iter_mut().for_each(|c| *c *= kd);
                e[12..15].iter_mut().for_each(|c| *c *= kp);
                e[16..].iter_mut().for_each(|c| *c *= ks);
            }
        }
        self.env_index.clear();
        if k.envs {
            for (i, e) in fr.envs.iter().take(lightfeed::pack::MAX_ENVS).enumerate() {
                self.env_index.insert(e.path.clone(), i);
            }
        }
        self.frames += 1;
    }
}

#[cfg(test)]
mod bench {
    /// `N1_FEED_BENCH=<feed json> cargo test --release -p n1ffi feed_cost -- --ignored --nocapture`
    #[test]
    #[ignore]
    fn feed_cost() {
        let path = std::env::var("N1_FEED_BENCH").expect("N1_FEED_BENCH");
        let bytes = std::fs::read(path).unwrap();
        let mut l = super::Lighting::new();
        let k = super::Knobs::from_env();
        let n = 200;
        let (mut a, mut b, mut c) = (0f64, 0f64, 0f64);
        for _ in 0..n {
            let t0 = std::time::Instant::now();
            let v: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
            let t1 = std::time::Instant::now();
            let fr = lightfeed::Frame::from_value(&v).unwrap();
            let t2 = std::time::Instant::now();
            l.set_feed(&fr, &k);
            let t3 = std::time::Instant::now();
            a += (t1 - t0).as_secs_f64();
            b += (t2 - t1).as_secs_f64();
            c += (t3 - t2).as_secs_f64();
        }
        let ms = |x: f64| x * 1000.0 / n as f64;
        println!("{} bytes: parse {:.3} ms, from_value {:.3} ms, set_feed {:.3} ms", bytes.len(), ms(a), ms(b), ms(c));
    }
}

#[cfg(test)]
mod matprobe {
    /// `N1_COOKED_DIR=<extracted disc>/C1Game/CookedXenon N1_MATPROBE=<skel mesh path>[,...] cargo test --release -p n1ffi matprobe -- --ignored --nocapture`
    #[test]
    #[ignore]
    fn matprobe() {
        let db = assetdb::AssetDb::open(
            std::path::Path::new(&std::env::var("N1_COOKED_DIR").expect("N1_COOKED_DIR (the disc's C1Game/CookedXenon)")),
            std::path::Path::new("cache/n12"),
        )
        .unwrap();
        for mesh in std::env::var("N1_MATPROBE").unwrap().split(',') {
            let Some(s) = db.skeletal(mesh) else {
                println!("{mesh}: not found");
                continue;
            };
            println!("{mesh}: {:?}", s.materials);
            for m in &s.materials {
                let a = db.material(m);
                println!("== {m}: class {} diffuse {:?} blend {}\n{:#?}", a.class, a.diffuse, a.blend, a.def);
            }
        }
    }
}

#[cfg(test)]
mod texprobe {
    /// `N1_COOKED_DIR=<...>/CookedXenon N1_TEXPROBE=<texture path>[,...] cargo test --release -p n1ffi texprobe -- --ignored --nocapture`: format, size
    /// and the mean endpoint-0 colour of an 8x8-or-larger mip (BC1/2/3 colour blocks).
    #[test]
    #[ignore]
    fn texprobe() {
        let db = assetdb::AssetDb::open(
            std::path::Path::new(&std::env::var("N1_COOKED_DIR").expect("N1_COOKED_DIR (the disc's C1Game/CookedXenon)")),
            std::path::Path::new("cache/n12"),
        )
        .unwrap();
        for p in std::env::var("N1_TEXPROBE").unwrap().split(',') {
            let Some(t) = db.texture(p) else {
                println!("{p}: not found");
                continue;
            };
            let m = t.mips.len().saturating_sub(4).min(t.mips.len() - 1);
            let d = &t.mips[m];
            let bs = d.len() / (((t.width >> m).max(4) / 4) * ((t.height >> m).max(4) / 4)) as usize;
            let off = if bs == 16 { 8 } else { 0 };
            let (mut s, mut n) = ([0f64; 3], 0f64);
            for b in d.chunks_exact(bs.max(8)) {
                let c = u16::from_le_bytes([b[off], b[off + 1]]);
                s[0] += ((c >> 11) & 31) as f64 / 31.0;
                s[1] += ((c >> 5) & 63) as f64 / 63.0;
                s[2] += (c & 31) as f64 / 31.0;
                n += 1.0;
            }
            if let Ok(dir) = std::env::var("N1_TEXDUMP") {
                let (w, h) = ((t.width >> m).max(4), (t.height >> m).max(4));
                let mut f = Vec::new();
                f.extend_from_slice(b"DDS ");
                let mut hd = [0u32; 31];
                hd[0] = 124;
                hd[1] = 0x1007;
                hd[2] = h;
                hd[3] = w;
                hd[18] = 32;
                hd[19] = 4;
                hd[20] = u32::from_le_bytes(*b"DXT1");
                hd[26] = 0x1000;
                for x in hd {
                    f.extend_from_slice(&x.to_le_bytes());
                }
                f.extend_from_slice(d);
                std::fs::write(format!("{dir}/{p}.dds"), f).unwrap();
            }
            println!(
                "{p}: {:?} srgb {} {}x{} mips {} normal {} unpack {:?}; mip {m} block {bs} B, mean c0 {:.2} {:.2} {:.2}",
                t.fmt, t.srgb, t.width, t.height, t.mips.len(), t.normal, t.unpack, s[0] / n, s[1] / n, s[2] / n
            );
        }
    }
}

#[cfg(test)]
mod post_tests {
    use super::kernel_taps;

    /// The port's blur weights (W4 capture f4420, draws 945-948): BloomKernelSize 16 and BlurKernelSize 1.
    #[test]
    fn kernels_match_port() {
        let w: Vec<f32> = kernel_taps(16.0, 1.0).iter().map(|t| t.1).collect();
        for (a, b) in w.iter().zip([0.093, 0.301, 0.381, 0.188, 0.036]) {
            assert!((a - b).abs() < 2e-3, "{w:?}");
        }
        let w: Vec<f32> = kernel_taps(1.0, 1.0).iter().map(|t| t.1).collect();
        assert!((w[0] - 0.893).abs() < 2e-3 && (w[1] - 0.107).abs() < 2e-3, "{w:?}");
    }
}

#[cfg(test)]
mod gradeprobe {
    /// Prints each level's MaterialEffect grade (needs the extracted game: `N1_COOKED_DIR=<...>/CookedXenon cargo test -- --ignored gradeprobe`).
    #[test]
    #[ignore]
    fn gradeprobe() {
        let db = assetdb::AssetDb::open(
            std::path::Path::new(&std::env::var("N1_COOKED_DIR").expect("N1_COOKED_DIR (the disc's C1Game/CookedXenon)")),
            &std::env::temp_dir().join("n1-gradeprobe"),
        )
        .unwrap();
        for (lvl, m) in [
            ("Ambush_P", "GEN_PostProcess.AmbushPP_MAT01"),
            ("Crib_P", "MIS_CribGeneric.Crib_MAT01"),
            ("Helicopter_P", "GEN_PostProcess.FlyingLevel_MAT01"),
            ("MountainCity_P", "MIS_MountainCityGeneric.MountainCityPP_MAT01"),
            ("TheFlyover_P", "MIS_TheFlyoverMisc.FlyoverPP_Final_MAT01"),
            ("TheMall_P", "GEN_PostProcess.MallPP_MAT01"),
            ("TheTheatre_P", "MIS_TheTheatre_Environmental.TheatrePP_MIC"),
            ("DesertChase_P", "MIS_Driving02_Environmental.PostFX_v03_MAT"),
        ] {
            db.prefer_packages([lvl.to_string()]);
            let g = db.material_params(m).map(|(b, s, v)| (lightfeed::post::Grade::from_params(&b, &s, &v), b));
            println!("{lvl}: {g:?}");
        }
    }
}
