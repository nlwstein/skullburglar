//! Print the mesh/decal compiler's `MatDef` (node list) of materials. Run from `n1/`:
//!   cargo run --release -p n1ffi --example matdefdump -- <CookedXenon dir> <cache dir> <material path>...
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: matdefdump <CookedXenon dir> <cache dir> <material path>...");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    for m in &a[3..] {
        println!("==== {m}");
        match &db.material(m).def {
            Some(d) => {
                for (i, n) in d.nodes.iter().enumerate() {
                    println!("{i}: {n:?}");
                }
                println!("textures {:?}", d.textures);
                println!("diffuse {:?} emissive {:?} opacity {:?} normal {:?}", d.diffuse, d.emissive, d.opacity, d.normal);
            }
            None => println!("(no def)"),
        }
    }
}
