//! assetdb: resolve the object paths a live scene feed names (meshes, materials, textures, level components) to the
//! disc's cooked packages, extract on demand with the `ue3` reader, and cache the results under a cache directory
//! (gitignored: everything here is derived from the disc).
//!
//! - **Index** (`<cache>/index.json`): every StaticMesh / SkeletalMesh / material / Texture2D export in every cooked
//!   package, by object path (the same dotted path UE3's Outer chain gives at run time, e.g.
//!   `MIS_Helicopter.C1StaticMeshes.Canyon04A`), with the packages that hold a copy. Built once (all packages but
//!   audio), rebuilt when the package list or sizes change.
//! - **Meshes** (`<cache>/mesh/<path>.bin`): LOD0 positions, normals, UV0, indices and sections, in UE3 space.
//! - **Textures** (`<cache>/tex/<path>.bin`): untiled mips as the GPU wants them (BC1/2/3/5, R8, BGRA8).
//! - **Component light** (`<cache>/light/<level package>.bin`): per-vertex baked light (FLightMap1D) of every
//!   StaticMeshComponent placed in a level package, keyed by the component's full path
//!   (`Helicopter_ARTStart.TheWorld.PersistentLevel.C1StaticMeshActor_99.StaticMeshComponent_804`), which is the
//!   `name` the scene feed reports.
//!
//! Everything is `Sync`: a loader thread can resolve while a render thread draws.

use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use ue3::Package;

mod skel;
pub use skel::{SkelAsset, SkelSectionAsset, SkinVertex, required_flags};
pub use ue3::matir::{MatDef, Node, TexRef};

/// GPU-ready texture formats (what the 360's cooked textures use).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum TexFmt {
    Bc1,
    Bc2,
    Bc3,
    Bc5,
    R8,
    Bgra8,
}

impl TexFmt {
    /// (block size in texels, bytes per block)
    pub fn block(self) -> (u32, u32) {
        match self {
            TexFmt::Bc1 => (4, 8),
            TexFmt::Bc2 | TexFmt::Bc3 | TexFmt::Bc5 => (4, 16),
            TexFmt::R8 => (1, 1),
            TexFmt::Bgra8 => (1, 4),
        }
    }
}

#[derive(Clone, Debug)]
pub struct TextureAsset {
    pub name: String,
    pub fmt: TexFmt,
    pub srgb: bool,
    pub width: u32,
    pub height: u32,
    /// Largest first; each mip is tightly packed rows of blocks.
    pub mips: Vec<Vec<u8>>,
    /// CompressionSettings is TC_Normalmap*.
    pub normal: bool,
    /// UnpackMin/Max when not the identity: sample = tex * (max - min) + min.
    pub unpack: Option<([f32; 3], [f32; 3])>,
}

/// A texture's header facts (see `AssetDb::texture_meta`).
#[derive(Clone, Copy, Debug)]
pub struct TexMeta {
    pub fmt: TexFmt,
    pub srgb: bool,
    pub normal: bool,
    pub unpack: Option<([f32; 3], [f32; 3])>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct SectionAsset {
    pub first_index: u32,
    pub index_count: u32,
    /// Material object path ("" = none).
    pub material: String,
}

#[derive(Clone, Debug)]
pub struct MeshAsset {
    pub name: String,
    pub positions: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub uvs: Vec<[f32; 2]>,
    pub indices: Vec<u32>,
    pub sections: Vec<SectionAsset>,
    /// Bounding sphere in mesh space.
    pub center: [f32; 3],
    pub radius: f32,
    /// UE3 space, w = bitangent sign (as stored).
    pub tangents: Vec<[f32; 4]>,
    /// Vertex colours, bytes in file order (A, R, G, B); white when the mesh has none.
    pub colors: Vec<[u8; 4]>,
    /// Second UV set (lightmap coordinates); zeros when absent.
    pub uv1: Vec<[f32; 2]>,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct MaterialAsset {
    pub class: String,
    pub diffuse: Option<String>,
    pub blend: String,
    pub two_sided: bool,
    /// N8's material IR (the whole graph, MIC parameters folded in), when the chain resolves to a Material.
    #[serde(skip)]
    pub def: Option<Arc<ue3::matir::MatDef>>,
}

/// A placed component's baked light, as `ue3::level::placements` reads it (N8's order of preference: 2D lightmap,
/// then directional vertex light, then the legacy single colour).
#[derive(Clone, Debug)]
pub enum ComponentLight {
    /// Per-vertex linear colour.
    Legacy(Vec<[f32; 3]>),
    /// Per-vertex directional light: three FColor words per vertex (bytes A, R, G, B) and an RGB scale per word.
    Dir { words: Vec<u32>, scale: [[f32; 3]; 3] },
    /// 2D lightmap: three LightMapTexture2D exports (level package, export index), a scale per coefficient and the
    /// UV1 transform (uv = uv1 * coord_scale + coord_bias).
    Map {
        textures: [Option<(String, i32)>; 3],
        scale: [[f32; 3]; 3],
        coord_scale: [f32; 2],
        coord_bias: [f32; 2],
    },
}


/// A SpeedTree component's baked vertex light (B-187): per geometry slot (branch+frond, leaf card, billboard, leaf mesh)
/// three FColor words (bytes A, R, G, B) per vertex and an RGB scale per word, when the component stores one.
#[derive(Clone, Debug, Default)]
pub struct TreeLight {
    pub slots: [Option<(Vec<u32>, [[f32; 3]; 3])>; 4],
}

/// The material object paths of a SpeedTree asset.
#[derive(Clone, Debug, Default)]
pub struct TreeMaterials {
    pub branch: String,
    pub frond: String,
    pub leaf: String,
}

#[derive(Serialize, Deserialize, Default)]
struct Index {
    /// (file stem, size) of every package indexed: the index is stale when this changes.
    packages: Vec<(String, u64)>,
    /// object path -> package indices (into `packages`), for meshes and materials
    objects: HashMap<String, Vec<u32>>,
    /// object path -> package indices, Texture2D only (a Material and a Texture2D can share one path in BotS)
    textures: HashMap<String, Vec<u32>>,
    /// object path -> class name
    classes: HashMap<String, String>,
    /// `INDEX_REV` when built: the index is stale when the set of indexed classes changes.
    #[serde(default)]
    rev: u32,
}

#[derive(Debug, Clone, Copy)]
pub struct Stats {
    pub index_objects: usize,
    pub index_textures: usize,
    pub packages: usize,
}

pub struct AssetDb {
    cooked: PathBuf,
    cache: PathBuf,
    index: Index,
    open: Mutex<Vec<(String, Arc<Package>)>>, // most recently used last
    max_open: usize,
    /// How many packages `package` has opened (decompressed) and the wall ms that took, summed over threads.
    pub opens: std::sync::atomic::AtomicU64,
    pub open_ms: std::sync::atomic::AtomicU64,
    tfc: ue3::texture::TfcCache,
    meshes: Mutex<HashMap<String, Option<Arc<MeshAsset>>>>,
    skeletals: Mutex<HashMap<String, Option<Arc<SkelAsset>>>>,
    materials: Mutex<HashMap<String, Arc<MaterialAsset>>>,
    textures: Mutex<HashMap<String, Option<Arc<TextureAsset>>>>,
    lights: Mutex<HashMap<String, Arc<HashMap<String, Arc<Vec<[f32; 3]>>>>>>,
    lights_full: Mutex<HashMap<String, Arc<HashMap<String, Arc<ComponentLight>>>>>,
    tree_lights: Mutex<HashMap<String, Arc<HashMap<String, Arc<TreeLight>>>>>,
    tree_mats: Mutex<HashMap<String, Option<Arc<TreeMaterials>>>>,
    /// Packages to prefer when an object exists in several (the level's own packages).
    preferred: Mutex<HashSet<String>>,
    /// Per package: (is Texture2D, object path) -> first export index, built on first lookup (`locate` otherwise walks
    /// every export of the package building paths: ~30 ms a call on the big ones).
    path_maps: Mutex<HashMap<String, Arc<HashMap<(bool, String), i32>>>>,
    pub verbose: bool,
    pub max_tex: u32,
}

fn cache_name(path: &str) -> String {
    path.chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-' {
                c
            } else {
                '_'
            }
        })
        .collect()
}

