//! Write a texture's top mip as a binary PPM (R,G,B and A as four tiles side by side, so each channel can be seen).
//! Run from `n1/`: cargo run --release -p n1ffi --example texdump -- <CookedXenon dir> <cache dir> <texture path> <out.ppm>
use std::path::Path;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 5 {
        eprintln!("usage: texdump <CookedXenon dir> <cache dir> <texture path> <out.ppm>");
        std::process::exit(2);
    }
    let db = assetdb::AssetDb::open(Path::new(&a[1]), Path::new(&a[2])).expect("open assetdb");
    let t = db.texture(&a[3]).expect("texture not found");
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
    let mut out = format!("P6\n{} {}\n255\n", w * 4, h).into_bytes();
    for y in 0..h {
        for ch in 0..4 {
            for x in 0..w {
                let v = px[(y * w + x) * bpp + ch.min(bpp - 1)];
                out.extend([v, v, v]);
            }
        }
    }
    std::fs::write(&a[4], out).unwrap();
    eprintln!("{}x{} {:?} srgb {} bytes/px {bpp}", t.width, t.height, t.fmt, t.srgb);
}
