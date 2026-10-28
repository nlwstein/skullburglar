//! UMaterial / MaterialInstanceConstant expression graphs as JSON (v538), for the particle shader: every export
//! reachable from the material's EmissiveColor / Opacity / OpacityMask inputs, with its tagged properties decoded.
//! Inputs (FExpressionInput) are tagged-property structs; they come out as `{"expr": <export index>, "out": n}`.

use crate::package::{Package, Reader};
use crate::props::{read_fname, read_props, Prop};
use anyhow::Result;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};

fn is_input_struct(n: &str) -> bool {
    n.ends_with("MaterialInput") || n == "ExpressionInput"
}

fn prop_value(p: &Package, pr: &Prop) -> Value {
    match pr.ty.as_str() {
        "ObjectProperty" | "ComponentProperty" => {
            let o = pr.as_obj();
            json!({"obj": o, "path": p.obj_path(o), "class": p.class_name(o)})
        }
        "FloatProperty" => json!(pr.as_f32()),
        "IntProperty" => json!(pr.as_i32()),
        "BoolProperty" => json!(pr.bool_value),
        "NameProperty" => json!(read_fname(p, &mut pr.r()).unwrap_or_default()),
        "ByteProperty" if pr.data.len() == 8 => json!(read_fname(p, &mut pr.r()).unwrap_or_default()),
        "ByteProperty" => json!(pr.data.first().copied().unwrap_or(0)),
        "StrProperty" => json!(pr.r().fstring().unwrap_or_default()),
        "StructProperty" if is_input_struct(&pr.struct_name) => {
            let mut r = Reader::new(pr.data, 0);
            match read_props(p, &mut r) {
                Ok(inner) => {
                    let mut m = serde_json::Map::new();
                    for ip in &inner.list {
                        m.insert(ip.name.clone(), prop_value(p, ip));
                    }
                    Value::Object(m)
                }
                Err(_) => Value::Null,
            }
        }
        "StructProperty" => {
            let f = |o: usize| pr.data.get(o..o + 4).map(|b| f32::from_be_bytes([b[0], b[1], b[2], b[3]]));
            match pr.struct_name.as_str() {
                "LinearColor" if pr.data.len() == 16 => json!([f(0), f(4), f(8), f(12)]),
                "Color" if pr.data.len() == 4 => json!([pr.data[0], pr.data[1], pr.data[2], pr.data[3]]),
                "Vector" if pr.data.len() == 12 => json!([f(0), f(4), f(8)]),
                _ => json!({"struct": pr.struct_name, "bytes": pr.data.len()}),
            }
        }
        "ArrayProperty" => {
            let a = pr.as_obj_array();
            if a.is_empty() { json!({"bytes": pr.data.len()}) } else { json!(a) }
        }
        _ => Value::Null,
    }
}

/// One export's tagged properties as JSON, plus its class.
pub fn export_json(p: &Package, idx: i32) -> Result<Value> {
    let (props, _) = crate::props::object_props(p, idx)?;
    let mut m = serde_json::Map::new();
    for pr in &props.list {
        let key = if pr.array_index > 0 { format!("{}[{}]", pr.name, pr.array_index) } else { pr.name.clone() };
        m.insert(key, prop_value(p, pr));
    }
    Ok(json!({"class": p.class_name(idx), "name": p.obj_path(idx), "props": m}))
}

/// Collect the expression exports reachable from the given root inputs of material `idx`.
/// Returns (material json, map export index -> expression json).
pub fn material_graph(p: &Package, idx: i32, roots: &[&str]) -> Result<(Value, BTreeMap<i32, Value>)> {
    let mat = export_json(p, idx)?;
    let mut seen = BTreeSet::new();
    let mut stack: Vec<i32> = Vec::new();
    for r in roots {
        if let Some(e) = mat["props"][*r]["Expression"]["obj"].as_i64() {
            stack.push(e as i32);
        }
    }
    let mut out = BTreeMap::new();
    while let Some(e) = stack.pop() {
        if e <= 0 || !seen.insert(e) {
            continue;
        }
        let Ok(j) = export_json(p, e) else { continue };
        fn walk(v: &Value, stack: &mut Vec<i32>) {
            match v {
                Value::Object(m) => {
                    if let Some(o) = m.get("Expression").and_then(|x| x.get("obj")).and_then(Value::as_i64) {
                        stack.push(o as i32);
                    }
                    for (k, x) in m {
                        if k != "Expression" {
                            walk(x, stack);
                        }
                    }
                }
                _ => {}
            }
        }
        walk(&j["props"], &mut stack);
        out.insert(e, j);
    }
    Ok((mat, out))
}
