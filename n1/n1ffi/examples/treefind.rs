//! List the SpeedTree placements of every package (or those whose package or tree path contains a substring): component,
//! tree asset, location, baked-light slots and any light parse error. Run from `n1/`:
//!   treefind <CookedXenon dir> <cache dir> [substring]
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 3 {
        eprintln!("usage: treefind <CookedXenon dir> <cache dir> [substring]");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let sub = a.get(3).map(|s| s.to_lowercase());
    for n in db.package_names() {
        for t in db.level_tree_placements(&n) {
            let line = format!(
                "{n}: {} tree {} loc {:?} scale {:?} rot {:?} lights {:?} err {:?}",
                t.component,
                t.tree,
                t.location,
                t.scale,
                t.rotation,
                t.lights.iter().map(|l| l.as_ref().map(|l| l.samples.len())).collect::<Vec<_>>(),
                t.light_error
            );
            if sub.as_ref().is_none_or(|s| line.to_lowercase().contains(s)) {
                println!("{line}");
            }
        }
    }
}
