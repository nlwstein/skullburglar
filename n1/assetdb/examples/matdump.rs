//! matdump COOKED_DIR MATERIAL_PATH...: print the compiled material definition (matdef IR) n1 builds for each material path.
//! `COOKED_DIR` is `<game>/C1Game/CookedXenon`; run-time variants work too ("Pkg.Mat?&s:StartTime=12"). Debug aid for B-349.
use assetdb::AssetDb;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 3 {
        eprintln!("usage: matdump COOKED_DIR PATH...");
        std::process::exit(2);
    }
    let cache = std::env::temp_dir().join("matdump-cache");
    let db = AssetDb::open(std::path::Path::new(&a[1]), &cache).expect("open");
    for p in &a[2..] {
        let m = db.material(p);
        println!("{p}: asset class {:?} diffuse {:?} blend {:?}", m.class, m.diffuse, m.blend);
        match &m.def {
            Some(d) => println!("{p}\n{}", serde_json::to_string(&**d).unwrap()),
            None => println!("{p}: no definition (class {:?})", m.class),
        }
    }
}