const INDEX_CLASSES: &[&str] = &[
    "StaticMesh",
    "SkeletalMesh",
    "Material",
    "MaterialInstanceConstant",
    "MaterialInstanceTimeVarying",
    "DecalMaterial",
    "SpeedTree",
    // W2: the startup step finds every particle material through its emitters.
    "ParticleSpriteEmitter",
    "ParticleMeshEmitter",
];
/// Bump when `INDEX_CLASSES` changes.
const INDEX_REV: u32 = 3;

impl AssetDb {
    /// Opens (or builds) the index. `cooked` is `game/C1Game/CookedXenon`.
    pub fn open(cooked: &Path, cache: &Path) -> Result<Self> {
        std::fs::create_dir_all(cache).with_context(|| format!("create {}", cache.display()))?;
        let mut files: Vec<(String, u64, PathBuf)> = std::fs::read_dir(cooked)
            .with_context(|| format!("read {}", cooked.display()))?
            .filter_map(|e| e.ok())
            .filter_map(|e| {
                let p = e.path();
                let stem = p.file_stem()?.to_string_lossy().into_owned();
                (p.extension()? == "xxx" && !stem.contains("Audio"))
                    .then(|| (stem, e.metadata().map(|m| m.len()).unwrap_or(0), p))
            })
            .collect();
        files.sort();
        let key: Vec<(String, u64)> = files.iter().map(|(n, s, _)| (n.clone(), *s)).collect();
        let index_path = cache.join("index.json");
        let index = match std::fs::read(&index_path)
            .ok()
            .and_then(|b| serde_json::from_slice::<Index>(&b).ok())
        {
            Some(ix) if ix.packages == key && ix.rev == INDEX_REV => ix,
            _ => {
                let t0 = std::time::Instant::now();
                let ix = build_index(&files)?;
                eprintln!(
                    "assetdb: indexed {} packages ({} objects, {} textures) in {:.1?}",
                    ix.packages.len(),
                    ix.objects.len(),
                    ix.textures.len(),
                    t0.elapsed()
                );
                std::fs::write(&index_path, serde_json::to_vec(&ix)?)?;
                ix
            }
        };
        for d in ["mesh", "tex", "light", "light2", "treelight"] {
            std::fs::create_dir_all(cache.join(d))?;
        }
        Ok(Self {
            cooked: cooked.to_path_buf(),
            cache: cache.to_path_buf(),
            index,
            open: Mutex::new(Vec::new()),
            max_open: std::env::var("BOTS_N1_MAX_OPEN")
                .ok()
                .and_then(|v| v.parse().ok())
                .unwrap_or(16),
            opens: Default::default(),
            open_ms: Default::default(),
            tfc: ue3::texture::TfcCache::new(cooked),
            meshes: Mutex::new(HashMap::new()),
            skeletals: Mutex::new(HashMap::new()),
            materials: Mutex::new(HashMap::new()),
            textures: Mutex::new(HashMap::new()),
            lights: Mutex::new(HashMap::new()),
            lights_full: Mutex::new(HashMap::new()),
            tree_lights: Mutex::new(HashMap::new()),
            tree_mats: Mutex::new(HashMap::new()),
            preferred: Mutex::new(HashSet::new()),
            path_maps: Mutex::new(HashMap::new()),
            verbose: std::env::var("ASSETDB_VERBOSE").is_ok(),
            max_tex: std::env::var("BOTS_N1_MAXTEX").ok().and_then(|v| v.parse().ok()).unwrap_or(2048).max(64),
        })
    }

    pub fn stats(&self) -> Stats {
        Stats {
            index_objects: self.index.objects.len(),
            index_textures: self.index.textures.len(),
            packages: self.index.packages.len(),
        }
    }

    /// Packages whose copy of an object wins (e.g. the packages the running level loaded).
    pub fn prefer_packages<I: IntoIterator<Item = String>>(&self, names: I) {
        self.preferred.lock().unwrap().extend(names);
    }

    /// Every indexed object path of the given classes (grouped by package, then by path).
    pub fn paths_of_classes(&self, classes: &[&str]) -> Vec<String> {
        let mut v: Vec<String> = self
            .index
            .classes
            .iter()
            .filter(|(p, c)| classes.contains(&c.as_str()) && self.index.objects.contains_key(*p))
            .map(|(p, _)| p.clone())
            .collect();
        // Grouped by the package holding the object, so a caller walking the list opens each package once.
        v.sort_by_key(|p| (self.index.objects.get(p).and_then(|x| x.first().copied()).unwrap_or(u32::MAX), p.clone()));
        v
    }

    /// Texture2D paths containing `sub` (case-insensitive) with the packages holding each, for finding same-named
    /// textures in several packages (a tool, `texfind`).
    pub fn texture_holders(&self, sub: &str) -> Vec<(String, Vec<String>)> {
        let sub = sub.to_lowercase();
        let mut v: Vec<(String, Vec<String>)> = self
            .index
            .textures
            .iter()
            .filter(|(p, _)| p.to_lowercase().contains(&sub))
            .map(|(p, pk)| (p.clone(), pk.iter().map(|&i| self.index.packages[i as usize].0.clone()).collect()))
            .collect();
        v.sort();
        v
    }

    /// Names of every indexed package (a tool, `placefind`).
    pub fn package_names(&self) -> Vec<String> {
        self.index.packages.iter().map(|(n, _)| n.clone()).collect()
    }

    /// The static-mesh placements a package holds (those that parse), for tools.
    pub fn level_placements(&self, pkg_name: &str) -> Vec<ue3::level::Placement> {
        let Ok(pkg) = self.package(pkg_name) else { return Vec::new() };
        ue3::level::placements(&pkg).into_iter().filter(|(_, e)| e.is_none()).map(|(p, _)| p).collect()
    }

    /// The class of an object and the bytes that follow its property list in its export (native serialization), for
    /// tools that decode what the property reader skips (`mictail`).
    pub fn object_tail(&self, path: &str) -> Option<(String, Vec<u8>)> {
        let (pkg, idx) = self.locate(path, false, None).ok().flatten()?;
        let (_, r) = ue3::props::object_props(&pkg, idx).ok()?;
        let data = pkg.export_data(idx);
        Some((pkg.class_name(idx).to_string(), data[r.o..].to_vec()))
    }

    /// The Parent of a MaterialInstanceConstant (object path), for tools.
    pub fn material_parent(&self, path: &str) -> Option<String> {
        let (pkg, idx) = self.locate(path, false, None).ok().flatten()?;
        let (props, _) = ue3::props::object_props(&pkg, idx).ok()?;
        let o = props.get("Parent").map(|x| x.as_obj()).filter(|&o| o != 0)?;
        Some(pkg.obj_path(o))
    }

    /// An object's property list as JSON (`ue3::pmaterial::export_json`), for tools.
    pub fn object_json(&self, path: &str) -> Option<serde_json::Value> {
        let (pkg, idx) = self.locate(path, false, None).ok().flatten()?;
        ue3::pmaterial::export_json(&pkg, idx).ok()
    }

    /// A name of the package that holds `path` (by name-table index), for tools decoding native bytes.
    pub fn name_in_package_of(&self, path: &str, i: usize) -> Option<String> {
        let (pkg, _) = self.locate(path, false, None).ok().flatten()?;
        pkg.names.get(i).cloned()
    }

    /// Whether a cooked package of this name (file stem) is indexed.
    pub fn has_package(&self, name: &str) -> bool {
        self.index.packages.iter().any(|(n, _)| n.eq_ignore_ascii_case(name))
    }

