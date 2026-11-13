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

/// Static switch values a `MaterialInstanceConstant` overrides, read from the native bytes after its property list
/// (B-273: a lamp's `LightOn` switch, so the shade glows). The cooked instance carries them inside its compiled
/// resource as `FStaticSwitchParameter` entries: an array count, then per entry the parameter `FName`, `Value` and
/// `bOverride` (both 32-bit booleans) and an expression GUID. `name_at` maps a name-table index to its text. Only
/// entries with `bOverride` set are returned; the others follow the parent's `DefaultValue`. The scan matches that
/// layout anywhere in `tail`; the caller only looks up names the parent graph declares as switches, so a chance match
/// on another name is never read.
pub fn scan_static_switches(tail: &[u8], name_at: &dyn Fn(u32) -> Option<String>) -> Vec<(String, bool)> {
    let word = |i: usize| -> Option<u32> { tail.get(i * 4..i * 4 + 4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]])) };
    let mut out: Vec<(String, bool)> = Vec::new();
    for i in 1..tail.len() / 4 {
        let (Some(count), Some(ni), Some(num), Some(value), Some(over)) = (word(i - 1), word(i), word(i + 1), word(i + 2), word(i + 3)) else {
            break;
        };
        // The entry's GUID (16 bytes) follows the two booleans.
        if !(1..=8).contains(&count) || num != 0 || value > 1 || over > 1 || (i + 4) * 4 + 16 > tail.len() {
            continue;
        }
        let Some(n) = name_at(ni) else { continue };
        if n == "None" || n.starts_with("FMaterial") || out.iter().any(|(k, _)| *k == n.to_lowercase()) {
            continue;
        }
        if over == 1 {
            out.push((n.to_lowercase(), value == 1));
        }
    }
    out
}

fn collect_params(pkg: &Package, idx: i32, into: &mut Params) {
    let Ok((props, rest)) = object_props(pkg, idx) else { return };
    if std::env::var("BOTS_N1_STATIC_SWITCH").map_or(true, |v| v != "0") {
        let tail = pkg.export_data(idx).get(rest.o..).unwrap_or_default();
        for (n, v) in scan_static_switches(tail, &|i| pkg.names.get(i as usize).cloned()) {
            into.switch.entry(n).or_insert(v);
        }
    }
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
            let mut def = if class == "MaterialBlended" {
                if !blended_on() {
                    return None;
                }
                blend_by(p, ei, &params, &lookup)?
            } else {
                let g = read_material(p, ei).ok()?;
                compile(p, &g, &params)
            };
            def.path = path.to_string();
            def.class = chain_class;
            return Some(def);
        }
        None
    }
}

/// `BOTS_N1_BLENDED=0` draws a `MaterialBlended` as before (no definition: the default untextured surface), the revert
/// switch for B-349.
pub fn blended_on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("BOTS_N1_BLENDED").map_or(true, |v| v != "0"))
}

/// `StartTime` a blended material's clock counts from when no instance has set it yet: far in the future, so every
/// weight sits at its start (the first material only) and the graph keeps one shape with and without the parameter.
const BLEND_NOT_STARTED: f32 = 1.0e9;

