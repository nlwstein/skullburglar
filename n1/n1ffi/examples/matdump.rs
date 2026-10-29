use std::path::Path;
fn main() {
    let a: Vec<String> = std::env::args().collect();
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).unwrap();
    for m in &a[3..] {
        println!("==== {m}");
        let Some(j) = db.particle_material_json(m) else { println!("no graph"); continue };
        if std::env::var("JSON").is_ok() { println!("{}", serde_json::to_string_pretty(&j).unwrap()); }
        println!("props: {}", j["props"]);
        let c = scenefmt::particles::matprog::compile_with(&j, &|_| Default::default());
        println!("blend {:?} two_sided {} clip {} soft {} textures {:?} unknown {:?}", c.blend as u8, c.two_sided, c.clip, c.soft, c.textures, c.unknown);
        for t in &c.textures {
            println!("tex {t}: meta {:?}", db.texture_meta(t));
            match db.texture(t) { Some(x) => println!("   loaded {}x{} fmt {:?} mips {}", x.width, x.height, x.fmt, x.mips.len()), None => println!("   texture() = None") }
        }
        if std::env::var("WGSL").is_ok() { println!("{}", c.wgsl); }
    }
}