    /// B-199 preload: every indexed object a package holds a copy of, as (object path, class), Texture2D excluded
    /// (materials and meshes name their textures). An object in the file's own package is indexed under two paths
    /// (with and without the package name in front, see `export_paths`); only the one the game uses at run time (the
    /// prefixed one) is returned.
    pub fn package_objects(&self, name: &str) -> Vec<(String, String)> {
        let Some(pi) = self.index.packages.iter().position(|(n, _)| n.eq_ignore_ascii_case(name)) else {
            return Vec::new();
        };
        let pi = pi as u32;
        let pkg = &self.index.packages[pi as usize].0;
        let prefix = format!("{pkg}.");
        let mut out: Vec<(String, String)> = self
            .index
            .objects
            .iter()
            .filter(|(p, pk)| {
                pk.contains(&pi) && (p.starts_with(&prefix) || !self.index.objects.contains_key(&format!("{prefix}{p}")))
            })
            .filter_map(|(p, _)| self.index.classes.get(p).map(|c| (p.clone(), c.clone())))
            .collect();
        out.sort();
        out
    }

    /// B-199 preload: the full paths of every placed component of a level package that has baked light (the keys
    /// `component_light_full` answers for). Opens the package on first use; call it from a loader thread.
    pub fn level_light_components(&self, pkg: &str) -> Vec<String> {
        let probe = format!("{pkg}.");
        let _ = self.component_light_full(&probe);
        self.lights_full
            .lock()
            .unwrap()
            .get(pkg)
            .map(|m| m.keys().cloned().collect())
            .unwrap_or_default()
    }

    /// B-199 preload: the full paths of every placed SpeedTreeComponent of a level package with baked light.
    pub fn level_tree_components(&self, pkg: &str) -> Vec<String> {
        let probe = format!("{pkg}.");
        let _ = self.tree_light(&probe);
        self.tree_lights
            .lock()
            .unwrap()
            .get(pkg)
            .map(|m| m.keys().cloned().collect())
            .unwrap_or_default()
    }

    /// The emitter at an object path of class `ParticleSpriteEmitter` / `ParticleMeshEmitter`: its material, screen
    /// alignment and type data.
    pub fn emitter_info(&self, path: &str) -> Option<ue3::particle::EmitterInfo> {
        let (p, ei) = self.locate(path, false, None).ok().flatten()?;
        ue3::particle::read_emitter(&p, ei).ok()
    }

    /// Class of an indexed object path, if any.
    pub fn class_of(&self, path: &str) -> Option<&str> {
        self.index.classes.get(path).map(|s| s.as_str())
    }

    fn package(&self, name: &str) -> Result<Arc<Package>> {
        {
            let mut open = self.open.lock().unwrap();
            if let Some(i) = open.iter().position(|(n, _)| n == name) {
                let e = open.remove(i);
                let p = e.1.clone();
                open.push(e);
                return Ok(p);
            }
        }
        let path = self.cooked.join(format!("{name}.xxx"));
        let t = std::time::Instant::now();
        let p = Arc::new(Package::open(&path)?);
        self.opens
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        self.open_ms.fetch_add(
            t.elapsed().as_millis() as u64,
            std::sync::atomic::Ordering::Relaxed,
        );
        let mut open = self.open.lock().unwrap();
        open.push((name.to_string(), p.clone()));
        while open.len() > self.max_open {
            open.remove(0);
        }
        Ok(p)
    }

    fn path_map(&self, name: &str, pkg: &Package) -> Arc<HashMap<(bool, String), i32>> {
        if let Some(m) = self.path_maps.lock().unwrap().get(name) {
            return m.clone();
        }
        let mut m: HashMap<(bool, String), i32> = HashMap::new();
        for i in 1..=pkg.exports.len() as i32 {
            let cls = pkg.class_name(i);
            if cls == "Texture2D" || INDEX_CLASSES.contains(&cls) {
                for p in export_paths(pkg, i) {
                    m.entry((cls == "Texture2D", p)).or_insert(i);
                }
            }
        }
        let m = Arc::new(m);
        self.path_maps.lock().unwrap().insert(name.to_string(), m.clone());
        m
    }

    /// The package holding `path` (preferred packages first) and the export index there.
    ///
    /// `want` is the export class the caller can use. A path can exist in several packages with different classes
    /// (a level's StaticMesh copy of a vehicle that another package ships as a SkeletalMesh); the preferred set only
    /// grows over a session, so without this an earlier level's wrong-class copy shadowed the right one.
    /// With no class match the first hit is returned, as before (`ASSETDB_CLASSAWARE=0` forces that).
    fn locate(
        &self,
        path: &str,
        textures: bool,
        want: Option<&str>,
    ) -> Result<Option<(Arc<Package>, i32)>> {
        let map = if textures {
            &self.index.textures
        } else {
            &self.index.objects
        };
        let Some(pkgs) = map.get(path) else {
            return Ok(None);
        };
        let pref = self.preferred.lock().unwrap().clone();
        let mut order: Vec<&String> = pkgs
            .iter()
            .map(|&i| &self.index.packages[i as usize].0)
            .collect();
        order.sort_by_key(|n| !pref.contains(*n));
        let want = want.filter(|_| std::env::var_os("ASSETDB_CLASSAWARE").is_none_or(|v| v != "0"));
        pick_class(
            want,
            order.into_iter().filter_map(|name| {
                let found = (|| -> Result<Option<(Arc<Package>, i32)>> {
                    let pkg = self.package(name)?;
                    let map = self.path_map(name, &pkg);
                    let i = map.get(&(textures, path.to_string())).copied();
                    Ok(i.map(|i| (pkg, i)))
                })();
                found.transpose()
            }),
            |(pkg, i), class| pkg.class_name(*i) == class,
        )
    }

    /// LOD0 of a StaticMesh by object path; `None` when no package has it (or it fails to parse).
    pub fn mesh(&self, path: &str) -> Option<Arc<MeshAsset>> {
        if let Some(m) = self.meshes.lock().unwrap().get(path) {
            return m.clone();
        }
        let r = self.load_mesh(path).unwrap_or_else(|e| {
            if self.verbose {
                eprintln!("assetdb: mesh {path}: {e:#}");
            }
            None
        });
        self.meshes
            .lock()
            .unwrap()
            .insert(path.to_string(), r.clone());
        r
    }

    fn load_mesh(&self, path: &str) -> Result<Option<Arc<MeshAsset>>> {
        let file = self.cache.join("mesh").join(cache_name(path) + ".bin");
        if let Ok(b) = std::fs::read(&file) {
            if let Ok(m) = decode_mesh(path, &b) {
                return Ok(Some(Arc::new(m)));
            }
        }
        let Some((pkg, idx)) = self.locate(path, false, Some("StaticMesh"))? else {
            return Ok(None);
        };
        if pkg.class_name(idx) != "StaticMesh" {
            bail!("{path} is a {}", pkg.class_name(idx));
        }
        let sm = ue3::mesh::read_static_mesh(&pkg, idx)?;
        let Some(l) = sm.lods.into_iter().next() else {
            bail!("no LODs")
        };
        let (mut lo, mut hi) = ([f32::MAX; 3], [f32::MIN; 3]);
        for p in &l.positions {
            for k in 0..3 {
                lo[k] = lo[k].min(p[k]);
                hi[k] = hi[k].max(p[k]);
            }
        }
        let center = [
            (lo[0] + hi[0]) * 0.5,
            (lo[1] + hi[1]) * 0.5,
            (lo[2] + hi[2]) * 0.5,
        ];
        let radius = l
            .positions
            .iter()
            .map(|p| {
                ((p[0] - center[0]).powi(2)
                    + (p[1] - center[1]).powi(2)
                    + (p[2] - center[2]).powi(2))
                .sqrt()
            })
            .fold(0.0f32, f32::max);
        let nv = l.positions.len();
        let fit = |mut v: Vec<[f32; 4]>| {
            v.resize(nv, [1.0, 0.0, 0.0, 1.0]);
            v
        };
        let mut colors = l.colors.clone();
        colors.resize(nv, [255; 4]);
        let mut uv1 = l.uvs.get(1).cloned().unwrap_or_default();
        uv1.resize(nv, [0.0, 0.0]);
        let m = MeshAsset {
            name: path.to_string(),
            tangents: fit(l.tangents.clone()),
            colors,
            uv1,
            uvs: l
                .uvs
                .first()
                .cloned()
                .unwrap_or_else(|| vec![[0.0, 0.0]; nv]),
            positions: l.positions,
            normals: l.normals,
            indices: l.indices.iter().map(|&i| i as u32).collect(),
            sections: l
                .sections
                .iter()
                .map(|s| SectionAsset {
                    first_index: s.first_index,
                    index_count: s.num_tris * 3,
                    material: s.material.clone(),
                })
                .collect(),
            center,
            radius,
        };
        let _ = std::fs::write(&file, encode_mesh(&m));
        Ok(Some(Arc::new(m)))
    }

