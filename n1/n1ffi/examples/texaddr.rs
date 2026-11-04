//! Print the sampler addressing (`AddressX`/`AddressY`) of textures. Run from `n1/`:
//!   texaddr <CookedXenon dir> <cache dir> <texture path>...
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: texaddr <CookedXenon dir> <cache dir> <texture path>...");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    for t in &a[3..] {
        match db.texture_meta(t) {
            Some(m) => println!("{t}: {:?} {:?}", m.addr, m.fmt),
            None => println!("{t}: not found"),
        }
    }
}
