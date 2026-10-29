//! Prints every enumerated material path with its startup-step shader key and blend, to compare with the keys a
//! run asks for (`BOTS_N1_COMPILE_LOG=1` logs "material #N is PATH" and "material #N needs shader KEY").
//! `COOKED=<CookedXenon dir> CACHE=<dir> cargo run --release --example enum_keys > keys.tsv`
use std::path::Path;

fn main() {
    let cooked = std::env::var("COOKED").expect("COOKED");
    let cache = std::env::var("CACHE").expect("CACHE");
    let db = assetdb::AssetDb::open(Path::new(&cooked), Path::new(&cache)).unwrap();
    let features: u32 = std::env::var("BOTS_N1_FEATURES")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(31);
    let plans = n1ffi::plan::enumerate(&db, features, true, 4, &|_, _| {}, &|_, _| {});
    for (path, p) in plans {
        println!("{path}\t{:016x}\t{:?}", p.key, p.blend);
    }
}