    /// Material by object path: diffuse texture (by name heuristics over ReferencedTextures, following Parent),
    /// blend mode and two-sidedness. Unknown paths give a default (opaque, no texture, class "missing").
    pub fn material(&self, path: &str) -> Arc<MaterialAsset> {
        if let Some(m) = self.materials.lock().unwrap().get(path) {
            return m.clone();
        }
        let mut mo = self.resolve_material(ue3::matir::base_path(path), 0);
        mo.def = ue3::matir::build_by(path, |q| self.locate(q, false, None).ok().flatten()).map(Arc::new);
        // A class the mesh/decal compiler can't express draws as a constant 0.0 (invisible or black surfaces): say so
        // once per material, so a gap shows up in the log instead of as a missing floor or a flat colour.
        // `BOTS_N1_MATLOG=0` silences it.
        if let Some(d) = mo.def.as_ref().filter(|d| !d.unsupported.is_empty()) {
            if std::env::var("BOTS_N1_MATLOG").map_or(true, |v| v != "0") {
                eprintln!("n1: material {path} [{}] uses unsupported expressions {:?} (drawn as constant 0)", d.blend, d.unsupported);
            }
        }
        let m = Arc::new(mo);
        self.materials
            .lock()
            .unwrap()
            .insert(path.to_string(), m.clone());
        m
    }

    /// Drops a cached material (a run-time variant the renderer released; `material` rebuilds it if asked again).
    pub fn forget_material(&self, path: &str) {
        self.materials.lock().unwrap().remove(path);
    }

    /// A material's named parameters (MIC overrides over the base material's defaults): see `ue3::matir::params_by`.
    pub fn material_params(&self, path: &str) -> Option<ue3::matir::NamedParams> {
        ue3::matir::params_by(path, |q| self.locate(q, false, None).ok().flatten())
    }

    /// N9's view of a material for particles: the expression graph (EmissiveColor, Opacity, OpacityMask,
    /// DiffuseColor roots) of the Material at the end of the Parent chain, as `scenefmt::particles::matprog`
    /// reads it. `None` when the path or its parent chain doesn't resolve to a Material.
    pub fn particle_material_json(&self, path: &str) -> Option<serde_json::Value> {
        // MaterialInstanceConstant parameter values along the chain (the run-time ones from the path first), so an
        // instance's own textures, colours and scalars replace the parent's defaults.
        let (base, mut params) = ue3::matir::path_overrides(path);
        let mut path = base.to_string();
        for _ in 0..6 {
            let (p, ei) = self.locate(&path, false, None).ok().flatten()?;
            let class = p.class_name(ei).to_string();
            if class == "MaterialInstanceConstant" {
                ue3::matir::instance_params(&p, ei, &mut params);
            }
            if class.starts_with("MaterialInstance") {
                let j = ue3::pmaterial::export_json(&p, ei).ok()?;
                let parent = j["props"]["Parent"]["path"].as_str().unwrap_or_default().to_string();
                if parent.is_empty() {
                    return None;
                }
                path = parent;
                continue;
            }
            let (mat, graph) = ue3::pmaterial::material_graph(&p, ei, &["EmissiveColor", "Opacity", "OpacityMask", "DiffuseColor", "SpecularColor", "SpecularPower"]).ok()?;
            let overrides = serde_json::json!({
                "scalar": params.scalar,
                "vector": params.vector,
                "texture": params.texture,
            });
            return Some(serde_json::json!({"class": class, "props": mat["props"], "expressions": graph, "overrides": overrides}));
        }
        None
    }

    fn resolve_material(&self, path: &str, depth: u32) -> MaterialAsset {
        let mut mo = MaterialAsset::default();
        let Ok(Some((p, ei))) = self.locate(path, false, None) else {
            mo.class = "missing".into();
            return mo;
        };
        mo.class = p.class_name(ei).to_string();
        let Ok((props, _)) = ue3::props::object_props(&p, ei) else {
            return mo;
        };
        mo.blend = props
            .get("BlendMode")
            .and_then(|x| {
                if x.data.len() == 8 {
                    ue3::props::read_fname(&p, &mut x.r()).ok()
                } else {
                    None
                }
            })
            .unwrap_or_default();
        mo.two_sided = props.get("TwoSided").map(|x| x.bool_value).unwrap_or(false);
        let cands: Vec<String> = props
            .list
            .iter()
            .filter(|x| x.name == "ReferencedTextures")
            .flat_map(|x| x.as_obj_array())
            .filter(|&o| o != 0)
            .map(|o| p.obj_path(o))
            .collect();
        // Texture parameters of a MaterialInstanceConstant name textures too (ParameterValue in a struct array).
        mo.diffuse = pick_diffuse(&cands);
        let parent = props
            .get("Parent")
            .map(|x| x.as_obj())
            .filter(|&o| o != 0)
            .map(|o| p.obj_path(o));
        if depth < 4 {
            if let Some(parent) = parent {
                if mo.diffuse.is_none() || mo.blend.is_empty() {
                    let pm = self.resolve_material(&parent, depth + 1);
                    if mo.diffuse.is_none() {
                        mo.diffuse = pm.diffuse;
                    }
                    if mo.blend.is_empty() {
                        mo.blend = pm.blend;
                    }
                    mo.two_sided |= pm.two_sided;
                }
            }
        }
        mo
    }

    /// What a texture's shader needs to know about it, from the export's properties alone (no mips are read):
    /// `None` when the path is unknown, the format is unsupported or the header doesn't parse.
    pub fn texture_meta(&self, path: &str) -> Option<TexMeta> {
        let (pkg, idx) = self.locate(path, true, None).ok().flatten()?;
        let (props, _) = ue3::props::object_props(&pkg, idx).ok()?;
        let name = |n: &str| -> String {
            props
                .get(n)
                .and_then(|p| if p.data.len() == 8 { ue3::props::read_fname(&pkg, &mut p.r()).ok() } else { None })
                .unwrap_or_default()
        };
        let mut format = name("Format");
        if format.is_empty() {
            format = "PF_A8R8G8B8".into();
        }
        let fmt = match ue3::texture::Fmt::from_name(&format)? {
            ue3::texture::Fmt::Dxt1 => TexFmt::Bc1,
            ue3::texture::Fmt::Dxt3 => TexFmt::Bc2,
            ue3::texture::Fmt::Dxt5 => TexFmt::Bc3,
            ue3::texture::Fmt::Bc5 => TexFmt::Bc5,
            ue3::texture::Fmt::G8 => TexFmt::R8,
            ue3::texture::Fmt::Rgba8 => TexFmt::Bgra8,
        };
        let (mut mn, mut mx) = ([0.0f32; 3], [1.0f32; 3]);
        for k in 0..3 {
            if let Some(p) = props.get_idx("UnpackMin", k as i32) {
                mn[k] = p.as_f32();
            }
            if let Some(p) = props.get_idx("UnpackMax", k as i32) {
                mx[k] = p.as_f32();
            }
        }
        Some(TexMeta {
            fmt,
            srgb: props.get("SRGB").map(|p| p.bool_value).unwrap_or(true),
            normal: name("CompressionSettings").contains("Normalmap"),
            unpack: (mn != [0.0; 3] || mx != [1.0; 3]).then_some((mn, mx)),
        })
    }

