//! W2: what a material needs from the GPU, as a pure function of the disc's data. The live path
//! (`Renderer::try_add_material`) and the ahead-of-time enumeration (`precompile`) both go through `plan`, so the shader
//! a material asks for during play is the shader the startup step already built.

use assetdb::{AssetDb, MaterialAsset, TexFmt, TexMeta};
use matdef::codegen::Compiled;
use matdef::shader::ShaderFlags;
use matdef::{MatDef, Node, TexRef};
use scenekit::Blend;

/// What the shader depends on in a texture: BC5 normal maps get Z rebuilt and an unpack range, others the asset's.
#[derive(Clone, Copy, Default, Debug)]
pub struct TexInfo {
    pub bc5: bool,
    pub unpack: Option<([f32; 3], [f32; 3])>,
    /// B-235: uploaded raw for Xenos's gamma fetch; the shader applies the PWL curve (`scenekit::tex_gamma`).
    pub gamma: bool,
}

impl TexInfo {
    pub fn from_meta(m: &TexMeta, bc5_ok: bool) -> TexInfo {
        TexInfo {
            bc5: m.fmt == TexFmt::Bc5 && bc5_ok,
            unpack: m.unpack,
            gamma: scenekit::tex_gamma(m.srgb, m.normal, m.fmt),
        }
    }
}

pub struct Plan {
    pub c: Compiled,
    pub flags: ShaderFlags,
    pub blend: Blend,
    pub two_sided: bool,
    /// Shader identity: the flags and the structure key, hashed (stable across runs and Rust versions).
    pub key: u64,
}

/// FNV-1a 64. `DefaultHasher` isn't stable across releases and these keys are stored on disk.
pub fn fnv(bytes: &[u8]) -> u64 {
    let mut h = 0xcbf29ce484222325u64;
    for &b in bytes {
        h = (h ^ b as u64).wrapping_mul(0x100000001b3);
    }
    h
}

/// What distance fog fades a draw toward (`ShaderFlags::fog_mode`): additive surfaces only add light, so fog dims them
/// toward black (toward the fog colour they drew a fog-coloured rectangle over the scene, B-273: the lamp's light
/// shafts); modulate surfaces fade toward white (no darkening). `BOTS_N1_FOG_BLEND=0` keeps the fog colour for all.
pub fn fog_mode(blend: Blend) -> u8 {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    if !*ON.get_or_init(|| std::env::var("BOTS_N1_FOG_BLEND").map_or(true, |v| v != "0")) {
        return 0;
    }
    match blend {
        Blend::Additive => 1,
        Blend::Modulate => 2,
        _ => 0,
    }
}

pub fn shader_flags(features: u32, def: &MatDef, blend: Blend) -> ShaderFlags {
    ShaderFlags {
        lm: features & 1 != 0,
        nmap: features & 2 != 0,
        spec: features & 4 != 0,
        fog: features & 8 != 0,
        dynl: features & 16 != 0,
        lm_pwl: scenekit::lm_pwl(),
        fog_mode: crate::plan::fog_mode(blend),
        ..ShaderFlags::new(
            blend == Blend::Masked,
            def.lighting == "MLM_Unlit",
            blend.blended(),
            (def.clip.max(0.0) * 1000.0) as u32,
        )
    }
}

/// The stand-in for a material with no graph: its diffuse texture alpha-tested, or flat grey.
pub fn fallback_def(textured: bool) -> MatDef {
    if textured {
        MatDef {
            textures: vec![TexRef { path: String::new(), normal: false }],
            nodes: vec![Node::Tex { tex: 0, uv: None }, Node::Mask { x: 0, m: [false, false, false, true] }],
            diffuse: Some(0),
            opacity: Some(1),
            opacity_mask: Some(1),
            clip: 0.33,
            ..Default::default()
        }
    } else {
        MatDef { diffuse_const: Some([0.6, 0.6, 0.6, 1.0]), ..Default::default() }
    }
}

/// `m`: the resolved material; `diffuse_tex`: its diffuse texture exists (used only when it has no graph);
/// `tex(k)`: what texture slot `k` of the graph (or the single fallback slot) holds.
pub fn plan(features: u32, m: &MaterialAsset, diffuse_tex: bool, tex: &dyn Fn(usize) -> TexInfo) -> Plan {
    let fallback;
    let def: &MatDef = match m.def.as_deref() {
        Some(d) => d,
        None => {
            fallback = fallback_def(diffuse_tex);
            &fallback
        }
    };
    let n = def.textures.len();
    let info: Vec<TexInfo> = (0..n).map(&tex).collect();
    // A BC5 normal map without UnpackMin/Max still needs X and Y in [-1, 1].
    let unpack: Vec<Option<([f32; 3], [f32; 3])>> =
        info.iter().map(|t| t.unpack.or_else(|| t.bc5.then_some(([-1.0; 3], [1.0; 3])))).collect();
    // BC5 normal maps (characters, weapons) hold X and Y only: rebuild Z as UE3 does.
    let recon_z: Vec<bool> = info.iter().map(|t| t.bc5).collect();
    let gamma: Vec<bool> = info.iter().map(|t| t.gamma).collect();
    let c = matdef::codegen::compile_gamma(def, &unpack, &recon_z, &gamma);
    let blend = Blend::from_ue3(
        m.def.as_deref().map(|d| d.blend.as_str()).filter(|b| !b.is_empty()).unwrap_or(&m.blend),
    );
    let two_sided = m.two_sided || m.def.as_deref().is_some_and(|d| d.two_sided);
    let flags = shader_flags(features, def, blend);
    let key = fnv(format!("{flags:?}|{}", c.key).as_bytes());
    Plan { c, flags, blend, two_sided, key }
}

/// Every Material-like object on the disc, planned: the superset of what any level can ask for. Runs on `threads`
/// threads over package-ordered paths (each package is read once).
pub fn enumerate(
    db: &AssetDb,
    features: u32,
    bc5_ok: bool,
    threads: usize,
    progress: &(dyn Fn(usize, usize) + Sync),
) -> Vec<(String, Plan)> {
    let paths = db.paths_of_classes(&[
        "Material",
        "MaterialInstanceConstant",
        "MaterialInstanceTimeVarying",
        "DecalMaterial",
    ]);
    let next = std::sync::atomic::AtomicUsize::new(0);
    let done = std::sync::atomic::AtomicUsize::new(0);
    let out = std::sync::Mutex::new(Vec::<(usize, String, Plan)>::new());
    let total = paths.len();
    std::thread::scope(|s| {
        for _ in 0..threads.max(1) {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    let Some(p) = paths.get(i) else { break };
                    let m = db.material(p);
                    let diffuse = m.diffuse.as_ref().is_some_and(|d| db.texture_meta(d).is_some());
                    let metas: Vec<Option<TexMeta>> = match m.def.as_deref() {
                        Some(d) => d.textures.iter().map(|t| db.texture_meta(&t.path)).collect(),
                        None => m.diffuse.as_ref().map(|d| vec![db.texture_meta(d)]).unwrap_or_default(),
                    };
                    let pl = plan(features, &m, diffuse, &|k| {
                        metas.get(k).and_then(|x| x.as_ref()).map(|x| TexInfo::from_meta(x, bc5_ok)).unwrap_or_default()
                    });
                    out.lock().unwrap().push((i, p.clone(), pl));
                    let d = done.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
                    progress(d, total);
                }
            });
        }
    });
    let mut v = out.into_inner().unwrap();
    v.sort_by_key(|x| x.0);
    v.into_iter().map(|(_, p, pl)| (p, pl)).collect()
}
