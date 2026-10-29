//! A small interpreter for straight-line Xenos vertex shader microcode, as the SDK's `ucode_disassembly()` prints it.
//! B-187 runs the game's own SpeedTree vertex shaders with it on the CPU, over the constants and vertex streams of a
//! draw the port issued, which gives the exact positions, UVs and baked-light indices the game drew (the geometry
//! SpeedTreeRT builds at load is not in the disc's packages).
//!
//! Supports what the SpeedTree shaders use: vfetch_full/mini, the usual vector ops and the co-issued scalar ops. No
//! control flow beyond exec/alloc (the tree shaders have none). Unknown operations are an error, never a guess.

use anyhow::{Result, anyhow, bail};
use std::collections::HashMap;

#[derive(Clone, Copy, Debug, PartialEq)]
enum Fmt {
    F3,
    F2,
    F4,
    F1,
    B8x4,
}

#[derive(Clone, Debug)]
enum SrcKind {
    Reg(usize),
    RegAbs(usize),
    Const(usize),
    /// `c[base+a0]`
    ConstRel(usize),
}

#[derive(Clone, Debug)]
struct Src {
    kind: SrcKind,
    neg: bool,
    /// Swizzle as given, padded by repeating the last component to four.
    swz: [usize; 4],
    /// Number of swizzle characters written (0 = none, meaning xyzw).
    given: usize,
}

#[derive(Clone, Debug)]
struct Dest {
    /// Register number, or an output slot (`out`) when `reg` is false.
    index: usize,
    reg: bool,
    mask: [u8; 4],
    mask_len: usize,
}

#[derive(Clone, Debug)]
struct Fetch {
    full: bool,
    dest: Dest,
    vtx: Option<Src>,
    stream: usize,
    offset_words: usize,
    fmt: Fmt,
    signed: bool,
    integer: bool,
}

#[derive(Clone, Debug)]
struct Alu {
    op: String,
    dest: Dest,
    srcs: Vec<Src>,
    paired: bool,
}

#[derive(Clone, Debug)]
enum Instr {
    Fetch(Fetch),
    Alu(Alu),
}

/// A parsed vertex program.
#[derive(Clone, Debug)]
pub struct Program {
    code: Vec<Instr>,
    outputs: Vec<String>,
}

/// What `run` gives back: the output registers by name (`oPos`, `o1`, ...).
pub struct Outputs<'a> {
    prog: &'a Program,
    values: Vec<[f64; 4]>,
}

impl Outputs<'_> {
    pub fn get(&self, name: &str) -> Option<[f64; 4]> {
        self.prog
            .outputs
            .iter()
            .position(|n| n == name)
            .map(|i| self.values[i])
    }
}

fn comp(c: char) -> Result<usize> {
    Ok(match c {
        'x' | 'r' => 0,
        'y' | 'g' => 1,
        'z' | 'b' => 2,
        'w' | 'a' => 3,
        _ => bail!("bad swizzle character {c:?}"),
    })
}

fn parse_src(s: &str) -> Result<Src> {
    let neg = s.starts_with('-');
    let s = s.trim_start_matches('-');
    let (name, swz) = s.split_once('.').unwrap_or((s, ""));
    let kind = if let Some(rest) = name.strip_prefix("r_abs[") {
        SrcKind::RegAbs(rest.trim_end_matches(']').parse()?)
    } else if let Some(rest) = name.strip_prefix("c[") {
        let base = rest.trim_end_matches(']').split('+').next().unwrap_or("0");
        SrcKind::ConstRel(base.parse()?)
    } else if let Some(rest) = name.strip_prefix('r') {
        SrcKind::Reg(rest.parse()?)
    } else if let Some(rest) = name.strip_prefix('c') {
        SrcKind::Const(rest.parse()?)
    } else {
        bail!("bad source {s:?}");
    };
    let chars: Vec<char> = swz.chars().collect();
    let mut out = [0usize, 1, 2, 3];
    if !chars.is_empty() {
        let last = comp(*chars.last().unwrap())?;
        for (i, o) in out.iter_mut().enumerate() {
            *o = chars.get(i).map(|&c| comp(c)).transpose()?.unwrap_or(last);
        }
    }
    Ok(Src {
        kind,
        neg,
        swz: out,
        given: chars.len(),
    })
}

