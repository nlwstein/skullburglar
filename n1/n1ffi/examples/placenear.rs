//! List the placements of one package within a radius of a UE3 world point. Run from `n1/`:
//!   placenear <CookedXenon dir> <cache dir> <package> x y z radius
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 8 {
        eprintln!("usage: placenear <CookedXenon dir> <cache dir> <package> x y z radius");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let p: Vec<f32> = a[4..8].iter().map(|s| s.parse().unwrap()).collect();
    for pl in db.level_placements(&a[3]) {
        let t = pl.transform[3];
        let d = ((t[0] - p[0]).powi(2) + (t[1] - p[1]).powi(2) + (t[2] - p[2]).powi(2)).sqrt();
        if d <= p[3] {
            println!("{d:6.1} {} mesh {} materials {:?} at ({:.0},{:.0},{:.0})", pl.actor, pl.mesh, pl.materials, t[0], t[1], t[2]);
        }
    }
}