/// B-349: a `MaterialBlended` (a licensee class: the burning characters' `TemplateBody_OnFire_Mat` and
/// `TemplateHead_OnFire_Mat`) is a set of `BlendMaterials` the engine fades between by time, `BlendTimes[i]` giving each
/// one's `StartTime`/`EndTime` in seconds after the instance's `StartTime` parameter (the game writes the world time
/// when a character catches fire). Compiles every material with the same instance parameters and merges them.
fn blend_by<P: std::ops::Deref<Target = Package>>(
    p: &Package,
    ei: i32,
    params: &Params,
    lookup: &impl Fn(&str) -> Option<(P, i32)>,
) -> Option<MatDef> {
    let (props, _) = object_props(p, ei).ok()?;
    let mats = props.get("BlendMaterials")?.as_obj_array();
    let times: Vec<(f32, f32)> = props
        .get("BlendTimes")
        .map(|t| {
            array_elems(p, t.data)
                .iter()
                .map(|e| (e.get("StartTime").map_or(0.0, |x| x.as_f32()), e.get("EndTime").map_or(0.0, |x| x.as_f32())))
                .collect()
        })
        .unwrap_or_default();
    let mut parts = Vec::new();
    for o in mats {
        let (pp, sei) = lookup(&p.obj_path(o))?;
        let g = read_material(&pp, sei).ok()?;
        parts.push(compile(&pp, &g, params));
    }
    let start = params.scalar.get("starttime").copied().unwrap_or(BLEND_NOT_STARTED);
    if std::env::var("BOTS_N1_BLEND_LOG").is_ok_and(|v| v == "1") {
        eprintln!("n1: blended material {} StartTime {} ({} materials, windows {:?})", p.obj_path(ei), if start == BLEND_NOT_STARTED { "unset".to_string() } else { start.to_string() }, parts.len(), times);
    }
    merge_blend(parts, &times, start)
}