fn parse_dest(s: &str, outputs: &mut Vec<String>) -> Result<Dest> {
    let (name, mask) = s.split_once('.').unwrap_or((s, ""));
    let mask = if mask.is_empty() { "xyzw" } else { mask };
    let mut m = [b'_'; 4];
    for (i, c) in mask.bytes().take(4).enumerate() {
        m[i] = c;
    }
    let mask_len = mask.len().min(4);
    if let Some(n) = name.strip_prefix('r').and_then(|n| n.parse::<usize>().ok()) {
        return Ok(Dest {
            index: n,
            reg: true,
            mask: m,
            mask_len,
        });
    }
    let index = match outputs.iter().position(|o| o == name) {
        Some(i) => i,
        None => {
            outputs.push(name.to_string());
            outputs.len() - 1
        }
    };
    Ok(Dest {
        index,
        reg: false,
        mask: m,
        mask_len,
    })
}

fn parse_fmt(s: &str) -> Result<Fmt> {
    Ok(match s {
        "FMT_32_32_32_FLOAT" => Fmt::F3,
        "FMT_32_32_FLOAT" => Fmt::F2,
        "FMT_32_32_32_32_FLOAT" => Fmt::F4,
        "FMT_32_FLOAT" => Fmt::F1,
        "FMT_8_8_8_8" => Fmt::B8x4,
        _ => bail!("unsupported vertex format {s}"),
    })
}

/// Splits `a, b, c` at top-level commas (the operands contain none inside brackets, but be safe).
fn split_args(rest: &str) -> Vec<String> {
    rest.split(',').map(|a| a.trim().to_string()).collect()
}

impl Program {
    /// Parses the vertex-shader part of a disassembly (instruction lines `/* n */ op ...` and co-issued `+ op ...`).
    pub fn parse(text: &str) -> Result<Program> {
        let mut code = Vec::new();
        let mut outputs = Vec::new();
        for raw in text.lines() {
            let s = raw.trim();
            if s.starts_with("--- pixel") {
                break;
            }
            let (body, paired) = if s.starts_with("/*") {
                match s.find("*/") {
                    Some(e)
                        if s[2..e]
                            .trim()
                            .chars()
                            .all(|c| c.is_ascii_digit() || c == '.') =>
                    {
                        (s[e + 2..].trim(), false)
                    }
                    _ => continue,
                }
            } else if let Some(r) = s.strip_prefix('+') {
                (r.trim(), true)
            } else {
                continue;
            };
            if body.is_empty() {
                continue;
            }
            let (op, rest) = body.split_once(char::is_whitespace).unwrap_or((body, ""));
            if matches!(op, "exec" | "exece" | "alloc" | "cnop" | "serialize")
                || op.starts_with("kill")
            {
                continue;
            }
            let args = split_args(rest);
            if op.starts_with("vfetch") {
                let full = op == "vfetch_full";
                let opt_from = if full { 3 } else { 1 };
                let mut opts: HashMap<&str, &str> = HashMap::new();
                for a in args.iter().skip(opt_from) {
                    let (k, v) = a.split_once('=').unwrap_or((a, ""));
                    opts.insert(k, v);
                }
                let dest = parse_dest(
                    args.first()
                        .ok_or_else(|| anyhow!("fetch without destination"))?,
                    &mut outputs,
                )?;
                let (vtx, stream) = if full {
                    let vf = args
                        .get(2)
                        .ok_or_else(|| anyhow!("vfetch_full without stream"))?;
                    (
                        Some(parse_src(&args[1])?),
                        vf.trim_start_matches("vf").parse::<usize>()?,
                    )
                } else {
                    (None, 0)
                };
                let fmt = parse_fmt(
                    opts.get("DataFormat")
                        .ok_or_else(|| anyhow!("fetch without DataFormat"))?,
                )?;
                code.push(Instr::Fetch(Fetch {
                    full,
                    dest,
                    vtx,
                    stream,
                    offset_words: opts.get("Offset").and_then(|v| v.parse().ok()).unwrap_or(0),
                    fmt,
                    signed: opts.contains_key("Signed"),
                    integer: opts.get("NumFormat") == Some(&"integer"),
                }));
            } else {
                let dest = parse_dest(
                    args.first()
                        .ok_or_else(|| anyhow!("{op} without destination"))?,
                    &mut outputs,
                )?;
                let srcs = args
                    .iter()
                    .skip(1)
                    .map(|a| parse_src(a))
                    .collect::<Result<Vec<_>>>()?;
                code.push(Instr::Alu(Alu {
                    op: op.to_string(),
                    dest,
                    srcs,
                    paired,
                }));
            }
        }
        if code.is_empty() {
            bail!("no instructions");
        }
        Ok(Program { code, outputs })
    }
}

/// Which stream and byte stride a fetch reads.
pub struct Stream<'a> {
    /// Guest-order (big-endian) bytes.
    pub data: &'a [u8],
    pub stride: usize,
}

