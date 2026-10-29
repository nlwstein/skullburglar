//! List objects of the given classes, optionally filtered by a path substring. Run from `n1/`:
//!   cargo run --release -p n1ffi --example objfind -- <CookedXenon dir> <cache dir> Class[,Class...] [substring]
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: objfind <CookedXenon dir> <cache dir> Class[,Class...] [substring]");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let classes: Vec<&str> = a[3].split(',').collect();
    let sub = a.get(4).map(|s| s.to_lowercase());
    for p in db.paths_of_classes(&classes) {
        if sub.as_ref().map_or(true, |s| p.to_lowercase().contains(s)) {
            println!("{} {p}", db.class_of(&p).unwrap_or("?"));
        }
    }
}
