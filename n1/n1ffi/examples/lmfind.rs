//! Print the baked-light kind and the 2D lightmap (texture paths, scales, atlas transform) of the placements of one package
//! whose mesh path contains a substring. Run from `n1/`: lmfind <CookedXenon dir> <cache dir> <package> <mesh substring>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 5 {
        eprintln!("usage: lmfind <CookedXenon dir> <cache dir> <package> <mesh substring>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let sub = a[4].to_lowercase();
    for pl in db.level_placements(&a[3]) {
        if !pl.mesh.to_lowercase().contains(&sub) {
            continue;
        }
        println!("{} mesh {} kind {} verts {:?}", pl.actor, pl.mesh, pl.lightmap_kind, pl.vertex_light_dir.as_ref().map(|d| d.0.len()));
        if let Some(l) = &pl.lightmap2d {
            println!("  textures {:?} idx {:?}\n  scale {:?}\n  coord scale {:?} bias {:?}", l.textures, l.texture_idx, l.scale, l.coord_scale, l.coord_bias);
        }
    }
}
