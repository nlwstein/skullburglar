//! List Texture2D paths containing a substring, with the packages that hold each. Run from `n1/`:
//!   cargo run --release -p n1ffi --example texfind -- <CookedXenon dir> <cache dir> <substring>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: texfind <CookedXenon dir> <cache dir> <substring>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    for (p, pk) in db.texture_holders(&a[3]) {
        println!("{p}  [{}]", pk.join(", "));
    }
}