    /// A stamp of what the index was built from (package names and sizes): a cache derived from the disc is stale
    /// when this changes.
    pub fn index_stamp(&self) -> u64 {
        let mut h = 0xcbf29ce484222325u64 ^ INDEX_REV as u64;
        for (n, sz) in &self.index.packages {
            for b in n.bytes().chain(sz.to_le_bytes()) {
                h = (h ^ b as u64).wrapping_mul(0x100000001b3);
            }
        }
        h
    }

    /// A Texture2D by object path, untiled; `None` if missing or in an unsupported format.
    pub fn texture(&self, path: &str) -> Option<Arc<TextureAsset>> {
        if let Some(t) = self.textures.lock().unwrap().get(path) {
            return t.clone();
        }
        let r = self.load_texture(path).unwrap_or_else(|e| {
            if self.verbose {
                eprintln!("assetdb: texture {path}: {e:#}");
            }
            None
        });
        self.textures
            .lock()
            .unwrap()
            .insert(path.to_string(), r.clone());
        r
    }

    fn load_texture(&self, path: &str) -> Result<Option<Arc<TextureAsset>>> {
        let file = self.cache.join("tex").join(cache_name(path) + ".bin");
        if let Ok(b) = std::fs::read(&file) {
            if let Ok(t) = decode_texture(path, &b) {
                return Ok(Some(Arc::new(self.trim(t))));
            }
        }
        let Some((pkg, idx)) = self.locate(path, true, None)? else {
            return Ok(None);
        };
        let ta = convert_texture(path, ue3::texture::read_texture(&pkg, idx, &self.tfc)?)?;
        let _ = std::fs::write(&file, encode_texture(&ta));
        Ok(Some(Arc::new(self.trim(ta))))
    }

    /// Drops mips above `max_tex` (BOTS_N1_MAXTEX, default 2048) for small GPUs.
    fn trim(&self, mut t: TextureAsset) -> TextureAsset {
        while t.mips.len() > 1 && t.width > self.max_tex {
            t.mips.remove(0);
            t.width = (t.width / 2).max(1);
            t.height = (t.height / 2).max(1);
        }
        t
    }

    /// A level package's LightMapTexture2D by export index (what `ComponentLight::Map` names).
    pub fn lightmap_texture(&self, pkg_name: &str, idx: i32) -> Option<Arc<TextureAsset>> {
        let key = format!("{pkg_name}::#{idx}");
        if let Some(t) = self.textures.lock().unwrap().get(&key) {
            return t.clone();
        }
        let r = (|| -> Result<Option<Arc<TextureAsset>>> {
            let file = self.cache.join("tex").join(cache_name(&key) + ".bin");
            if let Ok(b) = std::fs::read(&file) {
                if let Ok(t) = decode_texture(&key, &b) {
                    return Ok(Some(Arc::new(self.trim(t))));
                }
            }
            if !self.index.packages.iter().any(|(n, _)| n == pkg_name) {
                bail!("no package {pkg_name}");
            }
            let pkg = self.package(pkg_name)?;
            let ta = convert_texture(&key, ue3::texture::read_texture(&pkg, idx, &self.tfc)?)?;
            let _ = std::fs::write(&file, encode_texture(&ta));
            Ok(Some(Arc::new(self.trim(ta))))
        })()
        .unwrap_or_else(|e| {
            if self.verbose {
                eprintln!("assetdb: lightmap {key}: {e:#}");
            }
            None
        });
        self.textures.lock().unwrap().insert(key, r.clone());
        r
    }

    /// A placed component's full baked light (lightmap, directional or legacy vertex light), by its full path.
    pub fn component_light_full(&self, component: &str) -> Option<Arc<ComponentLight>> {
        let (pkg, _) = component.split_once('.')?;
        let cached = self.lights_full.lock().unwrap().get(pkg).cloned();
        let map = match cached {
            Some(m) => m,
            None => {
                let m = Arc::new(self.load_level_light_full(pkg).unwrap_or_else(|e| {
                    if self.verbose {
                        eprintln!("assetdb: light {pkg}: {e:#}");
                    }
                    HashMap::new()
                }));
                self.lights_full.lock().unwrap().insert(pkg.to_string(), m.clone());
                m
            }
        };
        map.get(component).cloned()
    }

