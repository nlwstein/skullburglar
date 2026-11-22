//! Write a level package's LightMapTexture2D (by export index) as a binary PPM (RGB). Run from `n1/`:
//!   lmdump <CookedXenon dir> <cache dir> <package> <export index> <out.ppm>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 6 {
        eprintln!("usage: lmdump <CookedXenon dir> <cache dir> <package> <export index> <out.ppm>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let t = db.lightmap_texture(&a[3], a[4].parse().unwrap()).expect("lightmap not found");
    let f = match t.fmt {
        assetdb::TexFmt::Bc1 => scenekit::TexFormat::Bc1,
        assetdb::TexFmt::Bc2 => scenekit::TexFormat::Bc2,
        assetdb::TexFmt::Bc3 => scenekit::TexFormat::Bc3,
        assetdb::TexFmt::Bc5 => scenekit::TexFormat::Bc5,
        _ => panic!("format {:?} not handled", t.fmt),
    };
    let px = n1ffi::bcdec::decode_mip(f, t.width, t.height, &t.mips[0]).expect("decode");
    let bpp = px.len() / (t.width * t.height) as usize;
    let (w, h) = (t.width as usize, t.height as usize);
    let mut out = format!("P6\n{w} {h}\n255\n").into_bytes();
    for i in 0..w * h {
        out.extend([px[i * bpp], px[i * bpp + 1.min(bpp - 1)], px[i * bpp + 2.min(bpp - 1)]]);
    }
    std::fs::write(&a[5], out).unwrap();
    eprintln!("{w}x{h} {:?} srgb {} mips {} bytes/px {bpp}", t.fmt, t.srgb, t.mips.len());
}
