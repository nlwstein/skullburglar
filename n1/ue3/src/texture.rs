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
    /// AddressX / AddressY enum names (`TA_Wrap` when absent): how a material sample outside 0..1 behaves.
    pub address_x: String,
    pub address_y: String,
    pub unpack_min: [f32; 3],
    pub unpack_max: [f32; 3],
    /// Linear (untiled, little-endian) mips, largest first.
    pub mips: Vec<(usize, usize, Vec<u8>)>,
    pub notes: Vec<String>,
}

/// The texture file caches (`Textures.tfc`, `CharTextures.tfc`). A file is opened once and a texture's mip is read from
/// its offset when asked for: `Textures.tfc` is 1.6 GB, and reading it whole made every first-time texture decode (a
/// cold n1 cache, which is every player's first visit to a level) hold 1.6 GB of host memory for the rest of the run
/// (B-296: the Steam Deck's 1.5 GB jump in one step). `BOTS_N1_TFC_SLURP=1` brings the old whole-file read back.
pub struct TfcCache {
    dir: PathBuf,
    files: Mutex<HashMap<String, std::sync::Arc<TfcFile>>>,
    slurp: bool,
}

enum TfcFile {
    /// The whole file in memory (`BOTS_N1_TFC_SLURP=1`).
    Whole(Vec<u8>),
    File(std::fs::File, u64),
}

impl TfcFile {
    fn len(&self) -> u64 {
        match self {
            TfcFile::Whole(v) => v.len() as u64,
            TfcFile::File(_, n) => *n,
        }
    }

    /// `len` bytes at `off`; an error past the end of the file.
    fn read(&self, off: usize, len: usize) -> Result<Vec<u8>> {
        ensure!(off as u64 + len as u64 <= self.len(), "tfc range");
        match self {
            TfcFile::Whole(v) => Ok(v[off..off + len].to_vec()),
            TfcFile::File(f, _) => {
                let mut buf = vec![0u8; len];
                read_exact_at(f, &mut buf, off as u64).context("tfc read")?;
                Ok(buf)
            }
        }
    }
}

#[cfg(unix)]
fn read_exact_at(f: &std::fs::File, buf: &mut [u8], off: u64) -> std::io::Result<()> {
    std::os::unix::fs::FileExt::read_exact_at(f, buf, off)
}

#[cfg(windows)]
fn read_exact_at(f: &std::fs::File, mut buf: &mut [u8], mut off: u64) -> std::io::Result<()> {
    while !buf.is_empty() {
        let n = std::os::windows::fs::FileExt::seek_read(f, buf, off)?;
        if n == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        buf = &mut buf[n..];
        off += n as u64;
    }
    Ok(())
}

impl TfcCache {
    pub fn new(dir: &Path) -> Self {
        let slurp = std::env::var("BOTS_N1_TFC_SLURP").is_ok_and(|v| !v.is_empty() && v != "0");
        Self { dir: dir.to_path_buf(), files: Mutex::new(HashMap::new()), slurp }
    }
    /// BOTS_MEMLOG: files open and the bytes of them held in memory.
    pub fn mem(&self) -> (usize, u64) {
        let m = self.files.lock().unwrap();
        (m.len(), m.values().map(|f| if let TfcFile::Whole(v) = &**f { v.len() as u64 } else { 0 }).sum())
    }
    fn get(&self, name: &str) -> Result<std::sync::Arc<TfcFile>> {
        let mut m = self.files.lock().unwrap();
        if let Some(f) = m.get(name) {
            return Ok(f.clone());
        }
        let p = self.dir.join(format!("{name}.tfc"));
        let f = if self.slurp {
            TfcFile::Whole(std::fs::read(&p).with_context(|| format!("read {}", p.display()))?)
        } else {
            let f = std::fs::File::open(&p).with_context(|| format!("open {}", p.display()))?;
            let n = f.metadata().with_context(|| format!("stat {}", p.display()))?.len();
            TfcFile::File(f, n)
        };
        let f = std::sync::Arc::new(f);
        m.insert(name.to_string(), f.clone());
        Ok(f)
    }
    /// `len` bytes of file `name` at `off`.
    fn read(&self, name: &str, off: usize, len: usize) -> Result<Vec<u8>> {
        self.get(name)?.read(off, len)
    }
    /// The bytes of the LZO chunk at `off` (header, block table, blocks), for `decompress_chunk` at offset 0.
    fn chunk(&self, name: &str, off: usize) -> Result<Vec<u8>> {
        let f = self.get(name)?;
        let head = f.read(off, 16)?;
        let mut r = Reader::new(&head, 4);
        let block = (r.u32()? as usize).max(1);
        let _ctotal = r.i32()?;
        let total = r.i32()?.max(0) as usize;
        let n = total.div_ceil(block);
        let mut out = f.read(off, 16 + 8 * n)?;
        let mut t = Reader::new(&out, 16);
        let mut comp = 0usize;
        for _ in 0..n {
            comp += t.i32()?.max(0) as usize;
            let _ = t.i32()?;
        }
        out.extend_from_slice(&f.read(off + 16 + 8 * n, comp)?);
        Ok(out)
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
    untile_at(fmt, src, w, h, (0, 0))
}

/// Mip 0 of a texture with mips and a side of at most 16 texels sits inside a packed tile (Xenia's GetPackedTileOffset).
/// BOTS_N1_PACKED_MIP0=0 decodes it from the tile corner as before (A/B).
pub fn packed_mip0_on() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var("BOTS_N1_PACKED_MIP0").map_or(true, |v| v != "0"))
}