/// Merges the graphs of a blended material into one: each material's nodes appended in turn, then the weights (from the
/// seconds since `start`) and, for every output, the weighted sum of the materials' values. Material i+1 fades in over
/// `times[i]` on top of the earlier ones (chained lerps), so the weights always add up to 1. With no usable windows the
/// first material stands alone.
pub fn merge_blend(parts: Vec<MatDef>, times: &[(f32, f32)], start: f32) -> Option<MatDef> {
    let first = parts.first()?;
    if parts.len() == 1 || times.len() != parts.len() {
        return Some(first.clone());
    }
    let mut out = MatDef {
        blend: first.blend.clone(),
        lighting: first.lighting.clone(),
        two_sided: first.two_sided,
        clip: first.clip,
        diffuse_const: first.diffuse_const,
        spec_power_const: first.spec_power_const,
        ..Default::default()
    };
    // (diffuse, specular, spec_power, normal, emissive) roots of every part, moved to the merged numbering
    let mut roots: Vec<[Option<u32>; 5]> = Vec::new();
    for d in &parts {
        let off = out.nodes.len() as u32;
        // a texture two materials share (the character's diffuse, say) takes one slot
        let remap: Vec<u32> = d
            .textures
            .iter()
            .map(|t| match out.textures.iter().position(|o| o.path == t.path && o.normal == t.normal) {
                Some(i) => i as u32,
                None => {
                    out.textures.push(t.clone());
                    out.textures.len() as u32 - 1
                }
            })
            .collect();
        out.nodes.extend(d.nodes.iter().map(|n| match n.shifted(off, 0) {
            Node::Tex { tex, uv } => Node::Tex { tex: remap[tex as usize], uv },
            other => other,
        }));
        let m = |o: Option<u32>| o.map(|i| i + off);
        roots.push([m(d.diffuse), m(d.specular), m(d.spec_power), m(d.normal), m(d.emissive)]);
        out.uses_vcolor |= d.uses_vcolor;
        out.uses_uv1 |= d.uses_uv1;
        for u in &d.unsupported {
            if !out.unsupported.contains(u) {
                out.unsupported.push(u.clone());
            }
        }
    }
    let push = |out: &mut MatDef, n: Node| -> u32 {
        out.nodes.push(n);
        out.nodes.len() as u32 - 1
    };
    let k = |out: &mut MatDef, v: f32| push(out, Node::Const { v: [v; 4], n: 1 });
    let time = push(&mut out, Node::Time);
    let st = k(&mut out, start);
    let t = push(&mut out, Node::Sub { a: time, b: st });
    // ramp(i): 0 before the window, 1 after it
    let mut ramps = Vec::new();
    for &(lo, hi) in times {
        let lo_n = k(&mut out, lo);
        let inv = k(&mut out, 1.0 / (hi - lo).max(1e-3));
        let d = push(&mut out, Node::Sub { a: t, b: lo_n });
        let x = push(&mut out, Node::Mul { a: d, b: inv });
        ramps.push(push(&mut out, Node::Sat { x, lo: 0.0, hi: 1.0 }));
    }
    // BOTS_N1_BLEND_FORCE=<i>: debug aid, shows material i alone (to compare one stage with the game's picture)
    let force: Option<usize> = std::env::var("BOTS_N1_BLEND_FORCE").ok().and_then(|v| v.parse().ok()).filter(|&i| i < parts.len());
    // Chained fades: material i+1 fades in over window i (`times[i]`) on top of what the earlier ones made, so
    // w0 = (1-r0)(1-r1).., w1 = r0 (1-r1).., w2 = r1: with the burn windows (0-1 s, 0-1 s) the surface goes body -> charred in
    // about a second, as the game's own picture does. The last window has no later material and is not used.
    let one_minus: Vec<u32> = (0..parts.len() - 1).map(|i| push(&mut out, Node::Un { f: "oneminus".into(), x: ramps[i] })).collect();
    let mut weights = Vec::new();
    for i in 0..parts.len() {
        let mut w: Option<u32> = if i == 0 { None } else { Some(ramps[i - 1]) };
        for &om in &one_minus[i..] {
            w = Some(match w {
                Some(x) => push(&mut out, Node::Mul { a: x, b: om }),
                None => om,
            });
        }
        weights.push(w.unwrap_or_else(|| k(&mut out, 1.0)));
    }
    if let Some(f) = force {
        for (i, w) in weights.iter_mut().enumerate() {
            *w = k(&mut out, if i == f { 1.0 } else { 0.0 });
        }
    }
    // weighted sum of one output over the parts; `None` when no part has it
    let sum = |out: &mut MatDef, slot: usize, absent: [f32; 4], n: u8| -> Option<u32> {
        if roots.iter().all(|r| r[slot].is_none()) {
            return None;
        }
        let mut acc: Option<u32> = None;
        for (i, r) in roots.iter().enumerate() {
            let v = match r[slot] {
                Some(v) => v,
                None => push(out, Node::Const { v: absent, n }),
            };
            let term = push(out, Node::Mul { a: weights[i], b: v });
            acc = Some(match acc {
                Some(a) => push(out, Node::Add { a, b: term }),
                None => term,
            });
        }
        acc
    };
    let dc = first.diffuse_const.unwrap_or([0.5, 0.5, 0.5, 1.0]);
    out.diffuse = sum(&mut out, 0, dc, 0);
    out.specular = sum(&mut out, 1, [0.0; 4], 0);
    out.spec_power = sum(&mut out, 2, [first.spec_power_const.unwrap_or(15.0); 4], 1);
    out.normal = sum(&mut out, 3, [0.0, 0.0, 1.0, 0.0], 3);
    out.emissive = sum(&mut out, 4, [0.0; 4], 0);
    Some(out)
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
            // B-345: VectorLength (the stylized counter-kill's white-room floor fades its tiles to white by the distance from
            // the centre; drawn as 0, the floor was all tiles: #32's grid).
            "VectorLength" => {
                let x = self.input(&e, "Input").unwrap_or_else(|| self.cst(0.0));
                self.push(Node::Un { f: "length".into(), x })
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
            "DepthBiasedAlpha" if soft_alpha_on() => {
                let alpha = self.input(&e, "Alpha");
                let bias = self.input(&e, "Bias");
                self.push(Node::SoftAlpha { alpha, bias, scale: f("BiasScale", 1.0) })
            }
            "DepthBiasedAlpha" => self.input(&e, "Alpha").unwrap_or_else(|| self.cst(1.0)),
            _ => {
                let _ = out;
                self.unsup(&e.class)
            }
        }
    }
}

