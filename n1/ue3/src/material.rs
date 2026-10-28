//! UMaterial / UMaterialInstanceConstant parsing (v538, BotS).
//!
//! A cooked `Material` export carries its inputs (`DiffuseColor`, `Normal`, ... as tagged `*MaterialInput`
//! structs naming an expression export) and the expression graph as separate exports
//! (`MaterialExpressionMultiply`, `...TextureSample`, ...). We read the graph into [`MatGraph`] so a classifier can
//! decide which texture feeds which channel. A `MaterialInstanceConstant` has a `Parent` and parameter value arrays.

use crate::package::{Package, Reader};
use crate::props::{Props, object_props, read_fname, read_props};
use anyhow::Result;
use std::collections::HashMap;

/// A connection to another expression's output (`Expression` 0 = unconnected).
#[derive(Debug, Clone, Default)]
pub struct Input {
    pub expr: i32,
    pub output: i32,
    /// Output mask (R,G,B,A) when `Mask` is set.
    pub mask: Option<[bool; 4]>,
    pub use_constant: bool,
    pub constant: [f32; 4],
}

#[derive(Debug, Clone, Default)]
pub struct Expr {
    pub class: String,
    pub name: String,
    pub inputs: Vec<(String, Input)>,
    /// Scalar props (R, DefaultValue, UTiling, ConstA...).
    pub floats: HashMap<String, f32>,
    pub ints: HashMap<String, i32>,
    pub bools: HashMap<String, bool>,
    /// Object props (Texture, Material...) as package indices.
    pub objs: HashMap<String, i32>,
    pub names: HashMap<String, String>,
    /// LinearColor / Color props (Constant, DefaultValue) as RGBA.
    pub colors: HashMap<String, [f32; 4]>,
}

impl Expr {
    pub fn input(&self, n: &str) -> Option<&Input> {
        self.inputs.iter().find(|(k, _)| k == n).map(|(_, v)| v).filter(|i| i.expr != 0)
    }
}

#[derive(Debug, Clone, Default)]
pub struct MatGraph {
    pub class: String,
    pub path: String,
    pub blend: String,
    pub two_sided: bool,
    pub masked: bool,
    pub clip: f32,
    pub lighting_model: String,
    /// DiffuseColor, SpecularColor, Normal, EmissiveColor, Opacity, OpacityMask, SpecularPower, ...
    pub inputs: Vec<(String, Input)>,
    pub exprs: HashMap<i32, Expr>,
    /// ReferencedTextures (package-local object paths)
    pub referenced: Vec<String>,
    pub parent: i32,
}

impl MatGraph {
    pub fn input(&self, n: &str) -> Option<&Input> {
        self.inputs.iter().find(|(k, _)| k == n).map(|(_, v)| v).filter(|i| i.expr != 0)
    }
}

fn parse_input(pkg: &Package, data: &[u8]) -> Input {
    let mut i = Input::default();
    let mut r = Reader::new(data, 0);
    let Ok(props) = read_props(pkg, &mut r) else { return i };
    for p in &props.list {
        match p.name.as_str() {
            "Expression" => i.expr = p.as_obj(),
            "OutputIndex" => i.output = p.as_i32(),
            "Mask" => {
                let m = i.mask.get_or_insert([false; 4]);
                let _ = m;
            }
            "MaskR" | "MaskG" | "MaskB" | "MaskA" => {
                let k = match p.name.as_str() {
                    "MaskR" => 0,
                    "MaskG" => 1,
                    "MaskB" => 2,
                    _ => 3,
                };
                i.mask.get_or_insert([false; 4])[k] = p.as_i32() != 0;
            }
            "UseConstant" => i.use_constant = p.as_i32() != 0 || p.bool_value,
            "Constant" => {
                // Color (BGRA bytes) for ColorMaterialInput, float for Scalar, Vector3 for Vector
                match p.data.len() {
                    4 if p.ty == "FloatProperty" => i.constant = [p.as_f32(); 4],
                    4 => i.constant = [p.data[2] as f32 / 255., p.data[1] as f32 / 255., p.data[0] as f32 / 255., p.data[3] as f32 / 255.],
                    _ => {}
                }
            }
            _ => {}
        }
    }
    i
}

fn color_of(p: &crate::props::Prop) -> Option<[f32; 4]> {
    let mut r = p.r();
    if p.data.len() == 16 {
        Some([r.f32().ok()?, r.f32().ok()?, r.f32().ok()?, r.f32().ok()?])
    } else if p.data.len() == 12 {
        Some([r.f32().ok()?, r.f32().ok()?, r.f32().ok()?, 1.0])
    } else {
        None
    }
}

