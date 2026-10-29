//! Print the WGSL the mesh material compiler emits for materials (plus its parameter table). Run from `n1/`:
//!   matwgsl <CookedXenon dir> <cache dir> <material path>...
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: matwgsl <CookedXenon dir> <cache dir> <material path>...");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    for m in &a[3..] {
        println!("==== {m}");
        match &db.material(m).def {
            Some(d) => {
                let unpack = vec![None; d.textures.len()];
                let c = matdef::codegen::compile(d, &unpack);
                println!("{}\nparams {:?}\nunsupported {:?}", c.body, c.params, c.unsupported);
            }
            None => println!("(no def)"),
        }
    }
}
