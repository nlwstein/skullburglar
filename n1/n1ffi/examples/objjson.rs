//! Print an object's property list as JSON. Run from `n1/`: objjson <CookedXenon dir> <cache dir> <object path>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: objjson <CookedXenon dir> <cache dir> <object path>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    match db.object_json(&a[3]) {
        Some(j) => println!("{}", serde_json::to_string_pretty(&j).unwrap()),
        None => println!("(not found)"),
    }
}
