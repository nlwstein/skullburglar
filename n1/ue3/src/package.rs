//! UE3 cooked package reader for 50 Cent: Blood on the Sand (Xenon, v538 / licensee 73).
//!
//! Big-endian, LZO-compressed chunks. The whole body is decompressed into memory at its
//! logical offsets, so export serial offsets index straight into `data`.

use anyhow::{Context, Result, bail, ensure};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

pub const PACKAGE_TAG: u32 = 0x9E2A83C1;

/// Big-endian cursor over a byte slice.
#[derive(Clone)]
pub struct Reader<'a> {
    pub d: &'a [u8],
    pub o: usize,
}

impl<'a> Reader<'a> {
    pub fn new(d: &'a [u8], o: usize) -> Self {
        Self { d, o }
    }
    pub fn remaining(&self) -> usize {
        self.d.len().saturating_sub(self.o)
    }
    pub fn bytes(&mut self, n: usize) -> Result<&'a [u8]> {
        ensure!(self.o + n <= self.d.len(), "read past end ({} + {} > {})", self.o, n, self.d.len());
        let s = &self.d[self.o..self.o + n];
        self.o += n;
        Ok(s)
    }
    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.bytes(1)?[0])
    }
    pub fn u16(&mut self) -> Result<u16> {
        Ok(u16::from_be_bytes(self.bytes(2)?.try_into()?))
    }
    pub fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_be_bytes(self.bytes(4)?.try_into()?))
    }
    pub fn i32(&mut self) -> Result<i32> {
        Ok(i32::from_be_bytes(self.bytes(4)?.try_into()?))
    }
    pub fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_be_bytes(self.bytes(8)?.try_into()?))
    }
    pub fn f32(&mut self) -> Result<f32> {
        Ok(f32::from_be_bytes(self.bytes(4)?.try_into()?))
    }
    pub fn vec3(&mut self) -> Result<[f32; 3]> {
        Ok([self.f32()?, self.f32()?, self.f32()?])
    }
    pub fn skip(&mut self, n: usize) -> Result<()> {
        self.bytes(n).map(|_| ())
    }
    /// UE3 FString: i32 length (negative = UTF-16), includes the terminator.
    pub fn fstring(&mut self) -> Result<String> {
        let n = self.i32()?;
        if n == 0 {
            return Ok(String::new());
        }
        if n > 0 {
            let b = self.bytes(n as usize)?;
            Ok(String::from_utf8_lossy(&b[..b.len() - 1]).into_owned())
        } else {
            let n = (-n) as usize;
            let mut v = Vec::with_capacity(n);
            for _ in 0..n {
                v.push(self.u16()?);
            }
            v.pop();
            Ok(String::from_utf16_lossy(&v))
        }
    }
    /// Count-prefixed array of fixed-size elements ("TArray"); returns (count, slice).
    pub fn tarray_raw(&mut self, elem: usize) -> Result<(usize, &'a [u8])> {
        let n = self.i32()?;
        ensure!(n >= 0 && (n as usize) < 64 << 20, "bad array count {n}");
        let n = n as usize;
        Ok((n, self.bytes(n * elem)?))
    }
    /// UE3 BulkSerialize: i32 element size, then TArray.
    pub fn bulk_array(&mut self) -> Result<(usize, usize, &'a [u8])> {
        let es = self.i32()?;
        ensure!((0..4096).contains(&es), "bad bulk element size {es}");
        let (n, s) = self.tarray_raw(es as usize)?;
        Ok((es as usize, n, s))
    }
}

#[derive(Debug, Clone)]
pub struct Import {
    pub class_package: String,
    pub class_name: String,
    pub outer: i32,
    pub name: String,
}

#[derive(Debug, Clone)]
pub struct Export {
    pub class: i32,
    pub super_: i32,
    pub outer: i32,
    pub name: String,
    pub archetype: i32,
    pub flags: u64,
    pub serial_size: usize,
    pub serial_offset: usize,
    pub export_flags: u32,
}