/// B-327: `BOTS_N1_SOFTALPHA=0` goes back to ignoring DepthBiasedAlpha on mesh materials (a hard-edged card).
pub fn soft_alpha_on() -> bool {
    std::env::var("BOTS_N1_SOFTALPHA").map_or(true, |v| v != "0")
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

#[cfg(test)]
mod static_switch_tests {
    use super::scan_static_switches;

    fn hex(s: &str) -> Vec<u8> {
        let s: String = s.split_whitespace().collect();
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }

    fn names(i: u32) -> Option<String> {
        match i {
            0x147 => Some("FMaterialUniformExpressionVectorParameter".into()),
            0x1c7 => Some("LightOn".into()),
            0x1c8 => Some("Reflections".into()),
            _ => Some("Acceleration".into()),
        }
    }

    // DownLightServ_ON_MAT01's native tail around its static parameter set: BaseMaterialId, count 1, `LightOn`,
    // Value 1, bOverride 1, the expression GUID.
    const ON: &str = "3f 80 00 00 00 00 00 00 35 28 64 44 4f 8a ee 3f dd a3 98 ad 71 52 77 04 \
                      00 00 00 01 00 00 01 c7 00 00 00 00 00 00 00 01 00 00 00 01 \
                      10 86 c5 46 45 d1 1d d8 91 20 29 b9 35 ea 3d 29 00 00 00 00 00 00 00 00";

    #[test]
    fn reads_an_overridden_switch() {
        assert_eq!(scan_static_switches(&hex(ON), &names), vec![("lighton".to_string(), true)]);
    }

    #[test]
    fn override_to_false_and_not_overridden() {
        // Value 0, bOverride 1 -> off; Value 0, bOverride 0 -> follows the parent's default (not returned).
        let off = ON.replace("00 00 00 00 00 00 00 01 00 00 00 01", "00 00 00 00 00 00 00 00 00 00 00 01");
        assert_eq!(scan_static_switches(&hex(&off), &names), vec![("lighton".to_string(), false)]);
        let none = ON.replace("00 00 00 00 00 00 00 01 00 00 00 01", "00 00 00 00 00 00 00 00 00 00 00 00");
        assert!(scan_static_switches(&hex(&none), &names).is_empty());
    }

    #[test]
    fn ignores_uniform_expression_records() {
        let b = hex("00 00 00 01 00 00 01 47 00 00 00 00 00 00 00 01 00 00 00 01 10 86 c5 46 45 d1 1d d8 91 20 29 b9 35 ea 3d 29");
        assert!(scan_static_switches(&b, &names).is_empty());
    }
}

#[cfg(test)]
mod blend_tests {
    use super::*;

    fn tex(path: &str) -> TexRef {
        TexRef { path: path.into(), normal: false }
    }

    /// A material that is just texture `t` (diffuse) and a constant emissive.
    fn part(t: &str, emissive: f32) -> MatDef {
        MatDef {
            nodes: vec![Node::Tex { tex: 0, uv: None }, Node::Const { v: [emissive; 4], n: 0 }],
            textures: vec![tex(t)],
            diffuse: Some(0),
            emissive: Some(1),
            ..Default::default()
        }
    }

    /// Reference evaluation of the merged graph for the nodes the blend adds (Time, Sub, Mul, Sat, Un, Const, Add).
    fn eval(d: &MatDef, i: u32, time: f32, tex_val: &dyn Fn(u32) -> f32) -> f32 {
        let e = |j: &u32| eval(d, *j, time, tex_val);
        match &d.nodes[i as usize] {
            Node::Time => time,
            Node::Const { v, .. } => v[0],
            Node::Tex { tex, .. } => tex_val(*tex),
            Node::Sub { a, b } => e(a) - e(b),
            Node::Add { a, b } => e(a) + e(b),
            Node::Mul { a, b } => e(a) * e(b),
            Node::Sat { x, lo, hi } => e(x).clamp(*lo, *hi),
            Node::Un { f, x } if f == "oneminus" => 1.0 - e(x),
            n => panic!("unexpected {n:?}"),
        }
    }

    // chained fades: flame over body during 0..1 s, then char over that during 0..1 s (the third window is unused)
    const TIMES: [(f32, f32); 3] = [(0.0, 1.0), (0.0, 1.0), (1.0, 6.0)];

    fn merged() -> MatDef {
        merge_blend(vec![part("Body", 0.0), part("Flame", 4.0), part("Char", 0.0)], &TIMES, 100.0).unwrap()
    }

    #[test]
    fn textures_are_shared_and_nodes_stay_ordered() {
        let m = merge_blend(vec![part("Body", 0.0), part("Body", 1.0), part("Char", 0.0)], &TIMES, 0.0).unwrap();
        assert_eq!(m.textures.iter().map(|t| t.path.as_str()).collect::<Vec<_>>(), ["Body", "Char"]);
        // every reference points at an earlier node (topological order) and every texture index exists
        for (i, n) in m.nodes.iter().enumerate() {
            if let Node::Tex { tex, .. } = n {
                assert!((*tex as usize) < m.textures.len());
            }
            let refs = match n {
                Node::Add { a, b } | Node::Sub { a, b } | Node::Mul { a, b } => vec![*a, *b],
                Node::Sat { x, .. } | Node::Un { x, .. } => vec![*x],
                _ => vec![],
            };
            assert!(refs.iter().all(|&r| (r as usize) < i), "node {i} {n:?}");
        }
    }

    #[test]
    fn weights_follow_the_clock_and_sum_to_one() {
        let m = merged();
        // diffuse = w0*body + w1*flame + w2*char with each texture sampled as 1, 10, 100
        let tv = |t: u32| [1.0, 10.0, 100.0][t as usize];
        let at = |sec: f32| eval(&m, m.diffuse.unwrap(), 100.0 + sec, &tv);
        assert_eq!(at(-5.0), 1.0, "before the start: the body alone");
        assert!((at(0.0) - 1.0).abs() < 1e-4);
        // t = 0.5: r = 0.5 for both fades: w0 = .25, w1 = .25, w2 = .5
        assert!((at(0.5) - (0.25 * 1.0 + 0.25 * 10.0 + 0.5 * 100.0)).abs() < 1e-3, "{}", at(0.5));
        assert!((at(1.0) - 100.0).abs() < 1e-3, "charred after a second");
        assert!((at(60.0) - 100.0).abs() < 1e-3);
        // the emissive follows the flame only: w1 = r0 (1 - r1) peaks at 0.25 and is gone by 1 s
        let em = |sec: f32| eval(&m, m.emissive.unwrap(), 100.0 + sec, &tv);
        assert_eq!(em(-5.0), 0.0);
        assert!((em(0.5) - 0.25 * 4.0).abs() < 1e-4);
        assert!(em(1.0).abs() < 1e-4);
    }

    #[test]
    fn unset_start_time_shows_the_body() {
        let m = merge_blend(vec![part("Body", 0.0), part("Flame", 4.0), part("Char", 0.0)], &TIMES, BLEND_NOT_STARTED).unwrap();
        let tv = |t: u32| [1.0, 10.0, 100.0][t as usize];
        assert_eq!(eval(&m, m.diffuse.unwrap(), 12345.0, &tv), 1.0);
    }

    #[test]
    fn missing_outputs_and_odd_inputs() {
        // an output only some parts have is blended with the neutral value; one none has stays absent
        let mut a = part("A", 0.0);
        a.normal = Some(1);
        let m = merge_blend(vec![a, part("B", 0.0), part("C", 0.0)], &TIMES, 0.0).unwrap();
        assert!(m.normal.is_some() && m.specular.is_none() && m.opacity.is_none());
        // windows that don't match the materials, or a single material: the first one, unchanged
        let one = merge_blend(vec![part("A", 0.0), part("B", 0.0)], &TIMES, 0.0).unwrap();
        assert_eq!(one.textures.len(), 1);
        assert!(merge_blend(vec![], &TIMES, 0.0).is_none());
    }
}
