//! List material-like objects whose path contains any of the given words (case-insensitive), with blend mode and
//! unsupported expression classes. Run from `n1/`:
//!   cargo run --release -p n1ffi --example matfind -- <CookedXenon dir> <cache dir> word [word...]
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: matfind <CookedXenon dir> <cache dir> word [word...]");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let words: Vec<String> = a[3..].iter().map(|w| w.to_lowercase()).collect();
    for path in db.paths_of_classes(&["Material", "MaterialInstanceConstant", "MaterialInstanceTimeVarying", "DecalMaterial"]) {
        let l = path.to_lowercase();
        if words.iter().any(|w| l.contains(w)) {
            let m = db.material(&path);
            match &m.def {
                Some(d) => println!("{path} [{}] unsupported={:?}", d.blend, d.unsupported),
                None => println!("{path} (no mesh def)"),
            }
        }
    }
}
