//! Material IR: a flattened, renderer-friendly form of a UE3 material (or instance) graph.
//!
//! `build` resolves a `Material` / `MaterialInstanceConstant` chain (parameters folded into constants and textures)
//! and compiles the expression graph into a topologically ordered node list (`MatDef`). Everything is a vec4 in the
//! renderer; a single-component mask splats. The renderer turns the node list into WGSL (see n8gfx).

use crate::material::{Input, MatGraph, read_material};
use crate::package::Package;
use crate::props::{object_props, read_fname, read_props};
use std::collections::HashMap;

pub use matdef::*;

/// Material instance parameter values by lowercase parameter name. The first value set wins (children are visited
/// before their parents).
#[derive(Default, Clone, Debug)]
pub struct Params {
    pub scalar: HashMap<String, f32>,
    pub vector: HashMap<String, [f32; 4]>,
    pub texture: HashMap<String, String>, // param name (lowercase) -> texture path
    pub switch: HashMap<String, bool>,
}

/// Adds a `MaterialInstanceConstant`'s own parameter values to `into` (names already there are kept).
pub fn instance_params(pkg: &Package, idx: i32, into: &mut Params) {
    collect_params(pkg, idx, into);
}

/// The runtime parameter values a path carries after `?` (see [`split_overrides`]), and the cooked path.
pub fn path_overrides(path: &str) -> (&str, Params) {
    split_overrides(path)
}

/// Splits `"<material path>?s:Name=0.5&v:Name=r,g,b,a&t:Name=Pkg.Tex"` into the cooked path and the runtime
/// parameter values (the live scene feed's encoding of a run-time `MaterialInstanceConstant`). Values given here
/// take precedence over every instance in the parent chain, as the run-time instance is the outermost child.
fn split_overrides(path: &str) -> (&str, Params) {
    let mut params = Params::default();
    let Some((base, rest)) = path.split_once('?') else {
        return (path, params);
    };
    for e in rest.split('&') {
        let Some((kind, kv)) = e.split_once(':') else { continue };
        let Some((name, val)) = kv.split_once('=') else { continue };
        let name = name.to_lowercase();
        match kind {
            "s" => {
                if let Ok(v) = val.parse() {
                    params.scalar.insert(name, v);
                }
            }
            "v" => {
                let c: Vec<f32> = val.split(',').filter_map(|x| x.parse().ok()).collect();
                if c.len() == 4 {
                    params.vector.insert(name, [c[0], c[1], c[2], c[3]]);
                }
            }
            "t" if !val.is_empty() => {
                params.texture.insert(name, val.to_string());
            }
            _ => {}
        }
    }
    (base, params)
}

/// The cooked material path of a possibly parameterised path (see [`split_overrides`]).
pub fn base_path(path: &str) -> &str {
    path.split_once('?').map_or(path, |x| x.0)
}

pub struct Resolver<'a> {
    pub pkgs: &'a [Package],
    pub mats: &'a HashMap<String, (usize, i32)>,
}

fn array_elems<'a>(pkg: &Package, data: &'a [u8]) -> Vec<crate::props::Props<'a>> {
    let mut r = crate::package::Reader::new(data, 0);
    let n = r.i32().unwrap_or(0).clamp(0, 4096);
    let mut out = Vec::new();
    for _ in 0..n {
        match read_props(pkg, &mut r) {
            Ok(p) => out.push(p),
            Err(_) => break,
        }
    }
    out
}