fn parse_expr(pkg: &Package, idx: i32) -> Option<Expr> {
    let (props, _) = object_props(pkg, idx).ok()?;
    let mut e = Expr { class: pkg.class_name(idx).to_string(), name: pkg.obj_path(idx), ..Default::default() };
    for p in &props.list {
        let n = p.name.clone();
        match p.ty.as_str() {
            "StructProperty" => {
                if p.struct_name.ends_with("Input") {
                    e.inputs.push((n, parse_input(pkg, p.data)));
                } else if matches!(p.struct_name.as_str(), "LinearColor" | "Color" | "Vector") {
                    if let Some(c) = color_of(p) {
                        e.colors.insert(n, c);
                    }
                }
            }
            "FloatProperty" => {
                e.floats.insert(n, p.as_f32());
            }
            "IntProperty" => {
                e.ints.insert(n, p.as_i32());
            }
            "BoolProperty" => {
                e.bools.insert(n, p.bool_value);
            }
            "ObjectProperty" => {
                e.objs.insert(n, p.as_obj());
            }
            "NameProperty" => {
                if let Ok(s) = read_fname(pkg, &mut p.r()) {
                    e.names.insert(n, s);
                }
            }
            "ByteProperty" if p.data.len() == 8 => {
                if let Ok(s) = read_fname(pkg, &mut p.r()) {
                    e.names.insert(n, s);
                }
            }
            _ => {}
        }
    }
    Some(e)
}

pub fn enum_prop(p: &Package, props: &Props, n: &str) -> String {
    props.get(n).and_then(|x| if x.data.len() == 8 { read_fname(p, &mut x.r()).ok() } else { None }).unwrap_or_default()
}

/// Parse a `Material` export and everything it references (recursively through expression inputs).
pub fn read_material(pkg: &Package, idx: i32) -> Result<MatGraph> {
    let (props, _) = object_props(pkg, idx)?;
    let mut g = MatGraph { class: pkg.class_name(idx).to_string(), path: pkg.obj_path(idx), clip: 0.333, ..Default::default() };
    g.blend = enum_prop(pkg, &props, "BlendMode");
    g.lighting_model = enum_prop(pkg, &props, "LightingModel");
    g.two_sided = props.get("TwoSided").is_some_and(|x| x.bool_value);
    g.masked = props.get("bIsMasked").is_some_and(|x| x.bool_value);
    if let Some(c) = props.get("OpacityMaskClipValue") {
        g.clip = c.as_f32();
    }
    g.parent = props.get("Parent").map(|x| x.as_obj()).unwrap_or(0);
    g.referenced = props
        .list
        .iter()
        .filter(|x| x.name == "ReferencedTextures")
        .flat_map(|x| x.as_obj_array())
        .filter(|&o| o != 0)
        .map(|o| pkg.obj_path(o))
        .collect();
    for p in &props.list {
        if p.ty == "StructProperty" && p.struct_name.ends_with("MaterialInput") {
            g.inputs.push((p.name.clone(), parse_input(pkg, p.data)));
        }
    }
    // BFS over expressions
    let mut stack: Vec<i32> = g.inputs.iter().map(|(_, i)| i.expr).filter(|&e| e > 0).collect();
    while let Some(ei) = stack.pop() {
        if g.exprs.contains_key(&ei) {
            continue;
        }
        let Some(e) = parse_expr(pkg, ei) else { continue };
        for (_, i) in &e.inputs {
            if i.expr > 0 {
                stack.push(i.expr);
            }
        }
        g.exprs.insert(ei, e);
    }
    Ok(g)
}

/// Pretty-print a material graph as an indented tree rooted at each connected input.
pub fn dump(pkg: &Package, g: &MatGraph) -> String {
    use std::fmt::Write;
    let mut s = String::new();
    let _ = writeln!(s, "{} [{}] blend={} two_sided={} masked={} clip={} lm={}", g.path, g.class, g.blend, g.two_sided, g.masked, g.clip, g.lighting_model);
    for (n, i) in &g.inputs {
        if i.expr == 0 {
            if i.use_constant {
                let _ = writeln!(s, "  {n} = const {:?}", i.constant);
            }
            continue;
        }
        let _ = writeln!(s, "  {n} <- out{} mask{:?}", i.output, i.mask);
        dump_expr(pkg, g, i.expr, 2, &mut s, 0);
    }
    s
}

fn dump_expr(pkg: &Package, g: &MatGraph, ei: i32, depth: usize, s: &mut String, rec: usize) {
    use std::fmt::Write;
    let pad = "  ".repeat(depth + 1);
    let Some(e) = g.exprs.get(&ei) else {
        let _ = writeln!(s, "{pad}<expr {ei} missing>");
        return;
    };
    let cls = e.class.trim_start_matches("MaterialExpression");
    let mut extra = String::new();
    for (k, v) in &e.objs {
        if *v != 0 {
            let _ = write!(extra, " {k}={}", pkg.obj_path(*v));
        }
    }
    for (k, v) in &e.floats {
        if !k.starts_with("Editor") {
            let _ = write!(extra, " {k}={v}");
        }
    }
    for (k, v) in &e.ints {
        if !k.starts_with("Editor") {
            let _ = write!(extra, " {k}={v}");
        }
    }
    for (k, v) in &e.names {
        let _ = write!(extra, " {k}={v}");
    }
    for (k, v) in &e.colors {
        let _ = write!(extra, " {k}={v:?}");
    }
    let _ = writeln!(s, "{pad}{cls}#{ei}{extra}");
    if rec > 12 {
        return;
    }
    for (n, i) in &e.inputs {
        if i.expr > 0 {
            let _ = writeln!(s, "{pad}  .{n} (out{} mask{:?}):", i.output, i.mask);
            dump_expr(pkg, g, i.expr, depth + 2, s, rec + 1);
        }
    }
}
