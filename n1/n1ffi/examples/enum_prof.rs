//! Where the startup step's enumeration spends its time (B-248): per stage, one thread.
//! `COOKED=<CookedXenon dir> CACHE=<dir> cargo run --release --example enum_prof`
use std::path::Path;
use std::time::Instant;

fn main() {
    let cooked = std::env::var("COOKED").expect("COOKED");
    let cache = std::env::var("CACHE").expect("CACHE");
    let db = assetdb::AssetDb::open(Path::new(&cooked), Path::new(&cache)).unwrap();
    let paths = db.paths_of_classes(&[
        "Material",
        "MaterialInstanceConstant",
        "MaterialInstanceTimeVarying",
        "DecalMaterial",
    ]);
    let (mut t_mat, mut t_meta, mut t_plan) = (0f64, 0f64, 0f64);
    let t_all = Instant::now();
    for p in &paths {
        let t = Instant::now();
        let m = db.material(p);
        t_mat += t.elapsed().as_secs_f64();
        let t = Instant::now();
        let diffuse = m
            .diffuse
            .as_ref()
            .is_some_and(|d| db.texture_meta(d).is_some());
        let metas: Vec<Option<assetdb::TexMeta>> = match m.def.as_deref() {
            Some(d) => d
                .textures
                .iter()
                .map(|t| db.texture_meta(&t.path))
                .collect(),
            None => m
                .diffuse
                .as_ref()
                .map(|d| vec![db.texture_meta(d)])
                .unwrap_or_default(),
        };
        t_meta += t.elapsed().as_secs_f64();
        let t = Instant::now();
        let _ = n1ffi::plan::plan(31, &m, diffuse, &|k| {
            metas
                .get(k)
                .and_then(|x| x.as_ref())
                .map(|x| n1ffi::plan::TexInfo::from_meta(x, true))
                .unwrap_or_default()
        });
        t_plan += t.elapsed().as_secs_f64();
    }
    println!(
        "package opens: {} ({} ms)",
        db.opens.load(std::sync::atomic::Ordering::Relaxed),
        db.open_ms.load(std::sync::atomic::Ordering::Relaxed)
    );
    println!(
        "{} materials: material() {t_mat:.1} s, texture_meta {t_meta:.1} s, plan {t_plan:.1} s, total {:.1} s",
        paths.len(),
        t_all.elapsed().as_secs_f64()
    );
}