fn collect_params(pkg: &Package, idx: i32, into: &mut Params) {
    let Ok((props, _)) = object_props(pkg, idx) else { return };
    let name_of = |e: &crate::props::Props| -> String {
        e.get("ParameterName").and_then(|p| read_fname(pkg, &mut p.r()).ok()).unwrap_or_default().to_lowercase()
    };
    for p in &props.list {
        match p.name.as_str() {
            "ScalarParameterValues" => {
                for e in array_elems(pkg, p.data) {
                    let n = name_of(&e);
                    if let Some(v) = e.get("ParameterValue") {
                        into.scalar.entry(n).or_insert(v.as_f32());
                    }
                }
            }
            "VectorParameterValues" => {
                for e in array_elems(pkg, p.data) {
                    let n = name_of(&e);
                    if let Some(v) = e.get("ParameterValue") {
                        let mut r = v.r();
                        if let (Ok(a), Ok(b), Ok(c), Ok(d)) = (r.f32(), r.f32(), r.f32(), r.f32()) {
                            into.vector.entry(n).or_insert([a, b, c, d]);
                        }
                    }
                }
            }
            "TextureParameterValues" => {
                for e in array_elems(pkg, p.data) {
                    let n = name_of(&e);
                    if let Some(v) = e.get("ParameterValue") {
                        let o = v.as_obj();
                        if o != 0 {
                            into.texture.entry(n).or_insert(pkg.obj_path(o));
                        }
                    }
                }
            }
            _ => {}
        }
    }
}

impl Resolver<'_> {
    /// Resolve `path` to a flattened definition. Returns None if the material isn't found.
    pub fn build(&self, path: &str) -> Option<MatDef> {
        build_by(path, |q| self.mats.get(q).map(|&(pi, ei)| (&self.pkgs[pi], ei)))
    }
}

/// `Resolver::build` over any package lookup (`path -> (package, export index)`), e.g. a lazily opened set.
///
/// `path` may carry runtime parameter overrides after a `?` (see [`split_overrides`]): a material instance the game
/// made at run time (no cooked package holds it) is fed as its first cooked parent plus its own parameter values.
pub fn build_by<P: std::ops::Deref<Target = Package>>(path: &str, lookup: impl Fn(&str) -> Option<(P, i32)>) -> Option<MatDef> {
    {
        let (base, mut params) = split_overrides(path);
        let mut cur_path = base.to_string();
        let mut chain_class = String::new();
        for depth in 0..8 {
            let (pp, ei) = lookup(&cur_path)?;
            let p: &Package = &pp;
            let class = p.class_name(ei).to_string();
            if chain_class.is_empty() {
                chain_class = class.clone();
            }
            if class == "MaterialInstanceConstant" {
                collect_params(p, ei, &mut params);
                let (props, _) = object_props(p, ei).ok()?;
                let parent = props.get("Parent").map(|x| x.as_obj()).filter(|&o| o != 0)?;
                cur_path = p.obj_path(parent);
                let _ = depth;
                continue;
            }
            let g = read_material(p, ei).ok()?;
            let mut def = compile(p, &g, &params);
            def.path = path.to_string();
            def.class = chain_class;
            return Some(def);
        }
        None
    }
}

/// A material's named parameters after the MIC chain: (base Material path, scalars, vectors), names lowercase.
/// Instance overrides win over the base material's expression defaults; a vector parameter without a DefaultValue
/// is zero, as in the engine. Used for post-process materials the renderer reproduces as fixed-function passes.
pub type NamedParams = (String, HashMap<String, f32>, HashMap<String, [f32; 4]>);

pub fn params_by<P: std::ops::Deref<Target = Package>>(path: &str, lookup: impl Fn(&str) -> Option<(P, i32)>) -> Option<NamedParams> {
    let mut params = Params::default();
    let mut cur_path = path.to_string();
    for _ in 0..8 {
        let (pp, ei) = lookup(&cur_path)?;
        let p: &Package = &pp;
        if p.class_name(ei) == "MaterialInstanceConstant" {
            collect_params(p, ei, &mut params);
            let (props, _) = object_props(p, ei).ok()?;
            let parent = props.get("Parent").map(|x| x.as_obj()).filter(|&o| o != 0)?;
            cur_path = p.obj_path(parent);
            continue;
        }
        let g = read_material(p, ei).ok()?;
        for e in g.exprs.values() {
            let Some(n) = e.names.get("ParameterName").map(|s| s.to_lowercase()) else { continue };
            match e.class.as_str() {
                "ScalarParameter" => {
                    params.scalar.entry(n).or_insert(e.floats.get("DefaultValue").copied().unwrap_or(0.0));
                }
                "VectorParameter" => {
                    params.vector.entry(n).or_insert(e.colors.get("DefaultValue").copied().unwrap_or([0.0; 4]));
                }
                _ => {}
            }
        }
        return Some((cur_path, params.scalar, params.vector));
    }
    None
}

