//! Print the baked light kind of the placements of a level package whose actor or mesh contains a substring.
//! Run from `n1/`: complight <CookedXenon dir> <cache dir> <package> <substring>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 5 {
        eprintln!("usage: complight <CookedXenon dir> <cache dir> <package> <substring>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    for pl in db.level_placements(&a[3]) {
        if !pl.mesh.contains(&a[4]) && !pl.actor.contains(&a[4]) {
            continue;
        }
        let key = format!("{}.{}", a[3], pl.component);
        let kind = match db.component_light_full(&key).as_deref() {
            None => "none".to_string(),
            Some(assetdb::ComponentLight::Legacy(v)) => format!("legacy {} verts, first {:?}", v.len(), v.first()),
            Some(assetdb::ComponentLight::Dir { words, scale }) => format!("dir {} words scale {scale:?}", words.len()),
            Some(assetdb::ComponentLight::Map { scale, .. }) => format!("map scale {scale:?}"),
        };
        println!("{} class {} comp {} mesh {} -> {kind}", pl.actor, pl.actor_class, pl.component, pl.mesh);
    }
}