pub struct Vm<'a> {
    prog: &'a Program,
    consts: &'a [f32],
    streams: &'a [Stream<'a>],
    r: Vec<[f64; 4]>,
    o: Vec<[f64; 4]>,
    a0: i64,
    ps: f64,
    last_vtx: usize,
    last_stream: usize,
    pending_a0: Option<i64>,
}

fn is_scalar_op(op: &str) -> bool {
    op.ends_with('s')
        || op.ends_with("sc")
        || matches!(
            op,
            "sin"
                | "cos"
                | "log"
                | "exp"
                | "rcp"
                | "rsq"
                | "maxas"
                | "adds_prev"
                | "muls_prev"
                | "sqrt"
        )
}

impl<'a> Vm<'a> {
    pub fn new(prog: &'a Program, consts: &'a [f32], streams: &'a [Stream<'a>]) -> Vm<'a> {
        Vm {
            prog,
            consts,
            streams,
            r: vec![[0.0; 4]; 64],
            o: vec![[0.0; 4]; prog.outputs.len()],
            a0: 0,
            ps: 0.0,
            last_vtx: 0,
            last_stream: 0,
            pending_a0: None,
        }
    }

    fn c(&self, i: i64) -> [f64; 4] {
        let i = i.clamp(0, 255) as usize;
        let c = &self.consts;
        if c.len() < (i + 1) * 4 {
            return [0.0; 4];
        }
        [
            c[i * 4] as f64,
            c[i * 4 + 1] as f64,
            c[i * 4 + 2] as f64,
            c[i * 4 + 3] as f64,
        ]
    }

    fn src(&self, s: &Src) -> [f64; 4] {
        let v = match s.kind {
            SrcKind::Reg(i) => self.r[i],
            SrcKind::RegAbs(i) => self.r[i].map(f64::abs),
            SrcKind::Const(i) => self.c(i as i64),
            SrcKind::ConstRel(b) => self.c(b as i64 + self.a0),
        };
        let mut o = [v[s.swz[0]], v[s.swz[1]], v[s.swz[2]], v[s.swz[3]]];
        if s.neg {
            o = o.map(|x| -x);
        }
        o
    }

    fn write(&mut self, d: &Dest, val: [f64; 4], fetch: bool) {
        let reg = if d.reg {
            &mut self.r[d.index]
        } else {
            &mut self.o[d.index]
        };
        for i in 0..d.mask_len {
            match d.mask[i] {
                b'_' => {}
                b'0' => reg[i] = 0.0,
                b'1' => reg[i] = 1.0,
                c => {
                    reg[i] = if fetch {
                        match comp(c as char) {
                            Ok(k) => val[k],
                            Err(_) => 0.0,
                        }
                    } else {
                        val[i]
                    }
                }
            }
        }
    }

    fn fetch(&self, f: &Fetch, vertex: usize, stream: usize) -> Result<[f64; 4]> {
        let s = self
            .streams
            .get(stream)
            .ok_or_else(|| anyhow!("fetch from missing stream {stream}"))?;
        let base = vertex * s.stride + f.offset_words * 4;
        let bytes = |n: usize| -> Result<&[u8]> {
            s.data
                .get(base..base + n)
                .ok_or_else(|| anyhow!("fetch past the end of stream {stream} (vertex {vertex})"))
        };
        let float = |n: usize| -> Result<[f64; 4]> {
            let b = bytes(4 * n)?;
            let mut v = [0.0, 0.0, 0.0, 1.0];
            for (i, x) in v.iter_mut().enumerate().take(n) {
                *x =
                    f32::from_be_bytes([b[4 * i], b[4 * i + 1], b[4 * i + 2], b[4 * i + 3]]) as f64;
            }
            Ok(v)
        };
        match f.fmt {
            Fmt::F1 => float(1),
            Fmt::F2 => float(2),
            Fmt::F3 => float(3),
            Fmt::F4 => float(4),
            Fmt::B8x4 => {
                let b = bytes(4)?;
                let w = u32::from_be_bytes([b[0], b[1], b[2], b[3]]);
                let mut v = [0.0; 4];
                for (i, x) in v.iter_mut().enumerate() {
                    let byte = (w >> (8 * i)) & 0xFF;
                    let sb = if byte > 127 {
                        byte as i32 - 256
                    } else {
                        byte as i32
                    };
                    *x = if f.integer {
                        if f.signed { sb as f64 } else { byte as f64 }
                    } else if f.signed {
                        sb as f64 / 127.0
                    } else {
                        byte as f64 / 255.0
                    };
                }
                Ok(v)
            }
        }
    }

    fn vector(&self, op: &str, a: &[[f64; 4]]) -> Result<[f64; 4]> {
        let m = |f: &dyn Fn(f64, f64) -> f64| {
            [
                f(a[0][0], a[1][0]),
                f(a[0][1], a[1][1]),
                f(a[0][2], a[1][2]),
                f(a[0][3], a[1][3]),
            ]
        };
        let dot = |n: usize| -> f64 { (0..n).map(|i| a[0][i] * a[1][i]).sum() };
        let need = |n: usize| -> Result<()> {
            if a.len() < n {
                bail!("{op}: {} operands, need {n}", a.len())
            } else {
                Ok(())
            }
        };
        let flag = |b: bool| if b { 1.0 } else { 0.0 };
        Ok(match op {
            "mul" => {
                need(2)?;
                m(&|x, y| x * y)
            }
            "add" => {
                need(2)?;
                m(&|x, y| x + y)
            }
            "mad" => {
                need(3)?;
                [0, 1, 2, 3].map(|i| a[0][i] * a[1][i] + a[2][i])
            }
            "max" => {
                need(2)?;
                m(&f64::max)
            }
            "min" => {
                need(2)?;
                m(&f64::min)
            }
            "dp4" => {
                need(2)?;
                [dot(4); 4]
            }
            "dp3" => {
                need(2)?;
                [dot(3); 4]
            }
            "dp2add" => {
                need(3)?;
                [a[0][0] * a[1][0] + a[0][1] * a[1][1] + a[2][0]; 4]
            }
            "seq" => {
                need(2)?;
                m(&|x, y| flag(x == y))
            }
            "sne" => {
                need(2)?;
                m(&|x, y| flag(x != y))
            }
            "sge" => {
                need(2)?;
                m(&|x, y| flag(x >= y))
            }
            "sgt" => {
                need(2)?;
                m(&|x, y| flag(x > y))
            }
            "cndeq" => {
                need(3)?;
                [0, 1, 2, 3].map(|i| if a[0][i] == 0.0 { a[1][i] } else { a[2][i] })
            }
            "cndge" => {
                need(3)?;
                [0, 1, 2, 3].map(|i| if a[0][i] >= 0.0 { a[1][i] } else { a[2][i] })
            }
            "cndgt" => {
                need(3)?;
                [0, 1, 2, 3].map(|i| if a[0][i] > 0.0 { a[1][i] } else { a[2][i] })
            }
            "frc" => {
                need(1)?;
                a[0].map(|x| x - x.floor())
            }
            "trunc" => {
                need(1)?;
                a[0].map(f64::trunc)
            }
            "floor" => {
                need(1)?;
                a[0].map(f64::floor)
            }
            _ => bail!("unsupported vector operation {op}"),
        })
    }

    fn scalar(&mut self, a: &Alu) -> Result<f64> {
        let op = a.op.as_str();
        let (x, y) = if op.ends_with("sc") && a.srcs.len() >= 2 {
            (self.src(&a.srcs[0])[0], self.src(&a.srcs[1])[0])
        } else {
            let s = a.srcs.first().ok_or_else(|| anyhow!("{op}: no operand"))?;
            let v = self.src(s);
            (v[0], if s.given > 1 { v[1] } else { v[0] })
        };
        let base = if let Some(b) = op.strip_suffix("sc") {
            b
        } else if op.ends_with('s') {
            &op[..op.len() - 1]
        } else {
            op
        };
        let ps = self.ps;
        let flag = |b: bool| if b { 1.0 } else { 0.0 };
        Ok(match op {
            "adds_prev" => x + ps,
            "muls_prev" => x * ps,
            "maxas" => {
                self.a0_next(x);
                x.max(y)
            }
            "sin" => x.sin(),
            "cos" => x.cos(),
            "log" => {
                if x > 0.0 {
                    x.log2()
                } else {
                    f64::NEG_INFINITY
                }
            }
            "exp" => 2f64.powf(x),
            "rcp" => {
                if x != 0.0 {
                    1.0 / x
                } else {
                    f64::INFINITY
                }
            }
            "rsq" => {
                if x > 0.0 {
                    1.0 / x.sqrt()
                } else {
                    f64::INFINITY
                }
            }
            "seqs" => flag(x == 0.0),
            "sges" => flag(x >= 0.0),
            "sgts" => flag(x > 0.0),
            "sqrt" => x.sqrt(),
            _ => match base {
                "add" => x + y,
                "mul" => x * y,
                "max" => x.max(y),
                "min" => x.min(y),
                "trunc" => x.trunc(),
                "floor" => x.floor(),
                "frc" => x - x.floor(),
                _ => bail!("unsupported scalar operation {op}"),
            },
        })
    }

    fn a0_next(&mut self, x: f64) {
        self.pending_a0 = Some((x + 0.5).floor() as i64);
    }
}