struct Comp<'a> {
    pkg: &'a Package,
    g: &'a MatGraph,
    params: &'a Params,
    def: MatDef,
    memo: HashMap<(i32, i32, Option<[bool; 4]>), u32>,
    depth: u32,
}

const DEFAULT_COL: [f32; 4] = [0.5, 0.5, 0.5, 1.0];

impl Comp<'_> {
    fn push(&mut self, n: Node) -> u32 {
        // Dedupe identical nodes, except constants: merging two that happen to hold the same value (two parameters both
        // 0, say) changes the graph's shape, so the shader, with the values. A run-time MIC then needs a new shader for
        // each set of values (B-203: the technical's DamageZone_* parameters built pipelines in play in 61465 on every
        // run). Constants are uniforms in the shader (`mp.c[]`), so keeping them apart costs nothing.
        if !matches!(n, Node::Const { .. })
            && let Some(i) = self.def.nodes.iter().position(|x| *x == n)
        {
            return i as u32;
        }
        self.def.nodes.push(n);
        self.def.nodes.len() as u32 - 1
    }
    fn cst(&mut self, v: f32) -> u32 {
        self.push(Node::Const { v: [v; 4], n: 0 })
    }
    fn tex_slot(&mut self, path: String) -> u32 {
        if let Some(i) = self.def.textures.iter().position(|t| t.path == path) {
            return i as u32;
        }
        self.def.textures.push(TexRef { path, normal: false });
        self.def.textures.len() as u32 - 1
    }
    fn unsup(&mut self, class: &str) -> u32 {
        if !self.def.unsupported.iter().any(|c| c == class) {
            self.def.unsupported.push(class.to_string());
        }
        self.push(Node::Unsup { class: class.to_string() })
    }

    fn input(&mut self, e: &crate::material::Expr, name: &str) -> Option<u32> {
        let i = e.input(name)?.clone();
        Some(self.conn(&i))
    }

    fn conn(&mut self, i: &Input) -> u32 {
        let n = self.expr(i.expr, i.output);
        match i.mask {
            Some(m) if m != [true, true, true, true] && m != [false; 4] => self.push(Node::Mask { x: n, m }),
            _ => n,
        }
    }

    fn expr(&mut self, ei: i32, out: i32) -> u32 {
        if let Some(&n) = self.memo.get(&(ei, out, None)) {
            return n;
        }
        self.depth += 1;
        let r = if self.depth > 64 { self.cst(0.0) } else { self.expr_inner(ei, out) };
        self.depth -= 1;
        self.memo.insert((ei, out, None), r);
        r
    }

    fn expr_inner(&mut self, ei: i32, out: i32) -> u32 {
        let Some(e) = self.g.exprs.get(&ei).cloned() else { return self.unsup("missing") };
        let cls = e.class.trim_start_matches("MaterialExpression").trim_start_matches("Sf").to_string();
        let f = |k: &str, d: f32| e.floats.get(k).copied().unwrap_or(d);
        let b = |k: &str| e.bools.get(k).copied().unwrap_or(false);
        let pname = e.names.get("ParameterName").map(|s| s.to_lowercase()).unwrap_or_default();
        match cls.as_str() {
            "Constant" => self.push(Node::Const { v: [f("R", 0.0); 4], n: 0 }),
            "Constant2Vector" => self.push(Node::Const { v: [f("R", 0.0), f("G", 0.0), 0.0, 0.0], n: 0 }),
            "Constant3Vector" => self.push(Node::Const { v: [f("R", 0.0), f("G", 0.0), f("B", 0.0), 1.0], n: 0 }),
            "ScalarParameter" => {
                let v = self.params.scalar.get(&pname).copied().unwrap_or(f("DefaultValue", 0.0));
                self.push(Node::Const { v: [v; 4], n: 1 })
            }
            "VectorParameter" => {
                let d = e.colors.get("DefaultValue").copied().unwrap_or([1.0, 0.0, 0.0, 0.0]);
                let v = self.params.vector.get(&pname).copied().unwrap_or(d);
                self.push(Node::Const { v, n: 3 })
            }
            "TextureSample" | "TextureSampleParameter2D" => {
                let mut tp = e.objs.get("Texture").filter(|&&o| o != 0).map(|&o| self.pkg.obj_path(o));
                if cls == "TextureSampleParameter2D" {
                    if let Some(t) = self.params.texture.get(&pname) {
                        tp = Some(t.clone());
                    }
                }
                let Some(tp) = tp else { return self.push(Node::Const { v: DEFAULT_COL, n: 0 }) };
                let uv = self.input(&e, "Coordinates");
                let tex = self.tex_slot(tp);
                self.push(Node::Tex { tex, uv })
            }
            "TextureCoordinate" => {
                let set = e.ints.get("CoordinateIndex").copied().unwrap_or(0) as u32;
                if set >= 1 {
                    self.def.uses_uv1 = true;
                }
                self.push(Node::TexCoord { set, u: f("UTiling", 1.0), v: f("VTiling", 1.0) })
            }
            "TextureCoordinate2" => {
                let set = e.ints.get("CoordinateIndex").copied().unwrap_or(0) as u32;
                if set >= 1 {
                    self.def.uses_uv1 = true;
                }
                self.push(Node::TexCoord { set, u: f("TilingU", 1.0), v: f("TilingV", 1.0) })
            }
            "VertexColor" => {
                self.def.uses_vcolor = true;
                self.push(Node::VColor)
            }
            "MeshEmitterVertexColor" => self.push(Node::Const { v: [1.0; 4], n: 0 }),
            "Add" | "Subtract" | "Multiply" | "Divide" | "Power" | "DotProduct" => {
                let a = self.input(&e, if cls == "Power" { "Base" } else { "A" });
                let b2 = self.input(&e, if cls == "Power" { "Exponent" } else { "B" });
                let a = a.unwrap_or_else(|| self.cst(0.0));
                let b2 = b2.unwrap_or_else(|| self.cst(if cls == "Multiply" || cls == "Divide" { 1.0 } else { 0.0 }));
                self.push(match cls.as_str() {
                    "Add" => Node::Add { a, b: b2 },
                    "Subtract" => Node::Sub { a, b: b2 },
                    "Multiply" => Node::Mul { a, b: b2 },
                    "Divide" => Node::Div { a, b: b2 },
                    "Power" => Node::Pow { a, b: b2 },
                    _ => Node::Dot { a, b: b2 },
                })
            }
            "MultiplyConstant" => {
                let x = self.input(&e, "Input").unwrap_or_else(|| self.cst(1.0));
                let c = self.cst(f("MultiplierR", 1.0));
                self.push(Node::Mul { a: x, b: c })
            }
            "MultiplyConstant3" => {
                let x = self.input(&e, "Input").unwrap_or_else(|| self.cst(1.0));
                let c = self.push(Node::Const { v: [f("MultiplierR", 1.0), f("MultiplierG", 1.0), f("MultiplierB", 1.0), 1.0], n: 0 });
                self.push(Node::Mul { a: x, b: c })
            }
            "LinearInterpolate" => {
                let a = self.input(&e, "A").unwrap_or_else(|| self.cst(0.0));
                let b2 = self.input(&e, "B").unwrap_or_else(|| self.cst(1.0));
                let t = self.input(&e, "Alpha").unwrap_or_else(|| self.cst(0.5));
                self.push(Node::Lerp { a, b: b2, t })
            }
            "Clamp" => {
                let x = self.input(&e, "Input").unwrap_or_else(|| self.cst(0.0));
                let lo = self.input(&e, "Min").unwrap_or_else(|| self.cst(0.0));
                let hi = self.input(&e, "Max").unwrap_or_else(|| self.cst(1.0));
                self.push(Node::Clamp { x, lo, hi })
            }
            "Clamp2" | "ConstantClamp" => {
                let x = self.input(&e, "Input").unwrap_or_else(|| self.cst(0.0));
                self.push(Node::Sat { x, lo: f("ClampMin", 0.0), hi: f("ClampMax", 1.0) })
            }
            "OneMinus" | "Abs" | "Frac" | "Floor" | "Ceil" | "Normalize" | "SquareRoot" | "TimesHalfPlusHalf" => {
                let x = self.input(&e, if cls == "Normalize" { "VectorInput" } else { "Input" }).unwrap_or_else(|| self.cst(0.0));
                let fname = match cls.as_str() {
                    "OneMinus" => "oneminus",
                    "Abs" => "abs",
                    "Frac" => "frac",
                    "Floor" => "floor",
                    "Ceil" => "ceil",
                    "Normalize" => "normalize",
                    "SquareRoot" => "sqrt",
                    _ => "half",
                };
                self.push(Node::Un { f: fname.into(), x })
            }
            "Sine" | "Cosine" => {
                let x = self.input(&e, "Input").unwrap_or_else(|| self.cst(0.0));
                self.push(Node::Trig { cos: cls == "Cosine", x, period: f("Period", 1.0) })
            }
            "ComponentMask" => {
                let x = self.input(&e, "Input").unwrap_or_else(|| self.cst(0.0));
                self.push(Node::Mask { x, m: [b("R"), b("G"), b("B"), b("A")] })
            }
            "AppendVector" => {
                let a = self.input(&e, "A").unwrap_or_else(|| self.cst(0.0));
                let b2 = self.input(&e, "B").unwrap_or_else(|| self.cst(0.0));
                self.push(Node::Append { a, b: b2 })
            }
            "Desaturation" => {
                let x = self.input(&e, "Input").unwrap_or_else(|| self.cst(0.0));
                let pct = self.input(&e, "Percent");
                let w = e.colors.get("LuminanceFactors").map(|c| [c[0], c[1], c[2]]).unwrap_or([0.0, 0.11, 0.59]);
                // BotS default factors (R=0, G=0.11, B=0.59, A=0.3) look scrambled; the engine default is (.3,.59,.11).
                let w = if e.colors.contains_key("LuminanceFactors") { w } else { [0.3, 0.59, 0.11] };
                self.push(Node::Desat { x, pct, w })
            }
            "Time" => self.push(Node::Time),
            "Panner" | "Panner2" => {
                let coord = self.input(&e, "Coordinate");
                let time = self.input(&e, "Time");
                self.push(Node::Panner { coord, time, sx: f("SpeedX", 0.0), sy: f("SpeedY", 0.0) })
            }
            "Rotator" => {
                let coord = self.input(&e, "Coordinate");
                let time = self.input(&e, "Time");
                self.push(Node::Rotator { coord, time, cx: f("CenterX", 0.5), cy: f("CenterY", 0.5), speed: f("Speed", 0.25) })
            }
            "BumpOffset" => {
                let coord = self.input(&e, "Coordinate").unwrap_or_else(|| self.push(Node::TexCoord { set: 0, u: 1.0, v: 1.0 }));
                let height = self.input(&e, "Height").unwrap_or_else(|| self.cst(0.5));
                self.push(Node::BumpOffset { coord, height, ratio: f("HeightRatio", 0.05), plane: f("ReferencePlane", 0.5) })
            }
            "CameraVector" => self.push(Node::CameraVec),
            "ReflectionVector" => self.push(Node::ReflVec),
            "LightVector" => self.push(Node::LightVec),
            "Fresnel" => {
                let n = self.input(&e, "Normal");
                self.push(Node::Fresnel { exp: f("Exponent", 3.0), n })
            }
            "WorldPosition" => self.push(Node::WorldPos),
            "PixelDepth" => self.push(Node::PixelDepth),
            "If" => {
                let a = self.input(&e, "A").unwrap_or_else(|| self.cst(0.0));
                let b2 = self.input(&e, "B").unwrap_or_else(|| self.cst(0.0));
                let gt = self.input(&e, "AGreaterThanB").unwrap_or_else(|| self.cst(0.0));
                let eq = self.input(&e, "AEqualsB").unwrap_or_else(|| self.cst(0.0));
                let lt = self.input(&e, "ALessThanB").unwrap_or_else(|| self.cst(0.0));
                self.push(Node::If { a, b: b2, gt, eq, lt })
            }
            "StaticSwitchParameter" => {
                let on = self.params.switch.get(&pname).copied().unwrap_or(b("DefaultValue"));
                let pick = if on { "A" } else { "B" };
                self.input(&e, pick).unwrap_or_else(|| self.cst(0.0))
            }
            // SfMaterialExpressionPlatformSpecific passes its input only on the platforms ticked (IsXbox, IsPS3, IsPC; all
            // default false). We render the Xbox build, so an unticked IsXbox is 0: e.g. TemplateBody_Mat's emissive rim
            // (IsPS3 + IsPC only), which the port never draws (W4).
            "PlatformSpecific" | "MaterialExpressionPlatformSpecific" => {
                if b("IsXbox") { self.input(&e, "Input").unwrap_or_else(|| self.cst(0.0)) } else { self.cst(0.0) }
            }
            "Transform" => self.input(&e, "Input").unwrap_or_else(|| self.cst(0.0)),
            "DepthBiasedAlpha" => self.input(&e, "Alpha").unwrap_or_else(|| self.cst(1.0)),
            _ => {
                let _ = out;
                self.unsup(&e.class)
            }
        }
    }
}

