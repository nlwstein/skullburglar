//! UStaticMesh (v538). Layout cross-checked against UEViewer's UnMesh3.cpp (generic UE3 path; UEViewer has
//! no BotS-specific StaticMesh quirks, only SkeletalMesh ones).

use crate::package::{Package, Reader};
use crate::props::object_props;
use anyhow::{Context, Result, ensure};

#[derive(Debug, Clone, Default)]
pub struct Section {
    pub material: String, // full object path (package-local), "" if none
    pub material_idx: i32,
    pub first_index: u32,
    pub num_tris: u32,
    pub min_vert: u32,
    pub max_vert: u32,
}

#[derive(Debug, Clone, Default)]
pub struct Lod {
    pub sections: Vec<Section>,
    pub positions: Vec<[f32; 3]>,
    pub normals: Vec<[f32; 3]>,
    pub tangents: Vec<[f32; 4]>,
    pub uvs: Vec<Vec<[f32; 2]>>, // per texcoord set
    pub colors: Vec<[u8; 4]>,
    pub indices: Vec<u16>,
    /// True when `colors` came from the separate colour stream (bytes in file order).
    pub color_stream: bool,
}

#[derive(Debug, Clone, Default)]
pub struct StaticMesh {
    pub bounds_origin: [f32; 3],
    pub bounds_extent: [f32; 3],
    pub sphere_radius: f32,
    pub internal_version: i32,
    pub lods: Vec<Lod>,
    /// Bytes left unread after the LOD array (should be 0 if the layout is right).
    pub trailing: usize,
}

/// Packed normal: 4 bytes in file order W, Z, Y, X, each (b - 128) / 127.5.
pub fn unpack_normal(b: &[u8]) -> [f32; 4] {
    let f = |x: u8| (x as f32 - 127.5) / 127.5;
    // Xenon stores the packed word big-endian-reversed: bytes on disc are W Z Y X (checked: reversed normals agree
    // with the face normals on 97% of triangles, file order on 41%).
    [f(b[3]), f(b[2]), f(b[1]), f(b[0])]
}

pub fn half(h: u16) -> f32 {
    let s = ((h >> 15) & 1) as u32;
    let e = ((h >> 10) & 0x1f) as i32;
    let m = (h & 0x3ff) as u32;
    let v = if e == 0 {
        (m as f32) * (2f32).powi(-24)
    } else if e == 31 {
        if m == 0 { f32::INFINITY } else { f32::NAN }
    } else {
        f32::from_bits(((e - 15 + 127) as u32) << 23 | (m << 13))
    };
    if s == 1 { -v } else { v }
}

/// Skip a FByteBulkData header (+ its inline payload).
pub fn skip_bulk(r: &mut Reader, abs_base: usize) -> Result<(u32, i32, i32, i32)> {
    let flags = r.u32()?;
    let count = r.i32()?;
    let size = r.i32()?;
    let off = r.i32()?;
    if flags & 0x20 == 0 && off as i64 == (abs_base + r.o) as i64 && size > 0 {
        r.skip(size as usize)?;
    }
    Ok((flags, count, size, off))
}

pub fn read_static_mesh(pkg: &Package, idx: i32) -> Result<StaticMesh> {
    let (_props, mut r) = object_props(pkg, idx)?;
    let abs = pkg.exports[idx as usize - 1].serial_offset;
    let mut m = StaticMesh::default();
    m.bounds_origin = r.vec3()?;
    m.bounds_extent = r.vec3()?;
    m.sphere_radius = r.f32()?;
    let _body_setup = r.i32()?;
    r.bulk_array().context("kdop nodes")?;
    r.bulk_array().context("kdop tris")?;
    m.internal_version = r.i32()?;
    if m.internal_version >= 17 {
        r.tarray_raw(8)?; // TArray<FName>
    }
    let nlods = r.i32()?;
    ensure!((0..16).contains(&nlods), "bad lod count {nlods}");
    for li in 0..nlods {
        m.lods.push(read_lod(pkg, &mut r, abs).with_context(|| format!("lod {li}"))?);
    }
    m.trailing = r.remaining();
    Ok(m)
}

