//! Decode one texture path from every package that holds it and print a hash per copy, to see whether same-named
//! copies differ. Run from `n1/`: texcmp <CookedXenon dir> <scratch cache root> <texture path>
use std::hash::{Hash, Hasher};
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: texcmp <CookedXenon dir> <scratch cache root> <texture path>");
        std::process::exit(2);
    }
    let root = Path::new(&a[2]);
    let probe = assetdb::AssetDb::open(Path::new(&a[1]), &root.join("probe")).expect("open assetdb");
    let holders = probe.texture_holders(&a[3]).into_iter().find(|(p, _)| *p == a[3]).map(|h| h.1).unwrap_or_default();
    for (i, pkg) in holders.iter().enumerate() {
        let dir = root.join(format!("c{i}"));
        let _ = std::fs::remove_dir_all(&dir);
        let db = assetdb::AssetDb::open(Path::new(&a[1]), &dir).expect("open assetdb");
        db.prefer_packages([pkg.clone()]);
        match db.texture(&a[3]) {
            Some(t) => {
                let mut h = std::collections::hash_map::DefaultHasher::new();
                t.mips[0].hash(&mut h);
                println!("{pkg}: {}x{} {:?} mips {} top-hash {:016x}", t.width, t.height, t.fmt, t.mips.len(), h.finish());
            }
            None => println!("{pkg}: none"),
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
}