impl Vm<'_> {
    /// Runs the program for one vertex index and returns the output registers.
    pub fn run(&mut self, vertex: usize) -> Result<Outputs<'_>> {
        for r in &mut self.r {
            *r = [0.0; 4];
        }
        for o in &mut self.o {
            *o = [0.0; 4];
        }
        self.r[0][0] = vertex as f64;
        self.a0 = 0;
        self.ps = 0.0;
        let prog = self.prog;
        let mut i = 0;
        while i < prog.code.len() {
            match &prog.code[i] {
                Instr::Fetch(f) => {
                    let (stream, vtx) = if f.full {
                        let v = self.src(
                            f.vtx
                                .as_ref()
                                .ok_or_else(|| anyhow!("vfetch_full without index"))?,
                        )[0] as usize;
                        self.last_vtx = v;
                        self.last_stream = f.stream;
                        (f.stream, v)
                    } else {
                        (self.last_stream, self.last_vtx)
                    };
                    let v = self.fetch(f, vtx, stream)?;
                    self.write(&f.dest, v, true);
                    i += 1;
                }
                Instr::Alu(a) => {
                    let base_op = a.op.trim_end_matches("_sat");
                    let sat = base_op.len() != a.op.len();
                    let scalar_only = {
                        let probe = Alu {
                            op: base_op.to_string(),
                            ..a.clone()
                        };
                        is_scalar_op(&probe.op)
                    };
                    let (vec, sca): (Option<&Alu>, Option<&Alu>) = if scalar_only {
                        i += 1;
                        (None, Some(a))
                    } else {
                        let next = match prog.code.get(i + 1) {
                            Some(Instr::Alu(n)) if n.paired => Some(n),
                            _ => None,
                        };
                        i += if next.is_some() { 2 } else { 1 };
                        (Some(a), next)
                    };
                    self.pending_a0 = None;
                    let vres = match vec {
                        Some(v) => {
                            let args: Vec<[f64; 4]> = v.srcs.iter().map(|s| self.src(s)).collect();
                            let mut r = self.vector(v.op.trim_end_matches("_sat"), &args)?;
                            if v.op.ends_with("_sat") {
                                r = r.map(|x| x.clamp(0.0, 1.0));
                            }
                            Some(r)
                        }
                        None => None,
                    };
                    let sres = match sca {
                        Some(s) => {
                            let mut probe = s.clone();
                            probe.op = probe.op.trim_end_matches("_sat").to_string();
                            let mut r = self.scalar(&probe)?;
                            if s.op.ends_with("_sat") {
                                r = r.clamp(0.0, 1.0);
                            }
                            Some(r)
                        }
                        None => None,
                    };
                    let _ = sat;
                    if let (Some(v), Some(r)) = (vec, vres) {
                        self.write(&v.dest, r, false);
                    }
                    if let (Some(s), Some(r)) = (sca, sres) {
                        self.ps = r;
                        self.write(&s.dest, [r; 4], false);
                    }
                    if let Some(a0) = self.pending_a0.take() {
                        self.a0 = a0;
                    }
                }
            }
        }
        Ok(Outputs {
            prog,
            values: self.o.clone(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mul_add_and_swizzle() {
        let text = "/*    0.0 */       exec\n\
                    /*   0    */          mul r0, c0.yxzw, c1\n\
                    /*   1    */          add r1.xy__, r0, c2.zzzz\n\
                    /*    1.0 */       alloc position\n\
                    /*    1.1 */       exec\n\
                    /*   2    */          max oPos, r1.xyxy, r1.xyxy\n";
        let prog = Program::parse(text).unwrap();
        let mut consts = vec![0f32; 1024];
        consts[0..4].copy_from_slice(&[1.0, 2.0, 3.0, 4.0]);
        consts[4..8].copy_from_slice(&[10.0, 10.0, 10.0, 10.0]);
        consts[8..12].copy_from_slice(&[0.5, 0.5, 0.5, 0.5]);
        let mut vm = Vm::new(&prog, &consts, &[]);
        let o = vm.run(0).unwrap();
        let p = o.get("oPos").unwrap();
        // r0 = (2, 1, 3, 4) * 10; r1.xy = r0.xy + 0.5
        assert_eq!([p[0], p[1], p[2], p[3]], [20.5, 10.5, 20.5, 10.5]);
    }
}