pub struct Package {
    pub path: PathBuf,
    pub name: String,
    pub version: u16,
    pub licensee: u16,
    pub data: Vec<u8>,
    pub names: Vec<String>,
    pub imports: Vec<Import>,
    pub exports: Vec<Export>,
}

fn name_str(names: &[String], idx: i32, num: i32) -> String {
    let base = names.get(idx as usize).cloned().unwrap_or_else(|| format!("<name {idx}>"));
    if num > 0 { format!("{base}_{}", num - 1) } else { base }
}

impl Package {
    pub fn open(path: &Path) -> Result<Self> {
        let raw = std::fs::read(path).with_context(|| format!("read {}", path.display()))?;
        let name = path.file_stem().unwrap().to_string_lossy().into_owned();
        Self::from_bytes(path.to_path_buf(), name, raw)
    }

    pub fn from_bytes(path: PathBuf, name: String, raw: Vec<u8>) -> Result<Self> {
        let mut r = Reader::new(&raw, 0);
        ensure!(r.u32()? == PACKAGE_TAG, "{}: not a UE3 package", path.display());
        let ver = r.u32()?;
        let (version, licensee) = ((ver & 0xffff) as u16, (ver >> 16) as u16);
        let _header_size = r.i32()?;
        let _folder = r.fstring()?;
        let _pkg_flags = r.u32()?;
        let name_count = r.i32()? as usize;
        let name_off = r.i32()? as usize;
        let export_count = r.i32()? as usize;
        let export_off = r.i32()? as usize;
        let import_count = r.i32()? as usize;
        let import_off = r.i32()? as usize;
        let _depends_off = r.i32()?;
        r.skip(16)?; // guid
        let gens = r.i32()? as usize;
        r.skip(gens * 12)?;
        let _engine = r.i32()?;
        let _cooker = r.i32()?;
        let cflags = r.u32()?;
        let nchunks = r.i32()? as usize;
        let mut chunks = Vec::new();
        for _ in 0..nchunks {
            chunks.push((r.i32()? as usize, r.i32()? as usize, r.i32()? as usize, r.i32()? as usize));
        }
        let data = if chunks.is_empty() {
            raw
        } else {
            ensure!(cflags == 2, "unsupported compression flags {cflags}");
            let first = chunks.iter().map(|c| c.0).min().unwrap();
            let end = chunks.iter().map(|c| c.0 + c.1).max().unwrap();
            let mut out = vec![0u8; end];
            out[..first].copy_from_slice(&raw[..first]);
            for &(uoff, usz, coff, _csz) in &chunks {
                decompress_chunk(&raw, coff, &mut out[uoff..uoff + usz])
                    .with_context(|| format!("chunk at {coff:#x}"))?;
            }
            out
        };

        let mut r = Reader::new(&data, name_off);
        let mut names = Vec::with_capacity(name_count);
        for _ in 0..name_count {
            names.push(r.fstring()?);
            r.u64()?;
        }
        let mut r = Reader::new(&data, import_off);
        let mut imports = Vec::with_capacity(import_count);
        for _ in 0..import_count {
            let cp = (r.i32()?, r.i32()?);
            let cn = (r.i32()?, r.i32()?);
            let outer = r.i32()?;
            let n = (r.i32()?, r.i32()?);
            imports.push(Import {
                class_package: name_str(&names, cp.0, cp.1),
                class_name: name_str(&names, cn.0, cn.1),
                outer,
                name: name_str(&names, n.0, n.1),
            });
        }
        let mut r = Reader::new(&data, export_off);
        let mut exports = Vec::with_capacity(export_count);
        for _ in 0..export_count {
            let class = r.i32()?;
            let super_ = r.i32()?;
            let outer = r.i32()?;
            let n = (r.i32()?, r.i32()?);
            let archetype = r.i32()?;
            let flags = r.u64()?;
            let serial_size = r.i32()? as usize;
            let serial_offset = r.i32()? as usize;
            let ncomp = r.i32()? as usize;
            r.skip(ncomp * 12)?;
            let export_flags = r.u32()?;
            let nnet = r.i32()? as usize;
            r.skip(nnet * 4)?;
            r.skip(16 + 4)?;
            exports.push(Export {
                class,
                super_,
                outer,
                name: name_str(&names, n.0, n.1),
                archetype,
                flags,
                serial_size,
                serial_offset,
                export_flags,
            });
        }
        Ok(Self { path, name, version, licensee, data, names, imports, exports })
    }

