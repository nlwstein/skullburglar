//! Find Materials whose expression graph JSON contains a substring (e.g. `"SfMaterialExpressionPlatformSpecific"`, or
//! `"R": 50.0`). Run from `n1/`:
//!   cargo run --release -p n1ffi --example matgrep -- <CookedXenon dir> <cache dir> <substring>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: matgrep <CookedXenon dir> <cache dir> <substring>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    for path in db.paths_of_classes(&["Material"]) {
        if let Some(j) = db.particle_material_json(&path) {
            if serde_json::to_string(&j).map_or(false, |s| s.contains(&a[3])) {
                println!("{path}");
            }
        }
    }
}