fn read_lod(pkg: &Package, r: &mut Reader, abs: usize) -> Result<Lod> {
    let mut lod = Lod::default();
    skip_bulk(r, abs)?;
    let ns = r.i32()?;
    ensure!((0..512).contains(&ns), "bad section count {ns}");
    for _ in 0..ns {
        let mat = r.i32()?;
        let _f10 = r.i32()?;
        let _f14 = r.i32()?;
        let _shadow = r.i32()?;
        let _lic73_extra = r.i32()?; // BotS-only: one extra int before FirstIndex (usually 1)
        let first_index = r.i32()? as u32;
        let num_tris = r.i32()? as u32;
        let min_vert = r.i32()? as u32;
        let max_vert = r.i32()? as u32;
        let _index = r.i32()?;
        r.tarray_raw(8)?; // f30: TArray<int[2]>
        lod.sections.push(Section {
            material: if mat != 0 { pkg.obj_path(mat) } else { String::new() },
            material_idx: mat,
            first_index,
            num_tris,
            min_vert,
            max_vert,
        });
    }
    // position stream
    let _vsize = r.i32()?;
    let nverts = r.i32()?;
    let (es, n, d) = r.bulk_array()?;
    ensure!(es == 12 && n as i32 == nverts, "vertex stream es={es} n={n} nv={nverts}");
    lod.positions = d
        .chunks_exact(12)
        .map(|c| {
            let mut rr = Reader::new(c, 0);
            rr.vec3().unwrap()
        })
        .collect();
    // UV/tangent stream
    let ntc = r.i32()? as usize;
    let _item = r.i32()?;
    let _nv = r.i32()?;
    let full = r.i32()? != 0;
    let (es, n, d) = r.bulk_array()?;
    let uvsz = if full { 8 } else { 4 } * ntc;
    ensure!(es == 8 + 4 + uvsz || es == 8 + uvsz, "uv item size {es} (tc={ntc} full={full})");
    let has_color = es == 8 + 4 + uvsz;
    lod.uvs = vec![Vec::with_capacity(n); ntc];
    for c in d.chunks_exact(es) {
        let t = unpack_normal(&c[0..4]);
        let nz = unpack_normal(&c[4..8]);
        lod.tangents.push([t[0], t[1], t[2], if nz[3] < 0.0 { -1.0 } else { 1.0 }]);
        lod.normals.push([nz[0], nz[1], nz[2]]);
        let mut o = 8;
        if has_color {
            lod.colors.push([c[8], c[9], c[10], c[11]]);
            o += 4;
        }
        let mut rr = Reader::new(c, o);
        for t in 0..ntc {
            let uv = if full {
                [rr.f32()?, rr.f32()?]
            } else {
                [half(rr.u16()?), half(rr.u16()?)]
            };
            lod.uvs[t].push(uv);
        }
    }
    // shadow volume stream (v < 686)
    // (UEViewer: this is the FStaticMeshColorStream when ver >= 615: ItemSize, NumVerts, bulk of FColor.)
    let cs_item = r.i32()?;
    let cs_n = r.i32()?;
    let (ces, cn, cd) = r.bulk_array().context("color stream")?;
    if ces == 4 && cn == nverts as usize && lod.colors.is_empty() {
        // FColor in a big-endian archive is read as a dword: B,G,R,A byte order in file = ARGB dword -> bytes A,R,G,B.
        lod.colors = cd.chunks_exact(4).map(|c| [c[0], c[1], c[2], c[3]]).collect();
        lod.color_stream = true;
    }
    let _ = (cs_item, cs_n);
    let _numverts = r.i32()?;
    let (es, _n, d) = r.bulk_array().context("indices")?;
    ensure!(es == 2, "index size {es}");
    lod.indices = d.chunks_exact(2).map(|c| u16::from_be_bytes([c[0], c[1]])).collect();
    r.bulk_array().context("wire indices")?;
    r.bulk_array().context("edges")?;
    r.tarray_raw(1).context("fEC")?;
    Ok(lod)
}
