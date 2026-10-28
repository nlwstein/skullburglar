//! UE3 tagged properties (v538): Name, Type, Size, ArrayIndex, [StructName | BoolValue(i32)], value.

use crate::package::{Package, Reader};
use anyhow::{Result, ensure};

#[derive(Debug, Clone)]
pub struct Prop<'a> {
    pub name: String,
    pub ty: String,
    pub struct_name: String,
    pub array_index: i32,
    pub bool_value: bool,
    pub data: &'a [u8],
}

impl<'a> Prop<'a> {
    pub fn r(&self) -> Reader<'a> {
        Reader::new(self.data, 0)
    }
    pub fn as_obj(&self) -> i32 {
        self.r().i32().unwrap_or(0)
    }
    pub fn as_f32(&self) -> f32 {
        self.r().f32().unwrap_or(0.0)
    }
    pub fn as_i32(&self) -> i32 {
        self.r().i32().unwrap_or(0)
    }
    pub fn as_vec3(&self) -> Option<[f32; 3]> {
        self.r().vec3().ok()
    }
    /// Rotator as (pitch, yaw, roll) in UE units (65536 = 360°).
    pub fn as_rot(&self) -> Option<[i32; 3]> {
        let mut r = self.r();
        Some([r.i32().ok()?, r.i32().ok()?, r.i32().ok()?])
    }
    /// Array of object references.
    pub fn as_obj_array(&self) -> Vec<i32> {
        let mut r = self.r();
        let n = r.i32().unwrap_or(0).max(0) as usize;
        if self.data.len() != 4 + n * 4 {
            return vec![];
        }
        (0..n).filter_map(|_| r.i32().ok()).collect()
    }
}

pub struct Props<'a> {
    pub list: Vec<Prop<'a>>,
    /// Offset (within the export data) right after the terminating "None".
    pub end: usize,
}

impl<'a> Props<'a> {
    pub fn get(&self, name: &str) -> Option<&Prop<'a>> {
        self.list.iter().find(|p| p.name == name)
    }
    pub fn get_idx(&self, name: &str, idx: i32) -> Option<&Prop<'a>> {
        self.list.iter().find(|p| p.name == name && p.array_index == idx)
    }
}

pub fn read_fname(pkg: &Package, r: &mut Reader) -> Result<String> {
    let i = r.i32()?;
    let n = r.i32()?;
    ensure!(i >= 0 && (i as usize) < pkg.names.len(), "bad name index {i} at {:#x}", r.o - 8);
    Ok(pkg.name(i, n))
}

/// Read a tagged property list starting at `r.o`.
pub fn read_props<'a>(pkg: &Package, r: &mut Reader<'a>) -> Result<Props<'a>> {
    let mut list = Vec::new();
    loop {
        let name = read_fname(pkg, r)?;
        if name == "None" {
            break;
        }
        let ty = read_fname(pkg, r)?;
        ensure!(ty.ends_with("Property"), "bad prop type {ty} for {name}");
        let size = r.i32()?;
        let array_index = r.i32()?;
        ensure!(size >= 0 && (size as usize) <= r.remaining(), "bad prop size {size} for {name}");
        let mut struct_name = String::new();
        let mut bool_value = false;
        match ty.as_str() {
            "StructProperty" => struct_name = read_fname(pkg, r)?,
            "BoolProperty" => bool_value = r.i32()? != 0,
            _ => {}
        }
        let data = r.bytes(size as usize)?;
        list.push(Prop { name, ty, struct_name, array_index, bool_value, data });
    }
    Ok(Props { list, end: r.o })
}

/// Classes whose exports are UComponent subclasses carry TemplateOwnerClass (+ TemplateName for templates)
/// before NetIndex; we detect the layout empirically in `object_props`.
pub const RF_HAS_STACK: u64 = 0x0200_0000_0000_0000;

pub fn object_props<'a>(pkg: &'a Package, idx: i32) -> Result<(Props<'a>, Reader<'a>)> {
    let data: &'a [u8] = pkg.export_data(idx);
    let flags = pkg.exports[idx as usize - 1].flags;
    let mut base = 0usize;
    if flags & RF_HAS_STACK != 0 {
        // FStateFrame: Node, StateNode, ProbeMask(u64), LatentAction(i32), StateStack(TArray), [Offset if Node]
        let mut r = Reader::new(data, 0);
        let node = r.i32()?;
        r.i32()?;
        r.u64()?;
        r.i32()?;
        let n = r.i32()?;
        ensure!(n == 0, "state stack not empty ({n})");
        if node != 0 {
            r.i32()?;
        }
        base = r.o;
    }
    // [NetIndex] | [TemplateOwnerClass, NetIndex] | [TemplateOwnerClass, TemplateName(FName), NetIndex]
    let mut last_err = None;
    for skip in [4usize, 8, 16] {
        let mut r = Reader::new(data, base + skip);
        match read_props(pkg, &mut r) {
            Ok(p) => return Ok((p, r)),
            Err(e) => last_err = Some(e),
        }
    }
    Err(last_err.unwrap())
}