/// True for a texture size whose mips pack into one 32x32 tile (the smaller side is at most 16 texels).
pub fn packs_mips(w: usize, h: usize) -> bool {
    std::cmp::min(w.next_power_of_two(), h.next_power_of_two()) <= 16
}

/// `untile` for an image whose top-left block sits at block `origin` inside its tile (a packed mip tail's first mip).
pub fn untile_at(fmt: Fmt, src: &[u8], w: usize, h: usize, origin: (usize, usize)) -> Option<Vec<u8>> {
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
            let a = xenos_tiled_offset(dx + origin.0, dy + origin.1, tbw, log_bpb);
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
        address_x: String::new(),
        address_y: String::new(),
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
    t.address_x = enum_name("AddressX");
    t.address_y = enum_name("AddressY");
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
                let off = off as usize;
                if flags & 0x10 != 0 {
                    let mut out = vec![0u8; count as usize];
                    let chunk = tfc.chunk(&tfc_name, off).context("tfc lzo")?;
                    decompress_chunk(&chunk, 0, &mut out).context("tfc lzo")?;
                    data = Some(out);
                } else {
                    data = Some(tfc.read(&tfc_name, off, count as usize)?);
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
        // A texture with mips and a side of at most 16 texels keeps its mips packed into one 32x32 tile (Xenia's
        // GetPackedTileOffset): mip 0 sits 16 texels down (wider than tall) or across (otherwise), not at the corner.
        let origin = if level == 0 && raw_mips.len() > 1 && packed_mip0_on() && packs_mips(w, h) {
            if w.next_power_of_two() > h.next_power_of_two() { (0, 16 / bh) } else { (16 / bw, 0) }
        } else {
            (0, 0)
        };
        match untile_at(fmt, d, w, h, origin) {
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

#[cfg(test)]
mod tfc_tests {
    use super::*;
    use crate::package::PACKAGE_TAG;

    /// A tfc with a raw run at 100 and a two-block LZO chunk at 200, then trailing bytes the chunk reader must not need.
    fn sample() -> (PathBuf, Vec<u8>, Vec<u8>) {
        let dir = std::env::temp_dir().join(format!("tfc-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let raw: Vec<u8> = (0..50u8).collect();
        let plain: Vec<u8> = (0..100u32).map(|i| (i * 7 % 13) as u8).collect();
        let (a, b) = plain.split_at(64);
        let ca = lzokay_native::compress(a).unwrap();
        let cb = lzokay_native::compress(b).unwrap();
        let mut f = vec![0xEE; 100];
        f.extend_from_slice(&raw);
        f.resize(200, 0xDD);
        f.extend_from_slice(&PACKAGE_TAG.to_be_bytes());
        f.extend_from_slice(&64u32.to_be_bytes());
        f.extend_from_slice(&((ca.len() + cb.len()) as i32).to_be_bytes());
        f.extend_from_slice(&(plain.len() as i32).to_be_bytes());
        for (c, u) in [(ca.len(), 64usize), (cb.len(), 36)] {
            f.extend_from_slice(&(c as i32).to_be_bytes());
            f.extend_from_slice(&(u as i32).to_be_bytes());
        }
        f.extend_from_slice(&ca);
        f.extend_from_slice(&cb);
        f.extend_from_slice(&[0xCC; 40]);
        std::fs::write(dir.join("T.tfc"), &f).unwrap();
        (dir, raw, plain)
    }

    #[test]
    fn reads_by_offset_match_the_whole_file() {
        let (dir, raw, plain) = sample();
        for slurp in [false, true] {
            let c = TfcCache { dir: dir.clone(), files: Mutex::new(HashMap::new()), slurp };
            assert_eq!(c.read("T", 100, 50).unwrap(), raw, "raw run, slurp {slurp}");
            let chunk = c.chunk("T", 200).unwrap();
            let mut out = vec![0u8; plain.len()];
            decompress_chunk(&chunk, 0, &mut out).unwrap();
            assert_eq!(out, plain, "lzo chunk, slurp {slurp}");
            assert!(c.read("T", 1_000_000, 4).is_err(), "past the end, slurp {slurp}");
            let (files, held) = c.mem();
            assert_eq!(files, 1);
            assert_eq!(held > 0, slurp, "only the old mode keeps the file in memory");
        }
        let _ = std::fs::remove_dir_all(dir);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packed_mip_origin_shifts_the_decode() {
        // A full 32x32 tile with a distinct value per byte; decode it whole, then decode a 1x1 image whose top-left
        // block sits at (3, 4) inside the same tile: it must be the whole decode's pixel (3, 4), not the corner.
        let src: Vec<u8> = (0..32 * 32 * 4).map(|i| (i * 7 % 251) as u8).collect();
        let full = untile_at(Fmt::Rgba8, &src, 32, 32, (0, 0)).unwrap();
        let one = untile_at(Fmt::Rgba8, &src, 1, 1, (3, 4)).unwrap();
        assert_eq!(one, full[(4 * 32 + 3) * 4..(4 * 32 + 3) * 4 + 4]);
        assert_ne!(one, untile_at(Fmt::Rgba8, &src, 1, 1, (0, 0)).unwrap());
        assert_eq!(untile(Fmt::Rgba8, &src, 32, 32).unwrap(), full);
    }

    #[test]
    fn packs_mips_needs_a_side_of_16_or_less() {
        assert!(packs_mips(256, 4));
        assert!(packs_mips(16, 16));
        assert!(packs_mips(8, 64));
        assert!(!packs_mips(32, 32));
        assert!(!packs_mips(256, 128));
    }
}
