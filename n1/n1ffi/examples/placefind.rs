//! List level placements whose per-section material override contains a substring (or whose mesh path does).
//! Run from `n1/`: placefind <CookedXenon dir> <cache dir> <substring>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: placefind <CookedXenon dir> <cache dir> <substring>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let sub = a[3].to_lowercase();
    for n in db.package_names() {
        for pl in db.level_placements(&n) {
            let hit = pl.materials.iter().any(|m| m.to_lowercase().contains(&sub)) || pl.mesh.to_lowercase().contains(&sub);
            if hit {
                let t = pl.transform;
                println!("{n}: {} mesh {} materials {:?} transform {:?}", pl.actor, pl.mesh, pl.materials, t);
            }
        }
    }
}