    fn load_level_light_full(&self, pkg_name: &str) -> Result<HashMap<String, Arc<ComponentLight>>> {
        let inherit = std::env::var_os("BOTS_N1_LIGHT_INHERIT").map_or(true, |v| v != "0");
        let file = self.cache.join(if inherit { "light3" } else { "light2" }).join(cache_name(pkg_name) + ".bin");
        if let Ok(b) = std::fs::read(&file) {
            if let Ok(m) = decode_light_full(&b) {
                return Ok(m);
            }
        }
        if !self.index.packages.iter().any(|(n, _)| n == pkg_name) {
            bail!("no package {pkg_name}");
        }
        let pkg = self.package(pkg_name)?;
        let mut out = HashMap::new();
        for (pl, err) in ue3::level::placements(&pkg) {
            if err.is_some() {
                continue;
            }
            let key = format!("{pkg_name}.{}", pl.component);
            let three = |s: &[[f32; 3]]| [s[0], s[1], s[2]];
            let l = if let Some(lm) = pl.lightmap2d.as_ref().filter(|l| l.texture_idx[0] > 0) {
                let t = |k: usize| (lm.texture_idx[k] > 0).then(|| (pkg_name.to_string(), lm.texture_idx[k]));
                ComponentLight::Map { textures: [t(0), t(1), t(2)], scale: three(&lm.scale), coord_scale: lm.coord_scale, coord_bias: lm.coord_bias }
            } else if let Some((v, sc)) = pl.vertex_light_dir {
                let words = v.iter().flat_map(|b| b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))).collect();
                ComponentLight::Dir { words, scale: sc }
            } else if let Some(v) = pl.vertex_light {
                ComponentLight::Legacy(v)
            } else {
                continue;
            };
            out.insert(key, Arc::new(l));
        }
        // Components whose StaticMesh comes from an archetype are not placements but still carry baked light.
        if inherit {
            for (comp, li) in ue3::level::component_lights(&pkg) {
                let key = format!("{pkg_name}.{comp}");
                if out.contains_key(&key) {
                    continue;
                }
                let l = if let Some(lm) = li.lm2d.as_ref().filter(|l| l.texture_idx[0] > 0) {
                    let t = |k: usize| (lm.texture_idx[k] > 0).then(|| (pkg_name.to_string(), lm.texture_idx[k]));
                    ComponentLight::Map { textures: [t(0), t(1), t(2)], scale: [lm.scale[0], lm.scale[1], lm.scale[2]], coord_scale: lm.coord_scale, coord_bias: lm.coord_bias }
                } else if let Some((v, sc)) = li.dir {
                    let words = v.iter().flat_map(|b| b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))).collect();
                    ComponentLight::Dir { words, scale: sc }
                } else if let Some(v) = li.flat {
                    ComponentLight::Legacy(v)
                } else {
                    continue;
                };
                out.insert(key, Arc::new(l));
            }
        }
        let _ = std::fs::write(&file, encode_light_full(&out));
        Ok(out)
    }

    /// Baked per-vertex light of a placed StaticMeshComponent, by its full path (level package first).
    pub fn component_light(&self, component: &str) -> Option<Arc<Vec<[f32; 3]>>> {
        let (pkg, _) = component.split_once('.')?;
        let map = {
            let cached = self.lights.lock().unwrap().get(pkg).cloned();
            match cached {
                Some(m) => m,
                None => {
                    let m = Arc::new(self.load_level_light(pkg).unwrap_or_else(|e| {
                        if self.verbose {
                            eprintln!("assetdb: light {pkg}: {e:#}");
                        }
                        HashMap::new()
                    }));
                    self.lights
                        .lock()
                        .unwrap()
                        .insert(pkg.to_string(), m.clone());
                    m
                }
            }
        };
        map.get(component).cloned()
    }

    fn load_level_light(&self, pkg_name: &str) -> Result<HashMap<String, Arc<Vec<[f32; 3]>>>> {
        let file = self.cache.join("light").join(cache_name(pkg_name) + ".bin");
        if let Ok(b) = std::fs::read(&file) {
            if let Ok(m) = decode_light(&b) {
                return Ok(m);
            }
        }
        if !self.index.packages.iter().any(|(n, _)| n == pkg_name) {
            bail!("no package {pkg_name}");
        }
        let pkg = self.package(pkg_name)?;
        let mut out = HashMap::new();
        for (pl, err) in ue3::level::placements(&pkg) {
            if err.is_some() {
                continue;
            }
            if let Some(v) = pl.vertex_light {
                out.insert(format!("{pkg_name}.{}", pl.component), Arc::new(v));
            }
        }
        let _ = std::fs::write(&file, encode_light(&out));
        Ok(out)
    }

    /// A placed SpeedTreeComponent's baked light, by its full path (`<package>.<component path>`). The first call for a
    /// package opens it (slow; a disk cache makes later runs fast), so call this from a loader thread.
    pub fn tree_light(&self, component: &str) -> Option<Arc<TreeLight>> {
        let (pkg, _) = component.split_once('.')?;
        let cached = self.tree_lights.lock().unwrap().get(pkg).cloned();
        let map = match cached {
            Some(m) => m,
            None => {
                let m = Arc::new(self.load_tree_lights(pkg).unwrap_or_else(|e| {
                    if self.verbose {
                        eprintln!("assetdb: tree light {pkg}: {e:#}");
                    }
                    HashMap::new()
                }));
                self.tree_lights.lock().unwrap().insert(pkg.to_string(), m.clone());
                m
            }
        };
        map.get(component).cloned()
    }

    fn load_tree_lights(&self, pkg_name: &str) -> Result<HashMap<String, Arc<TreeLight>>> {
        let file = self.cache.join("treelight").join(cache_name(pkg_name) + ".bin");
        if let Ok(b) = std::fs::read(&file) {
            if let Ok(m) = decode_tree_lights(&b) {
                return Ok(m);
            }
        }
        if !self.index.packages.iter().any(|(n, _)| n == pkg_name) {
            bail!("no package {pkg_name}");
        }
        let pkg = self.package(pkg_name)?;
        let mut out = HashMap::new();
        for pl in ue3::speedtree::tree_placements(&pkg) {
            let mut t = TreeLight::default();
            for (k, l) in pl.lights.iter().take(4).enumerate() {
                if let Some(l) = l {
                    let words = l.samples.iter().flat_map(|b| b.chunks_exact(4).map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))).collect();
                    t.slots[k] = Some((words, l.scale));
                }
            }
            out.insert(format!("{pkg_name}.{}", pl.component), Arc::new(t));
        }
        let _ = std::fs::write(&file, encode_tree_lights(&out));
        Ok(out)
    }

    /// The materials of a SpeedTree asset by its object path (`AST_FoliageTree.Shrub04`; the package is the first
    /// segment). Opens the package on first use.
    pub fn tree_materials(&self, asset: &str) -> Option<Arc<TreeMaterials>> {
        if let Some(m) = self.tree_mats.lock().unwrap().get(asset) {
            return m.clone();
        }
        let found = match self.locate(asset, false, None) {
            Ok(Some((pkg, _))) => ue3::speedtree::tree_assets(&pkg)
                .into_iter()
                .find(|a| a.path == asset)
                .map(|a| Arc::new(TreeMaterials { branch: a.branch_material, frond: a.frond_material, leaf: a.leaf_material })),
            Ok(None) => None,
            Err(e) => {
                if self.verbose {
                    eprintln!("assetdb: tree asset {asset}: {e:#}");
                }
                None
            }
        };
        self.tree_mats.lock().unwrap().insert(asset.to_string(), found.clone());
        found
    }

    pub fn cache_dir(&self) -> &Path {
        &self.cache
    }
}

fn convert_texture(name: &str, mut t: ue3::texture::Texture) -> Result<TextureAsset> {
    use ue3::texture::Fmt;
    let fmt = match t.fmt {
        Some(Fmt::Dxt1) => TexFmt::Bc1,
        Some(Fmt::Dxt3) => TexFmt::Bc2,
        Some(Fmt::Dxt5) => TexFmt::Bc3,
        Some(Fmt::Bc5) => TexFmt::Bc5,
        Some(Fmt::G8) => TexFmt::R8,
        Some(Fmt::Rgba8) => TexFmt::Bgra8,
        None => bail!("unsupported format {}", t.format_name),
    };
    if t.mips.is_empty() {
        bail!("no mips");
    }
    while t.mips.len() > 1 && t.mips[0].0 > 2048 {
        t.mips.remove(0);
    }
    // BC formats need block-aligned mips: stop at the first that isn't.
    let (b, _) = fmt.block();
    let mut mips = Vec::new();
    let (w0, h0) = (t.mips[0].0 as u32, t.mips[0].1 as u32);
    if w0 % b != 0 || h0 % b != 0 {
        bail!("{w0}x{h0} not block-aligned");
    }
    for (i, (w, h, d)) in t.mips.into_iter().enumerate() {
        if (w as u32, h as u32) != ((w0 >> i).max(1), (h0 >> i).max(1)) || w as u32 % b != 0 || h as u32 % b != 0 {
            break;
        }
        mips.push(d);
    }
    let unpack = (t.unpack_min != [0.0; 3] || t.unpack_max != [1.0; 3]).then_some((t.unpack_min, t.unpack_max));
    Ok(TextureAsset {
        name: name.to_string(),
        fmt,
        srgb: t.srgb,
        width: w0,
        height: h0,
        mips,
        normal: t.compression.contains("Normalmap"),
        unpack,
    })
}

fn pick_diffuse(cands: &[String]) -> Option<String> {
    let score = |n: &str| -> i32 {
        let u = n.rsplit('.').next().unwrap_or(n).to_uppercase();
        if u.contains("DIFF") || u.contains("_DIF") || u.contains("DIFFUSE") {
            3
        } else if u.contains("NORM")
            || u.contains("SPEC")
            || u.ends_with("_N")
            || u.ends_with("_S")
            || u.contains("MASK")
            || u.contains("EFCT")
            || u.contains("BUMP")
        {
            0
        } else {
            1
        }
    };
    cands
        .iter()
        .max_by_key(|c| score(c))
        .filter(|c| score(c) > 0)
        .cloned()
}

/// The paths an export is known by: its Outer-chain path, and, when the chain doesn't start at a Package export
/// (the object lives in this file's own package), the same path prefixed with the file's name.
fn export_paths(pkg: &Package, i: i32) -> Vec<String> {
    let p = pkg.obj_path(i);
    let mut top = i;
    while pkg.outer_of(top) != 0 {
        top = pkg.outer_of(top);
    }
    if top == i || pkg.class_name(top) != "Package" {
        vec![format!("{}.{p}", pkg.name), p]
    } else {
        vec![p]
    }
}

fn build_index(files: &[(String, u64, PathBuf)]) -> Result<Index> {
    let threads = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(4)
        .min(8);
    let next = std::sync::atomic::AtomicUsize::new(0);
    type Part = Vec<(u32, Vec<(String, String)>)>;
    let parts: Vec<Part> = std::thread::scope(|s| {
        let hs: Vec<_> = (0..threads)
            .map(|_| {
                let next = &next;
                s.spawn(move || {
                    let mut out: Part = Vec::new();
                    loop {
                        let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                        let Some((_, _, path)) = files.get(i) else {
                            break;
                        };
                        let Ok(pkg) = Package::open(path) else {
                            continue;
                        };
                        let mut objs = Vec::new();
                        for e in 1..=pkg.exports.len() as i32 {
                            let cls = pkg.class_name(e);
                            if cls == "Texture2D" || INDEX_CLASSES.contains(&cls) {
                                for p in export_paths(&pkg, e) {
                                    objs.push((p, cls.to_string()));
                                }
                            }
                        }
                        out.push((i as u32, objs));
                    }
                    out
                })
            })
            .collect();
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    });
    let mut ix = Index {
        packages: files.iter().map(|(n, s, _)| (n.clone(), *s)).collect(),
        rev: INDEX_REV,
        ..Default::default()
    };
    let mut parts: Vec<_> = parts.into_iter().flatten().collect();
    parts.sort_by_key(|p| p.0);
    for (pi, objs) in parts {
        for (path, cls) in objs {
            let map = if cls == "Texture2D" {
                &mut ix.textures
            } else {
                &mut ix.objects
            };
            let v = map.entry(path.clone()).or_default();
            if !v.contains(&pi) {
                v.push(pi);
            }
            if cls != "Texture2D" {
                ix.classes.entry(path).or_insert(cls);
            }
        }
    }
    Ok(ix)
}

