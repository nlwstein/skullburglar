//! UTexture2D (v538, Xenon). Mips live inline or in Textures.tfc / CharTextures.tfc (raw or LZO chunk),
//! are Xenos-tiled (32x32-block macro tiles) and stored as big-endian 16-bit words (32-bit for RGBA8).
//! Untiling follows XGAddress2DTiledOffset (same maths as UEViewer's GetXbox360TiledOffset).

use crate::package::{Package, Reader, decompress_chunk};
use crate::props::{object_props, read_fname};
use anyhow::{Context, Result, bail, ensure};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Fmt {
    Dxt1,
    Dxt3,
    Dxt5,
    Bc5, // "DXN"/ATI2
    G8,
    Rgba8, // PF_A8R8G8B8
}

impl Fmt {
    pub fn from_name(n: &str) -> Option<Self> {
        Some(match n {
            "PF_DXT1" => Fmt::Dxt1,
            "PF_DXT3" => Fmt::Dxt3,
            "PF_DXT5" => Fmt::Dxt5,
            "PF_BC5" | "PF_V8U8" | "PF_ATI2" => Fmt::Bc5,
            "PF_G8" => Fmt::G8,
            "PF_A8R8G8B8" => Fmt::Rgba8,
            _ => return None,
        })
    }
    /// (block w, block h, bytes per block, x360 align x, align y)
    pub fn info(self) -> (usize, usize, usize, usize, usize) {
        match self {
            Fmt::Dxt1 => (4, 4, 8, 128, 128),
            Fmt::Dxt3 | Fmt::Dxt5 | Fmt::Bc5 => (4, 4, 16, 128, 128),
            Fmt::G8 => (1, 1, 1, 64, 64),
            Fmt::Rgba8 => (1, 1, 4, 32, 32),
        }
    }
}

#[derive(Debug, Clone)]
pub struct Texture {
    pub name: String,
    pub format_name: String,
    pub fmt: Option<Fmt>,
    pub width: usize,
    pub height: usize,
    pub srgb: bool,
    pub lod_group: String,
    /// CompressionSettings enum name (TC_Normalmap ...), UnpackMin/Max (sample = tex * (max - min) + min).
    pub compression: String,
    pub unpack_min: [f32; 3],
    pub unpack_max: [f32; 3],
    /// Linear (untiled, little-endian) mips, largest first.
    pub mips: Vec<(usize, usize, Vec<u8>)>,
    pub notes: Vec<String>,
}

pub struct TfcCache {
    dir: PathBuf,
    files: Mutex<HashMap<String, std::sync::Arc<Vec<u8>>>>,
}

impl TfcCache {
    pub fn new(dir: &Path) -> Self {
        Self { dir: dir.to_path_buf(), files: Mutex::new(HashMap::new()) }
    }
    fn get(&self, name: &str) -> Result<std::sync::Arc<Vec<u8>>> {
        let mut m = self.files.lock().unwrap();
        if let Some(f) = m.get(name) {
            return Ok(f.clone());
        }
        let p = self.dir.join(format!("{name}.tfc"));
        let d = std::sync::Arc::new(std::fs::read(&p).with_context(|| format!("read {}", p.display()))?);
        m.insert(name.to_string(), d.clone());
        Ok(d)
    }
}

fn log2(mut n: usize) -> u32 {
    let mut r = 0;
    while n > 1 {
        n >>= 1;
        r += 1;
    }
    r
}