fn compile(pkg: &Package, g: &MatGraph, params: &Params) -> MatDef {
    let mut c = Comp {
        pkg,
        g,
        params,
        def: MatDef {
            blend: g.blend.clone(),
            lighting: g.lighting_model.clone(),
            two_sided: g.two_sided,
            clip: g.clip,
            ..Default::default()
        },
        memo: HashMap::new(),
        depth: 0,
    };
    let get = |c: &mut Comp, n: &str| -> Option<u32> {
        let i = g.input(n)?.clone();
        Some(c.conn(&i))
    };
    c.def.diffuse = get(&mut c, "DiffuseColor");
    c.def.specular = get(&mut c, "SpecularColor");
    c.def.spec_power = get(&mut c, "SpecularPower");
    c.def.normal = get(&mut c, "Normal");
    c.def.emissive = get(&mut c, "EmissiveColor");
    c.def.opacity = get(&mut c, "Opacity");
    c.def.opacity_mask = get(&mut c, "OpacityMask");
    for (n, i) in &g.inputs {
        if i.expr == 0 && i.use_constant {
            match n.as_str() {
                "DiffuseColor" => c.def.diffuse_const = Some(i.constant),
                "SpecularPower" => c.def.spec_power_const = Some(i.constant[0]),
                "Opacity" => c.def.opacity_const = Some(i.constant[0]),
                _ => {}
            }
        }
    }
    let mut def = c.def;
    // Texture roles: a texture feeding (only) a Normal input is a normal map; the extractor refines this from the
    // texture's own CompressionSettings.
    def.textures.iter_mut().for_each(|t| t.normal = false);
    def
}
