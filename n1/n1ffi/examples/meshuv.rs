//! Find static meshes with a section using a material (substring match) and print the vertex UV ranges of those
//! sections. Run from `n1/`: meshuv <CookedXenon dir> <cache dir> <material substring> [--verts]
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: meshuv <CookedXenon dir> <cache dir> <material substring> [--verts]");
        std::process::exit(2);
    }
    let verts = a.iter().any(|x| x == "--verts");
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let sub = a[3].to_lowercase();
    for p in db.paths_of_classes(&["StaticMesh"]) {
        let Some(m) = db.mesh(&p) else { continue };
        for (si, s) in m.sections.iter().enumerate() {
            if !s.material.to_lowercase().contains(&sub) {
                continue;
            }
            let idx = &m.indices[s.first_index as usize..(s.first_index + s.index_count) as usize];
            let mut used: Vec<u32> = idx.to_vec();
            used.sort_unstable();
            used.dedup();
            let (mut lo, mut hi) = ([f32::MAX; 2], [f32::MIN; 2]);
            for &i in &used {
                let uv = m.uvs[i as usize];
                for k in 0..2 {
                    lo[k] = lo[k].min(uv[k]);
                    hi[k] = hi[k].max(uv[k]);
                }
            }
            println!("{p} section {si} mat {} verts {} tris {} u [{:.3},{:.3}] v [{:.3},{:.3}] radius {:.1}",
                s.material, used.len(), idx.len() / 3, lo[0], hi[0], lo[1], hi[1], m.radius);
            if verts {
                for &i in &used {
                    let (pp, uv) = (m.positions[i as usize], m.uvs[i as usize]);
                    println!("  v{i} pos ({:.1},{:.1},{:.1}) uv ({:.3},{:.3})", pp[0], pp[1], pp[2], uv[0], uv[1]);
                }
            }
        }
    }
}
