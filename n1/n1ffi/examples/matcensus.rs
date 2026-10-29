//! Material census: which material expression classes the renderer's two material compilers cannot express, and
//! which materials / emitters use each. Run from `n1/`:
//!
//!   cargo run --release -p n1ffi --example matcensus -- <game>/C1Game/CookedXenon <cache dir> [--json]
//!
//! Sections:
//! - particle compiler (`scenefmt::particles::matprog`): per particle material, the classes that hit the
//!   unknown-expression arm (it draws a constant 1.0), with blend mode and the emitters using it;
//! - mesh/decal compiler (`ue3::matir` -> `matdef`): per material, `MatDef::unsupported` (it draws a constant 0.0);
//! - raw census: every expression class used by any Material, with the material count and two examples.
//!
//! A class listed under the first two sections is a renderer gap: fix it, or make the fallback loud (see
//! `BOTS_N1_MATLOG`). An empty report means every material in the cooked data compiles without a fallback.
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 3 {
        eprintln!("usage: matcensus <CookedXenon dir> <cache dir> [--json]");
        std::process::exit(2);
    }
    let json = a.iter().any(|x| x == "--json");
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");

    // Particle compiler: class -> material -> (blend, emitters).
    let mut by_emitter_mat: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for e in db.paths_of_classes(&["ParticleSpriteEmitter", "ParticleMeshEmitter"]) {
        if let Some(i) = db.emitter_info(&e) {
            if !i.material.is_empty() {
                by_emitter_mat.entry(i.material.clone()).or_default().push(e);
            }
        }
    }
    let mut part: BTreeMap<String, BTreeMap<String, (u8, Vec<String>)>> = BTreeMap::new();
    let mut nograph = Vec::new();
    for (m, es) in &by_emitter_mat {
        let Some(j) = db.particle_material_json(m) else { nograph.push(m.clone()); continue };
        let c = scenefmt::particles::matprog::compile_with(&j, &|_| Default::default());
        for u in c.unknown.iter().collect::<BTreeSet<_>>() {
            part.entry(u.clone()).or_default().insert(m.clone(), (c.blend as u8, es.clone()));
        }
    }

    // Mesh / decal compiler, and the raw expression census, over every material-like object.
    let mut mesh: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut raw: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut nmat = 0usize;
    for path in db.paths_of_classes(&["Material", "MaterialInstanceConstant", "MaterialInstanceTimeVarying", "DecalMaterial"]) {
        nmat += 1;
        let m = db.material(&path);
        if let Some(d) = &m.def {
            for u in &d.unsupported {
                mesh.entry(u.clone()).or_default().push(format!("{path} [{}]", d.blend));
            }
        }
        if db.class_of(&path) == Some("Material") {
            if let Some(j) = db.particle_material_json(&path) {
                if let Some(ex) = j.get("expressions").and_then(|x| x.as_object()) {
                    let classes: BTreeSet<String> =
                        ex.values().filter_map(|e| e["class"].as_str()).map(|c| c.trim_start_matches("MaterialExpression").to_string()).collect();
                    for c in classes {
                        raw.entry(c).or_default().push(path.clone());
                    }
                }
            }
        }
    }

    if json {
        let part_j: BTreeMap<_, _> = part.iter().map(|(c, ms)| (c.clone(), ms.iter().map(|(m, (b, es))| (m.clone(), serde_json::json!({"blend": b, "emitters": es}))).collect::<BTreeMap<_, _>>())).collect();
        let raw_j: BTreeMap<_, _> = raw.iter().map(|(c, v)| (c.clone(), v.len())).collect();
        println!("{}", serde_json::json!({"particle_unknown": part_j, "mesh_unsupported": mesh, "raw_class_counts": raw_j, "materials": nmat, "particle_no_graph": nograph}));
        return;
    }
    println!("== particle compiler: classes that fall back to constant 1.0 ({} emitter materials, {} materials scanned) ==", by_emitter_mat.len(), nmat);
    // blend: 0 opaque 1 masked 2 translucent 3 additive 4 modulate (as Blend's discriminant)
    for (c, ms) in &part {
        println!("\n{c}: {} materials", ms.len());
        for (m, (b, es)) in ms {
            println!("  {m} blend={b} emitters={} e.g. {}", es.len(), es[0]);
        }
    }
    println!("\n== mesh/decal compiler: classes that fall back to constant 0.0 ==");
    for (c, ms) in &mesh {
        println!("\n{c}: {} materials", ms.len());
        for m in ms.iter().take(40) {
            println!("  {m}");
        }
        if ms.len() > 40 {
            println!("  ... {} more", ms.len() - 40);
        }
    }
    println!("\n== raw expression classes over all Materials (count, examples) ==");
    for (c, v) in &raw {
        println!("{c}: {} e.g. {}", v.len(), v.iter().take(2).cloned().collect::<Vec<_>>().join(", "));
    }
    if !nograph.is_empty() {
        println!("\n(no graph for {} emitter materials)", nograph.len());
    }
}