    pub fn name(&self, idx: i32, num: i32) -> String {
        name_str(&self.names, idx, num)
    }

    /// Object name of a package index (0 = None).
    pub fn obj_name(&self, idx: i32) -> &str {
        if idx > 0 {
            &self.exports[idx as usize - 1].name
        } else if idx < 0 {
            &self.imports[(-idx) as usize - 1].name
        } else {
            "None"
        }
    }

    pub fn outer_of(&self, idx: i32) -> i32 {
        if idx > 0 {
            self.exports[idx as usize - 1].outer
        } else if idx < 0 {
            self.imports[(-idx) as usize - 1].outer
        } else {
            0
        }
    }

    /// Dotted path *without* the package for exports (outer chain), e.g. `Env.Meshes.Wall01`.
    /// For imports the top outer is the source package name.
    pub fn obj_path(&self, idx: i32) -> String {
        let mut parts = vec![self.obj_name(idx).to_string()];
        let mut o = self.outer_of(idx);
        let mut guard = 0;
        while o != 0 && guard < 64 {
            parts.push(self.obj_name(o).to_string());
            o = self.outer_of(o);
            guard += 1;
        }
        parts.reverse();
        parts.join(".")
    }

    pub fn class_name(&self, idx: i32) -> &str {
        if idx > 0 {
            let c = self.exports[idx as usize - 1].class;
            if c == 0 { "Class" } else { self.obj_name(c) }
        } else if idx < 0 {
            &self.imports[(-idx) as usize - 1].class_name
        } else {
            "None"
        }
    }

    pub fn export_data(&self, idx: i32) -> &[u8] {
        let e = &self.exports[idx as usize - 1];
        &self.data[e.serial_offset..e.serial_offset + e.serial_size]
    }

    /// Exports whose class is `class`.
    pub fn exports_of_class<'s>(&'s self, class: &'s str) -> impl Iterator<Item = i32> + 's {
        (1..=self.exports.len() as i32).filter(move |&i| self.class_name(i) == class)
    }

    pub fn class_histogram(&self) -> Vec<(String, usize)> {
        let mut h: HashMap<String, usize> = HashMap::new();
        for i in 1..=self.exports.len() as i32 {
            *h.entry(self.class_name(i).to_string()).or_default() += 1;
        }
        let mut v: Vec<_> = h.into_iter().collect();
        v.sort_by(|a, b| b.1.cmp(&a.1));
        v
    }
}

/// One LZO compressed chunk: tag, block size, compressed total, uncompressed total, block table, blocks.
pub fn decompress_chunk(raw: &[u8], off: usize, out: &mut [u8]) -> Result<()> {
    let mut r = Reader::new(raw, off);
    ensure!(r.u32()? == PACKAGE_TAG, "bad chunk tag");
    let _block = r.u32()?;
    let _ctotal = r.i32()?;
    let total = r.i32()? as usize;
    ensure!(total == out.len(), "chunk size {total} != {}", out.len());
    let mut blocks = Vec::new();
    let mut got = 0;
    while got < total {
        let (c, u) = (r.i32()? as usize, r.i32()? as usize);
        blocks.push((c, u));
        got += u;
    }
    let mut pos = 0;
    for (c, u) in blocks {
        let src = r.bytes(c)?;
        let dec = lzokay_native::decompress_all(src, Some(u)).map_err(|e| anyhow::anyhow!("lzo: {e:?}"))?;
        if dec.len() != u {
            bail!("lzo block {} != {}", dec.len(), u);
        }
        out[pos..pos + u].copy_from_slice(&dec);
        pos += u;
    }
    Ok(())
}