/// XGAddress2DTiledOffset: block (x, y) in an image `width` blocks wide → linear block index in tiled data.
pub fn xenos_tiled_offset(x: usize, y: usize, width: usize, log_bpb: u32) -> usize {
    let aligned_w = (width + 31) & !31;
    let macro_ = ((x >> 5) + (y >> 5) * (aligned_w >> 5)) << (log_bpb + 7);
    let micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpb;
    let offset = macro_ + ((micro & !0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    ((((offset & !0x1FF) << 3)
        + ((y & 16) << 7)
        + ((offset & 0x1C0) << 2)
        + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)
        + (offset & 0x3F))
        >> log_bpb) as usize
}

/// Untile + byte-swap one mip. Returns None if the data doesn't fit an aligned tile (UEViewer drops those too).
pub fn untile(fmt: Fmt, src: &[u8], w: usize, h: usize) -> Option<Vec<u8>> {
    let (bw, bh, bpb, ax, ay) = fmt.info();
    let aw = w.div_ceil(ax) * ax;
    let ah = h.div_ceil(ay) * ay;
    let tbw = aw / bw;
    let tbh = ah / bh;
    let obw = (w / bw).max(1);
    let obh = (h / bh).max(1);
    if tbw * tbh * bpb > src.len() {
        return None;
    }
    let log_bpb = log2(bpb);
    let mut dst = vec![0u8; obw * obh * bpb];
    for dy in 0..obh {
        for dx in 0..obw {
            let a = xenos_tiled_offset(dx, dy, tbw, log_bpb);
            let (sy, sx) = (a / tbw, a % tbw);
            let s = (sy * tbw + sx) * bpb;
            let d = (dy * obw + dx) * bpb;
            if s + bpb > src.len() {
                return None;
            }
            dst[d..d + bpb].copy_from_slice(&src[s..s + bpb]);
        }
    }
    match fmt {
        Fmt::Rgba8 => {
            for c in dst.chunks_exact_mut(4) {
                c.reverse();
            }
        }
        Fmt::G8 => {}
        _ => {
            for c in dst.chunks_exact_mut(2) {
                c.swap(0, 1);
            }
        }
    }
    Some(dst)
}

pub fn read_texture(pkg: &Package, idx: i32, tfc: &TfcCache) -> Result<Texture> {
    let (props, mut r) = object_props(pkg, idx)?;
    let abs = pkg.exports[idx as usize - 1].serial_offset;
    let mut t = Texture {
        name: pkg.obj_path(idx),
        format_name: String::new(),
        fmt: None,
        width: props.get("SizeX").map(|p| p.as_i32() as usize).unwrap_or(0),
        height: props.get("SizeY").map(|p| p.as_i32() as usize).unwrap_or(0),
        srgb: props.get("SRGB").map(|p| p.bool_value).unwrap_or(true),
        lod_group: String::new(),
        compression: String::new(),
        unpack_min: [0.0; 3],
        unpack_max: [1.0; 3],
        mips: vec![],
        notes: vec![],
    };
    let enum_name = |n: &str| -> String {
        props
            .get(n)
            .and_then(|p| if p.data.len() == 8 { read_fname(pkg, &mut p.r()).ok() } else { None })
            .unwrap_or_default()
    };
    t.format_name = enum_name("Format");
    if t.format_name.is_empty() {
        t.format_name = "PF_A8R8G8B8".into(); // default value of the enum when the tag is absent
    }
    t.lod_group = enum_name("LODGroup");
    t.compression = enum_name("CompressionSettings");
    for k in 0..3 {
        if let Some(p) = props.get_idx("UnpackMin", k as i32) {
            t.unpack_min[k] = p.as_f32();
        }
        if let Some(p) = props.get_idx("UnpackMax", k as i32) {
            t.unpack_max[k] = p.as_f32();
        }
    }
    t.fmt = Fmt::from_name(&t.format_name);
    let tfc_name = props
        .get("TextureFileCacheName")
        .and_then(|p| read_fname(pkg, &mut p.r()).ok())
        .unwrap_or_else(|| "Textures".into());
    let tail = props.get("MipTailBaseIdx").map(|p| p.as_i32()).unwrap_or(99) as usize;
    // SourceArt
    crate::mesh::skip_bulk(&mut r, abs)?;
    let nmips = r.i32()?;
    ensure!((0..20).contains(&nmips), "bad mip count {nmips}");
    let mut raw_mips = Vec::new();
    for _ in 0..nmips {
        let flags = r.u32()?;
        let count = r.i32()?;
        let size = r.i32()?;
        let off = r.i32()?;
        let mut data: Option<Vec<u8>> = None;
        if flags & 0x20 == 0 && count > 0 {
            if flags & 0x01 != 0 {
                let f = tfc.get(&tfc_name)?;
                let off = off as usize;
                if flags & 0x10 != 0 {
                    let mut out = vec![0u8; count as usize];
                    decompress_chunk(&f, off, &mut out).context("tfc lzo")?;
                    data = Some(out);
                } else {
                    ensure!(off + count as usize <= f.len(), "tfc range");
                    data = Some(f[off..off + count as usize].to_vec());
                }
            } else {
                // inline
                ensure!(off as usize == abs + r.o, "inline bulk not at cursor (flags {flags:#x})");
                let b = r.bytes(size as usize)?;
                if flags & 0x10 != 0 {
                    let mut out = vec![0u8; count as usize];
                    decompress_chunk(b, 0, &mut out).context("inline lzo")?;
                    data = Some(out);
                } else {
                    data = Some(b.to_vec());
                }
            }
        } else if flags & 0x20 == 0 && size > 0 && flags & 0x01 == 0 {
            r.skip(size as usize)?;
        }
        let sx = r.i32()? as usize;
        let sy = r.i32()? as usize;
        raw_mips.push((sx, sy, data));
    }
    let Some(fmt) = t.fmt else {
        t.notes.push(format!("unsupported format {}", t.format_name));
        return Ok(t);
    };
    // Base size from the first mip entry (top mips may be stripped by the 360 cook).
    let (w0, h0) = raw_mips.first().map(|m| (m.0, m.1)).unwrap_or((t.width, t.height));
    for (level, (_sx, _sy, data)) in raw_mips.iter().enumerate() {
        let Some(d) = data else {
            if !t.mips.is_empty() {
                break;
            }
            continue;
        };
        if level >= tail {
            t.notes.push(format!("stopped at packed mip tail (level {level})"));
            break;
        }
        let (w, h) = ((w0 >> level).max(1), (h0 >> level).max(1));
        let (bw, bh, ..) = fmt.info();
        if w < bw || h < bh {
            break;
        }
        match untile(fmt, d, w, h) {
            Some(lin) => t.mips.push((w, h, lin)),
            None => {
                t.notes.push(format!("mip {level} {w}x{h}: {} bytes don't fill an aligned tile", d.len()));
                break;
            }
        }
    }
    if t.mips.is_empty() {
        bail!("{}: no usable mips ({:?})", t.name, t.notes);
    }
    Ok(t)
}

/// Write mips as a legacy DDS (FourCC DXT1/DXT3/DXT5/ATI2, or uncompressed L8 / A8R8G8B8).
pub fn write_dds(t: &Texture, path: &Path) -> Result<()> {
    let fmt = t.fmt.unwrap();
    let (w, h, _) = &t.mips[0];
    let mut o = Vec::new();
    let p32 = |o: &mut Vec<u8>, v: u32| o.extend_from_slice(&v.to_le_bytes());
    o.extend_from_slice(b"DDS ");
    p32(&mut o, 124);
    let compressed = !matches!(fmt, Fmt::G8 | Fmt::Rgba8);
    p32(&mut o, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | if compressed { 0x80000 } else { 0x8 });
    p32(&mut o, *h as u32);
    p32(&mut o, *w as u32);
    p32(&mut o, if compressed { t.mips[0].2.len() as u32 } else { (*w * fmt.info().2) as u32 });
    p32(&mut o, 0);
    p32(&mut o, t.mips.len() as u32);
    for _ in 0..11 {
        p32(&mut o, 0);
    }
    // pixel format
    p32(&mut o, 32);
    match fmt {
        Fmt::G8 => {
            p32(&mut o, 0x20000); // DDPF_LUMINANCE
            p32(&mut o, 0);
            p32(&mut o, 8);
            p32(&mut o, 0xff);
            p32(&mut o, 0);
            p32(&mut o, 0);
            p32(&mut o, 0);
        }
        Fmt::Rgba8 => {
            p32(&mut o, 0x41); // RGB | ALPHAPIXELS
            p32(&mut o, 0);
            p32(&mut o, 32);
            p32(&mut o, 0x00ff0000);
            p32(&mut o, 0x0000ff00);
            p32(&mut o, 0x000000ff);
            p32(&mut o, 0xff000000);
        }
        _ => {
            p32(&mut o, 0x4);
            o.extend_from_slice(match fmt {
                Fmt::Dxt1 => b"DXT1",
                Fmt::Dxt3 => b"DXT3",
                Fmt::Dxt5 => b"DXT5",
                _ => b"ATI2",
            });
            for _ in 0..5 {
                p32(&mut o, 0);
            }
        }
    }
    p32(&mut o, 0x1000 | 0x8 | 0x400000);
    for _ in 0..4 {
        p32(&mut o, 0);
    }
    for (_, _, d) in &t.mips {
        if fmt == Fmt::Rgba8 {
            // stored ARGB big-endian -> after dword reverse it's BGRA in memory = A8R8G8B8 little-endian
            o.extend_from_slice(d);
        } else {
            o.extend_from_slice(d);
        }
    }
    std::fs::write(path, o)?;
    Ok(())
}

/// Read a level's inline lightmap textures etc. share this path; helper to read a fixed `Reader` slice.
pub fn _unused(_r: &Reader) {}
