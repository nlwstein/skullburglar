//! Count the StaticMeshes by LightMapCoordinateIndex (and list those that are not 1). Run from `n1/`:
//!   lmcoord <CookedXenon dir> <cache dir>
use std::collections::BTreeMap;
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 3 {
        eprintln!("usage: lmcoord <CookedXenon dir> <cache dir>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let mut n: BTreeMap<i64, usize> = BTreeMap::new();
    for p in db.paths_of_classes(&["StaticMesh"]) {
        let i = db.object_json(&p).and_then(|j| j.get("props")?.get("LightMapCoordinateIndex")?.as_i64()).unwrap_or(1);
        *n.entry(i).or_default() += 1;
        if i != 1 {
            println!("index {i}: {p}");
        }
    }
    println!("counts by LightMapCoordinateIndex: {n:?}");
}