// ---- cache encodings (little-endian, versioned by magic)

struct W(Vec<u8>);
impl W {
    fn u32(&mut self, v: u32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn f32(&mut self, v: f32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn str(&mut self, s: &str) {
        self.u32(s.len() as u32);
        self.0.extend_from_slice(s.as_bytes());
    }
    fn bytes(&mut self, b: &[u8]) {
        self.u32(b.len() as u32);
        self.0.extend_from_slice(b);
    }
}
struct R<'a>(&'a [u8], usize);
impl<'a> R<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        if self.1 + n > self.0.len() {
            bail!("truncated");
        }
        let s = &self.0[self.1..self.1 + n];
        self.1 += n;
        Ok(s)
    }
    fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into()?))
    }
    fn f32(&mut self) -> Result<f32> {
        Ok(f32::from_le_bytes(self.take(4)?.try_into()?))
    }
    fn str(&mut self) -> Result<String> {
        let n = self.u32()? as usize;
        Ok(String::from_utf8(self.take(n)?.to_vec())?)
    }
    fn bytes(&mut self) -> Result<&'a [u8]> {
        let n = self.u32()? as usize;
        self.take(n)
    }
}

const MESH_MAGIC: u32 = 0x4853_4D32; // "2MSH": + tangents, colours, uv1
const TEX_MAGIC: u32 = 0x5845_5432; // + normal, unpack
const LIGHT_MAGIC: u32 = 0x5447_4C31;
const TREELIGHT_MAGIC: u32 = 0x5447_4C33;

fn encode_tree_lights(m: &HashMap<String, Arc<TreeLight>>) -> Vec<u8> {
    let mut w = W(Vec::new());
    w.u32(TREELIGHT_MAGIC);
    w.u32(m.len() as u32);
    for (k, v) in m {
        w.str(k);
        for s in &v.slots {
            match s {
                Some((words, sc)) => {
                    w.u32(words.len() as u32 + 1);
                    words.iter().for_each(|&x| w.u32(x));
                    sc.iter().flatten().for_each(|&x| w.f32(x));
                }
                None => w.u32(0),
            }
        }
    }
    w.0
}

fn decode_tree_lights(b: &[u8]) -> Result<HashMap<String, Arc<TreeLight>>> {
    let mut r = R(b, 0);
    if r.u32()? != TREELIGHT_MAGIC {
        bail!("magic");
    }
    let n = r.u32()? as usize;
    let mut out = HashMap::with_capacity(n);
    for _ in 0..n {
        let k = r.str()?;
        let mut t = TreeLight::default();
        for slot in t.slots.iter_mut() {
            let c = r.u32()? as usize;
            if c == 0 {
                continue;
            }
            let mut words = Vec::with_capacity(c - 1);
            for _ in 1..c {
                words.push(r.u32()?);
            }
            let mut sc = [[0f32; 3]; 3];
            for s in sc.iter_mut() {
                for x in s.iter_mut() {
                    *x = r.f32()?;
                }
            }
            *slot = Some((words, sc));
        }
        out.insert(k, Arc::new(t));
    }
    Ok(out)
}

const LIGHT2_MAGIC: u32 = 0x5447_4C32;

fn encode_light_full(m: &HashMap<String, Arc<ComponentLight>>) -> Vec<u8> {
    let mut w = W(Vec::new());
    w.u32(LIGHT2_MAGIC);
    w.u32(m.len() as u32);
    let f3 = |w: &mut W, s: &[[f32; 3]]| s.iter().flatten().for_each(|&x| w.f32(x));
    for (k, v) in m {
        w.str(k);
        match &**v {
            ComponentLight::Legacy(c) => {
                w.u32(0);
                w.u32(c.len() as u32);
                f3(&mut w, c);
            }
            ComponentLight::Dir { words, scale } => {
                w.u32(1);
                w.u32(words.len() as u32);
                words.iter().for_each(|&x| w.u32(x));
                f3(&mut w, scale);
            }
            ComponentLight::Map { textures, scale, coord_scale, coord_bias } => {
                w.u32(2);
                for t in textures {
                    match t {
                        Some((p, i)) => {
                            w.str(p);
                            w.u32(*i as u32);
                        }
                        None => {
                            w.str("");
                            w.u32(0);
                        }
                    }
                }
                f3(&mut w, scale);
                coord_scale.iter().chain(coord_bias).for_each(|&x| w.f32(x));
            }
        }
    }
    w.0
}

fn decode_light_full(b: &[u8]) -> Result<HashMap<String, Arc<ComponentLight>>> {
    let mut r = R(b, 0);
    if r.u32()? != LIGHT2_MAGIC {
        bail!("magic");
    }
    let n = r.u32()? as usize;
    let mut out = HashMap::with_capacity(n);
    let f3 = |r: &mut R| -> Result<[f32; 3]> { Ok([r.f32()?, r.f32()?, r.f32()?]) };
    for _ in 0..n {
        let k = r.str()?;
        let l = match r.u32()? {
            0 => {
                let nv = r.u32()? as usize;
                ComponentLight::Legacy((0..nv).map(|_| f3(&mut r)).collect::<Result<_>>()?)
            }
            1 => {
                let nw = r.u32()? as usize;
                let words = (0..nw).map(|_| r.u32()).collect::<Result<_>>()?;
                ComponentLight::Dir { words, scale: [f3(&mut r)?, f3(&mut r)?, f3(&mut r)?] }
            }
            2 => {
                let mut textures: [Option<(String, i32)>; 3] = Default::default();
                for t in textures.iter_mut() {
                    let p = r.str()?;
                    let i = r.u32()? as i32;
                    *t = (!p.is_empty()).then_some((p, i));
                }
                let scale = [f3(&mut r)?, f3(&mut r)?, f3(&mut r)?];
                let coord_scale = [r.f32()?, r.f32()?];
                let coord_bias = [r.f32()?, r.f32()?];
                ComponentLight::Map { textures, scale, coord_scale, coord_bias }
            }
            x => bail!("light kind {x}"),
        };
        out.insert(k, Arc::new(l));
    }
    Ok(out)
}

fn encode_mesh(m: &MeshAsset) -> Vec<u8> {
    let mut w = W(Vec::new());
    w.u32(MESH_MAGIC);
    w.u32(m.positions.len() as u32);
    w.u32(m.indices.len() as u32);
    for p in &m.positions {
        p.iter().for_each(|&v| w.f32(v));
    }
    for i in 0..m.positions.len() {
        let n = m.normals.get(i).copied().unwrap_or([0.0, 0.0, 1.0]);
        n.iter().for_each(|&v| w.f32(v));
    }
    for i in 0..m.positions.len() {
        let t = m.uvs.get(i).copied().unwrap_or([0.0, 0.0]);
        t.iter().for_each(|&v| w.f32(v));
    }
    for &i in &m.indices {
        w.u32(i);
    }
    w.u32(m.sections.len() as u32);
    for s in &m.sections {
        w.u32(s.first_index);
        w.u32(s.index_count);
        w.str(&s.material);
    }
    m.center.iter().for_each(|&v| w.f32(v));
    w.f32(m.radius);
    for i in 0..m.positions.len() {
        m.tangents.get(i).copied().unwrap_or([1.0, 0.0, 0.0, 1.0]).iter().for_each(|&v| w.f32(v));
    }
    for i in 0..m.positions.len() {
        w.0.extend_from_slice(&m.colors.get(i).copied().unwrap_or([255; 4]));
    }
    for i in 0..m.positions.len() {
        m.uv1.get(i).copied().unwrap_or([0.0; 2]).iter().for_each(|&v| w.f32(v));
    }
    w.0
}

