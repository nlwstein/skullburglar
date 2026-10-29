//! Hex-dump the native bytes after an object's property list (a MaterialInstanceConstant's static parameter set,
//! say). Run from `n1/`: mictail <CookedXenon dir> <cache dir> <object path>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: mictail <CookedXenon dir> <cache dir> <object path>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let (class, b) = db.object_tail(&a[3]).expect("object not found");
    println!("{class}: {} bytes after properties", b.len());
    // Every aligned word that is a valid name index and followed by a zero word could be an FName: name them.
    for (i, w) in b.chunks_exact(4).take(200).enumerate() {
        let v = u32::from_be_bytes([w[0], w[1], w[2], w[3]]) as usize;
        if let Some(n) = db.name_in_package_of(&a[3], v) {
            println!("word {:#06x} = {v:#x} -> name {n:?}", i * 4);
        }
    }
    for (i, ch) in b.chunks(16).enumerate() {
        let hex: Vec<String> = ch.iter().map(|x| format!("{x:02x}")).collect();
        let asc: String = ch.iter().map(|&x| if (32..127).contains(&x) { x as char } else { '.' }).collect();
        println!("{:04x}  {:<48} {asc}", i * 16, hex.join(" "));
    }
}
