//! For every MaterialInstanceConstant, look in the native bytes after its properties for static switch entries (an
//! FName, two 0/1 words, a 16-byte GUID, preceded by an array count) and print what it finds. Run from `n1/`:
//!   micscan <CookedXenon dir> <cache dir> [parent substring]
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 3 {
        eprintln!("usage: micscan <CookedXenon dir> <cache dir> [parent substring]");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let want = a.get(3).cloned().unwrap_or_default();
    for p in db.paths_of_classes(&["MaterialInstanceConstant"]) {
        let Some((_, tail)) = db.object_tail(&p) else { continue };
        let parent = db.material_parent(&p).unwrap_or_default();
        if !parent.contains(&want) {
            continue;
        }
        let w = |i: usize| -> Option<u32> { tail.get(i * 4..i * 4 + 4).map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]])) };
        let mut hits = Vec::new();
        for i in 1..tail.len() / 4 {
            let (Some(c), Some(ni), Some(nn), Some(v), Some(o)) = (w(i - 1), w(i), w(i + 1), w(i + 2), w(i + 3)) else { break };
            if (1..=8).contains(&c) && nn == 0 && v <= 1 && o <= 1 && i * 4 + 16 + 16 <= tail.len() {
                if let Some(n) = db.name_in_package_of(&p, ni as usize) {
                    if !n.starts_with("FMaterial") && n != "None" {
                        hits.push(format!("{n}(count {c} v {v} o {o})"));
                    }
                }
            }
        }
        println!("{p} <- {parent}: {}", hits.join(" "));
    }
}