fn decode_mesh(name: &str, b: &[u8]) -> Result<MeshAsset> {
    let mut r = R(b, 0);
    if r.u32()? != MESH_MAGIC {
        bail!("magic");
    }
    let (nv, ni) = (r.u32()? as usize, r.u32()? as usize);
    let v3 = |r: &mut R| -> Result<Vec<[f32; 3]>> {
        (0..nv)
            .map(|_| Ok([r.f32()?, r.f32()?, r.f32()?]))
            .collect()
    };
    let positions = v3(&mut r)?;
    let normals = v3(&mut r)?;
    let uvs = (0..nv)
        .map(|_| Ok([r.f32()?, r.f32()?]))
        .collect::<Result<Vec<_>>>()?;
    let indices = (0..ni).map(|_| r.u32()).collect::<Result<Vec<_>>>()?;
    let ns = r.u32()? as usize;
    let sections = (0..ns)
        .map(|_| {
            Ok(SectionAsset {
                first_index: r.u32()?,
                index_count: r.u32()?,
                material: r.str()?,
            })
        })
        .collect::<Result<Vec<_>>>()?;
    let center = [r.f32()?, r.f32()?, r.f32()?];
    let radius = r.f32()?;
    let tangents = (0..nv).map(|_| Ok([r.f32()?, r.f32()?, r.f32()?, r.f32()?])).collect::<Result<Vec<_>>>()?;
    let colors = (0..nv).map(|_| Ok(<[u8; 4]>::try_from(r.take(4)?)?)).collect::<Result<Vec<_>>>()?;
    let uv1 = (0..nv).map(|_| Ok([r.f32()?, r.f32()?])).collect::<Result<Vec<_>>>()?;
    Ok(MeshAsset {
        name: name.to_string(),
        tangents,
        colors,
        uv1,
        positions,
        normals,
        uvs,
        indices,
        sections,
        center,
        radius,
    })
}

fn encode_texture(t: &TextureAsset) -> Vec<u8> {
    let mut w = W(Vec::new());
    w.u32(TEX_MAGIC);
    w.u32(t.fmt as u32);
    w.u32(t.srgb as u32);
    w.u32(t.width);
    w.u32(t.height);
    w.u32(t.mips.len() as u32);
    for m in &t.mips {
        w.bytes(m);
    }
    w.u32(t.normal as u32);
    let (mn, mx) = t.unpack.unwrap_or(([0.0; 3], [1.0; 3]));
    w.u32(t.unpack.is_some() as u32);
    mn.iter().chain(&mx).for_each(|&v| w.f32(v));
    w.0
}

fn decode_texture(name: &str, b: &[u8]) -> Result<TextureAsset> {
    let mut r = R(b, 0);
    if r.u32()? != TEX_MAGIC {
        bail!("magic");
    }
    let fmt = match r.u32()? {
        0 => TexFmt::Bc1,
        1 => TexFmt::Bc2,
        2 => TexFmt::Bc3,
        3 => TexFmt::Bc5,
        4 => TexFmt::R8,
        5 => TexFmt::Bgra8,
        x => bail!("fmt {x}"),
    };
    let srgb = r.u32()? != 0;
    let (width, height) = (r.u32()?, r.u32()?);
    let n = r.u32()? as usize;
    let mips = (0..n)
        .map(|_| Ok(r.bytes()?.to_vec()))
        .collect::<Result<Vec<_>>>()?;
    let normal = r.u32()? != 0;
    let has_unpack = r.u32()? != 0;
    let mn = [r.f32()?, r.f32()?, r.f32()?];
    let mx = [r.f32()?, r.f32()?, r.f32()?];
    Ok(TextureAsset {
        normal,
        unpack: has_unpack.then_some((mn, mx)),
        name: name.to_string(),
        fmt,
        srgb,
        width,
        height,
        mips,
    })
}

fn encode_light(m: &HashMap<String, Arc<Vec<[f32; 3]>>>) -> Vec<u8> {
    let mut w = W(Vec::new());
    w.u32(LIGHT_MAGIC);
    w.u32(m.len() as u32);
    for (k, v) in m {
        w.str(k);
        w.u32(v.len() as u32);
        for c in v.iter() {
            c.iter().for_each(|&x| w.f32(x));
        }
    }
    w.0
}

fn decode_light(b: &[u8]) -> Result<HashMap<String, Arc<Vec<[f32; 3]>>>> {
    let mut r = R(b, 0);
    if r.u32()? != LIGHT_MAGIC {
        bail!("magic");
    }
    let n = r.u32()? as usize;
    let mut out = HashMap::with_capacity(n);
    for _ in 0..n {
        let k = r.str()?;
        let nv = r.u32()? as usize;
        let v = (0..nv)
            .map(|_| Ok([r.f32()?, r.f32()?, r.f32()?]))
            .collect::<Result<Vec<_>>>()?;
        out.insert(k, Arc::new(v));
    }
    Ok(out)
}

/// The first hit whose class is `want`, else the first hit (none wanted: the first). Hits are taken lazily, so packages
/// after a match are never opened.
fn pick_class<T>(
    want: Option<&str>,
    hits: impl Iterator<Item = Result<T>>,
    class_is: impl Fn(&T, &str) -> bool,
) -> Result<Option<T>> {
    let mut first = None;
    for hit in hits {
        let hit = hit?;
        if want.is_none_or(|w| class_is(&hit, w)) {
            return Ok(Some(hit));
        }
        first.get_or_insert(hit);
    }
    Ok(first)
}

#[cfg(test)]
mod pick_tests {
    use super::pick_class;

    // (package, class of the path's export there): the shapes of the 61509 helicopter, where a level package holds a
    // StaticMesh copy of the path and the vehicle package holds the SkeletalMesh.
    fn hits(v: &[(&'static str, &'static str)]) -> impl Iterator<Item = anyhow::Result<(&'static str, &'static str)>> {
        v.to_vec().into_iter().map(Ok)
    }
    fn is(h: &(&str, &str), c: &str) -> bool {
        h.1 == c
    }

    #[test]
    fn picks_the_wanted_class_over_an_earlier_package() {
        let v = [("level_a", "StaticMesh"), ("vehicles", "SkeletalMesh")];
        let got = pick_class(Some("SkeletalMesh"), hits(&v), is).unwrap();
        assert_eq!(got, Some(("vehicles", "SkeletalMesh")));
    }

    #[test]
    fn no_class_match_falls_back_to_the_first_hit() {
        let v = [("level_a", "StaticMesh"), ("level_b", "StaticMesh")];
        assert_eq!(pick_class(Some("SkeletalMesh"), hits(&v), is).unwrap(), Some(("level_a", "StaticMesh")));
    }

    #[test]
    fn no_wanted_class_is_the_first_hit_and_nothing_is_none() {
        let v = [("level_a", "Texture2D"), ("level_b", "StaticMesh")];
        assert_eq!(pick_class(None, hits(&v), is).unwrap(), Some(("level_a", "Texture2D")));
        assert_eq!(pick_class(Some("StaticMesh"), hits(&[]), is).unwrap(), None);
    }

    #[test]
    fn stops_at_the_first_match() {
        let seen = std::cell::Cell::new(0);
        let it = ["StaticMesh", "SkeletalMesh", "SkeletalMesh"].into_iter().map(|c| {
            seen.set(seen.get() + 1);
            anyhow::Ok(("p", c))
        });
        assert!(pick_class(Some("SkeletalMesh"), it, is).unwrap().is_some());
        assert_eq!(seen.get(), 2);
    }
}
