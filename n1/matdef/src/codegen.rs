//! Material IR -> WGSL fragment code (`fn eval_material`), plain Rust and independent of wgpu: the same source is
//! compiled by naga to SPIR-V for the in-port ash renderer. Constants (and texture unpack scale/bias) are hoisted into
//! a per-material parameter array `mp.c[]`, so materials with the same graph structure share one shader.

use crate::{MatDef, Node};
use std::fmt::Write;

#[derive(Clone)]
pub struct Compiled {
    /// WGSL text of `fn eval_material(fi: FragIn) -> MatOut` plus the texture declarations (`t0..`).
    pub body: String,
    /// Parameter vec4s in order (`mp.c[i]`); padded by the renderer to `nparams`.
    pub params: Vec<[f32; 4]>,
    pub ntex: usize,
    /// Structure key: equal keys share one shader module.
    pub key: String,
    pub has_normal: bool,
    /// Needs the tangent frame / view vector (any of Normal, BumpOffset, Fresnel, Reflection)
    pub unsupported: Vec<String>,
}

/// `unpack[i]` = Some((min, max)) when texture `i` needs `tex * (max - min) + min` applied (normal maps).
pub fn compile(def: &MatDef, unpack: &[Option<([f32; 3], [f32; 3])>]) -> Compiled {
    compile_ex(def, unpack, &[])
}

/// As `compile`; `recon_z[i]`: texture `i` is a two-channel (BC5/DXN) normal map whose Z is rebuilt after unpacking,
/// `sqrt(1 - x^2 - y^2)`, as UE3's material compiler does for those formats.
pub fn compile_ex(def: &MatDef, unpack: &[Option<([f32; 3], [f32; 3])>], recon_z: &[bool]) -> Compiled {
    compile_gamma(def, unpack, recon_z, &[])
}

/// As `compile_ex`; `gamma[i]`: texture `i` is fetched by the original with Xenos's gamma sign and uploaded raw
/// (UNORM), so its colour goes through the hardware's piecewise-linear curve after filtering (`xe_pwl` in the scene
/// template), as the SDK's shader translator does (B-235).
pub fn compile_gamma(
    def: &MatDef,
    unpack: &[Option<([f32; 3], [f32; 3])>],
    recon_z: &[bool],
    gamma: &[bool],
) -> Compiled {
    let mut params: Vec<[f32; 4]> = Vec::new();
    // One slot per use, never shared by value: two materials with the same graph but constants that happen to
    // coincide would otherwise index differently and need different shaders. The key stays purely structural.
    let mut p = |v: [f32; 4]| -> usize {
        params.push(v);
        params.len() - 1
    };
    let n = def.nodes.len();
    let mut nc = vec![4u8; n]; // component count tracking
    let mut code = String::new();
    // Which nodes are actually reachable from an output? Emit all (graphs are tiny) but skip dead ones.
    let mut live = vec![false; n];
    let outs: Vec<u32> = [def.diffuse, def.specular, def.spec_power, def.normal, def.emissive, def.opacity, def.opacity_mask].into_iter().flatten().collect();
    let mut stack = outs.clone();
    while let Some(i) = stack.pop() {
        if live[i as usize] {
            continue;
        }
        live[i as usize] = true;
        stack.extend(children(&def.nodes[i as usize]));
    }
    // B-261: UE3's ReflectionVector is the tangent-space reflection of the view about the material's Normal output, so
    // the Normal subtree is emitted first when a reflection vector is live (the nodes' index order does not guarantee it).
    let has_refl = (0..n).any(|i| live[i] && matches!(def.nodes[i], Node::ReflVec));
    let mut order: Vec<usize> = Vec::with_capacity(n);
    let mut done = vec![false; n];
    if has_refl {
        if let Some(root) = def.normal {
            let mut st: Vec<(usize, bool)> = vec![(root as usize, false)];
            while let Some((i, expanded)) = st.pop() {
                if done[i] {
                    continue;
                }
                if expanded {
                    done[i] = true;
                    order.push(i);
                } else {
                    st.push((i, true));
                    for c in children(&def.nodes[i]) {
                        if !done[c as usize] {
                            st.push((c as usize, false));
                        }
                    }
                }
            }
        }
    }
    order.extend((0..n).filter(|&i| !done[i]));
    for i in order {
        if !live[i] {
            continue;
        }
        let v = |j: u32| format!("n{j}");
        let uvs = |o: &Option<u32>| match o {
            Some(j) => format!("n{j}.xy"),
            None => "fi.uv0".to_string(),
        };
        let (expr, c): (String, u8) = match &def.nodes[i] {
            Node::Const { v: c, n: w } => {
                let k = p(*c);
                let cnt = if *w > 0 {
                    *w
                } else if c[0] == c[1] && c[1] == c[2] && c[2] == c[3] {
                    1
                } else if c[2] == 0.0 && c[3] == 0.0 {
                    2
                } else {
                    3
                };
                (format!("mp.c[{k}]"), cnt)
            }
            Node::Tex { tex, uv } => {
                let t = *tex as usize;
                let mut e = format!("textureSample(t{t}, smp, {})", uvs(uv));
                if gamma.get(t).copied().unwrap_or(false) {
                    e = format!("xe_pwl({e})");
                }
                if let Some(Some((mn, mx))) = unpack.get(t) {
                    let s = p([mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], 1.0]);
                    let b = p([mn[0], mn[1], mn[2], 0.0]);
                    e = format!("({e} * mp.c[{s}] + mp.c[{b}])");
                }
                if recon_z.get(t).copied().unwrap_or(false) {
                    e = format!("recon_z({e})");
                }
                (e, 4)
            }
            Node::TexCoord { set, u, v: vv } => {
                let k = p([*u, *vv, 0.0, 0.0]);
                (format!("vec4<f32>(fi.uv{} * mp.c[{k}].xy, 0.0, 0.0)", (*set).min(1)), 2)
            }
            Node::VColor => ("fi.vcol".into(), 4),
            Node::Add { a, b } => (format!("({} + {})", v(*a), v(*b)), nc[*a as usize].max(nc[*b as usize])),
            Node::Sub { a, b } => (format!("({} - {})", v(*a), v(*b)), nc[*a as usize].max(nc[*b as usize])),
            Node::Mul { a, b } => (format!("({} * {})", v(*a), v(*b)), nc[*a as usize].max(nc[*b as usize])),
            Node::Div { a, b } => (format!("({} / (abs({}) + vec4<f32>(1e-6)))", v(*a), v(*b)), nc[*a as usize].max(nc[*b as usize])),
            Node::Pow { a, b } => (format!("pow(max({}, vec4<f32>(1e-6)), {})", v(*a), v(*b)), nc[*a as usize]),
            Node::Dot { a, b } => {
                let (na, nb) = (nc[*a as usize], nc[*b as usize]);
                let m = na.max(nb);
                let e = match m {
                    1 => format!("vec4<f32>({}.x * {}.x)", v(*a), v(*b)),
                    2 => format!("vec4<f32>(dot({}.xy, {}.xy))", v(*a), v(*b)),
                    _ => format!("vec4<f32>(dot({}.xyz, {}.xyz))", v(*a), v(*b)),
                };
                (e, 1)
            }
            Node::Lerp { a, b, t } => (format!("mix({}, {}, {})", v(*a), v(*b), v(*t)), nc[*a as usize].max(nc[*b as usize])),
            Node::Clamp { x, lo, hi } => (format!("clamp({}, {}, {})", v(*x), v(*lo), v(*hi)), nc[*x as usize]),
            Node::Sat { x, lo, hi } => {
                let k = p([*lo, *hi, 0.0, 0.0]);
                (format!("clamp({}, vec4<f32>(mp.c[{k}].x), vec4<f32>(mp.c[{k}].y))", v(*x)), nc[*x as usize])
            }
            Node::Un { f, x } => {
                let s = v(*x);
                let e = match f.as_str() {
                    "oneminus" => format!("(vec4<f32>(1.0) - {s})"),
                    "abs" => format!("abs({s})"),
                    "frac" => format!("fract({s})"),
                    "floor" => format!("floor({s})"),
                    "ceil" => format!("ceil({s})"),
                    "sqrt" => format!("sqrt(max({s}, vec4<f32>(0.0)))"),
                    "normalize" => format!("vec4<f32>(normalize({s}.xyz), 0.0)"),
                    _ => format!("({s} * 0.5 + vec4<f32>(0.5))"),
                };
                (e, nc[*x as usize])
            }
            Node::Trig { cos, x, period } => {
                let k = p([std::f32::consts::TAU / period.max(1e-6), 0.0, 0.0, 0.0]);
                (format!("{}({} * mp.c[{k}].x)", if *cos { "cos" } else { "sin" }, v(*x)), nc[*x as usize])
            }
            Node::Mask { x, m } => {
                let comps: Vec<char> = ['x', 'y', 'z', 'w'].iter().zip(m).filter(|(_, on)| **on).map(|(c, _)| *c).collect();
                let s = v(*x);
                let e = match comps.len() {
                    0 => s,
                    1 => format!("vec4<f32>({s}.{})", comps[0]),
                    2 => format!("vec4<f32>({s}.{}, {s}.{}, 0.0, 0.0)", comps[0], comps[1]),
                    3 => format!("vec4<f32>({s}.{}, {s}.{}, {s}.{}, 0.0)", comps[0], comps[1], comps[2]),
                    _ => s,
                };
                (e, comps.len().max(1) as u8)
            }
            Node::Append { a, b } => {
                let (na, nb) = (nc[*a as usize] as usize, nc[*b as usize] as usize);
                let mut cs: Vec<String> = Vec::new();
                for (k, ch) in ['x', 'y', 'z', 'w'].iter().enumerate().take(na.min(4)) {
                    let _ = k;
                    cs.push(format!("{}.{ch}", v(*a)));
                }
                for ch in ['x', 'y', 'z', 'w'].iter().take(nb.min(4)) {
                    cs.push(format!("{}.{ch}", v(*b)));
                }
                cs.resize(4, "0.0".into());
                (format!("vec4<f32>({}, {}, {}, {})", cs[0], cs[1], cs[2], cs[3]), (na + nb).min(4) as u8)
            }
            Node::Desat { x, pct, w } => {
                let k = p([w[0], w[1], w[2], 0.0]);
                let pc = pct.map(|j| format!("n{j}.x")).unwrap_or("1.0".into());
                (format!("vec4<f32>(mix({0}.xyz, vec3<f32>(dot({0}.xyz, mp.c[{k}].xyz)), {pc}), {0}.w)", v(*x)), 3)
            }
            Node::Time => ("vec4<f32>(g.cam_pos.w)".into(), 1),
            Node::Panner { coord, time, sx, sy } => {
                let k = p([*sx, *sy, 0.0, 0.0]);
                let t = time.map(|j| format!("n{j}.x")).unwrap_or("g.cam_pos.w".into());
                (format!("vec4<f32>({} + {t} * mp.c[{k}].xy, 0.0, 0.0)", uvs(coord)), 2)
            }
            Node::Rotator { coord, time, cx, cy, speed } => {
                let k = p([*cx, *cy, *speed, 0.0]);
                let t = time.map(|j| format!("n{j}.x")).unwrap_or("g.cam_pos.w".into());
                (format!("vec4<f32>(rotate_uv({}, mp.c[{k}].xy, {t} * mp.c[{k}].z), 0.0, 0.0)", uvs(coord)), 2)
            }
            Node::BumpOffset { coord, height, ratio, plane } => {
                let k = p([*ratio, *plane, 0.0, 0.0]);
                (format!("vec4<f32>({}.xy + ({}.x - mp.c[{k}].y) * mp.c[{k}].x * fi.view_ts.xy, 0.0, 0.0)", v(*coord), v(*height)), 2)
            }
            Node::CameraVec => ("vec4<f32>(fi.view, 0.0)".into(), 3),
            Node::LightVec => ("vec4<f32>(fi.light_ts, 0.0)".into(), 3),
            Node::ReflVec => {
                let nts = def.normal.map(|j| format!("normalize(n{j}.xyz)")).unwrap_or("vec3<f32>(0.0, 0.0, 1.0)".into());
                (
                    format!("vec4<f32>(select(reflect(-fi.view, fi.nrm), reflect(-fi.view_ts, {nts}), fi.refl_ts), 0.0)"),
                    3,
                )
            }
            Node::Fresnel { exp, n: nn } => {
                let k = p([*exp, 0.0, 0.0, 0.0]);
                // B-277: UE3's Normal input is tangent space, so it pairs with the tangent-space view vector (the
                // way `ReflVec` pairs it with `view_ts`); the world normal pairs with the world view. Dotting a
                // tangent-space normal with the world view gave a near-constant term (a launcher drawn flat white).
                let dot = match nn {
                    Some(j) => format!("select(dot(normalize(n{j}.xyz), fi.view), dot(normalize(n{j}.xyz), fi.view_ts), fi.fresnel_ts)"),
                    None => "dot(fi.nrm, fi.view)".to_string(),
                };
                (format!("vec4<f32>(pow(1.0 - max({dot}, 0.0), mp.c[{k}].x))"), 1)
            }
            Node::WorldPos => ("vec4<f32>(fi.wpos, 1.0)".into(), 3),
            Node::PixelDepth => ("vec4<f32>(fi.depth)".into(), 1),
            Node::If { a, b, gt, eq, lt } => (
                format!("select(select({}, {}, {}.x > {}.x), {}, {}.x == {}.x)", v(*lt), v(*gt), v(*a), v(*b), v(*eq), v(*a), v(*b)),
                nc[*gt as usize].max(nc[*lt as usize]),
            ),
            Node::Unsup { .. } => ("vec4<f32>(0.0)".into(), 1),
        };
        nc[i] = c;
        let _ = writeln!(code, "    let n{i}: vec4<f32> = {expr};");
    }
    let mut body = String::new();
    let ntex = def.textures.len();
    for t in 0..ntex {
        let _ = writeln!(body, "@group(1) @binding({}) var t{t}: texture_2d<f32>;", 2 + t);
    }
    body.push_str("fn eval_material(fi: FragIn) -> MatOut {\n    var o: MatOut;\n");
    body.push_str(&code);
    let rgb = |n: Option<u32>, d: &str| n.map(|j| format!("n{j}.xyz")).unwrap_or(d.to_string());
    let sc = |n: Option<u32>, d: &str| n.map(|j| format!("n{j}.x")).unwrap_or(d.to_string());
    // Unconnected inputs' constants go through the parameter block too (a literal here would split the key).
    let dc = if def.diffuse.is_some() {
        String::new()
    } else {
        let c = def.diffuse_const.unwrap_or([0.5, 0.5, 0.5, 1.0]);
        format!("mp.c[{}].xyz", p([c[0], c[1], c[2], 0.0]))
    };
    let _ = writeln!(body, "    o.diffuse = {};", rgb(def.diffuse, &dc));
    let _ = writeln!(body, "    o.specular = {};", rgb(def.specular, "vec3<f32>(0.0)"));
    let spc = if def.spec_power.is_some() { String::new() } else { format!("mp.c[{}].x", p([def.spec_power_const.unwrap_or(15.0), 0.0, 0.0, 0.0])) };
    let _ = writeln!(body, "    o.spec_power = {};", sc(def.spec_power, &spc));
    let _ = writeln!(body, "    o.has_normal = {};", def.normal.is_some());
    let _ = writeln!(body, "    o.normal = {};", rgb(def.normal, "vec3<f32>(0.0, 0.0, 1.0)"));
    let dbg: Vec<String> = std::env::var("BOTS_N1_MATDBG").ok().map(|v| v.split(',').map(|x| x.trim().to_string()).collect()).unwrap_or_default();
    let dbg_ok = dbg.len() == 3
        && dbg.iter().all(|e| {
            e == "uvx" || e == "uvy" || e == "zero" || e.split('.').next().and_then(|k| k.parse::<usize>().ok()).is_some_and(|k| k < n && live[k])
        });
    if dbg_ok {
        let comp: Vec<String> = dbg
            .iter()
            .map(|e| match e.as_str() {
                "uvx" => "fi.uv0.x".to_string(),
                "uvy" => "fi.uv0.y".to_string(),
                "zero" => "0.0".to_string(),
                _ => {
                    let mut it = e.split('.');
                    let k = it.next().unwrap();
                    format!("n{k}.{}", it.next().unwrap_or("x"))
                }
            })
            .collect();
        let _ = writeln!(body, "    o.emissive = vec3<f32>({}, {}, {});", comp[0], comp[1], comp[2]);
    } else {
        let _ = writeln!(body, "    o.emissive = {};", rgb(def.emissive, "vec3<f32>(0.0)"));
    }
    let opc = if def.opacity.is_some() { String::new() } else { format!("mp.c[{}].x", p([def.opacity_const.unwrap_or(1.0), 0.0, 0.0, 0.0])) };
    let _ = writeln!(body, "    o.opacity = {};", sc(def.opacity, &opc));
    let _ = writeln!(body, "    o.mask = {};", sc(def.opacity_mask, "1.0"));
    body.push_str("    return o;\n}\n");
    let key = format!("{}#{}#{}", ntex, params.len().max(1), body);
    Compiled { body, params, ntex, key, has_normal: def.normal.is_some(), unsupported: def.unsupported.clone() }
}

fn children(n: &Node) -> Vec<u32> {
    match n {
        Node::Tex { uv, .. } => uv.iter().copied().collect(),
        Node::Add { a, b } | Node::Sub { a, b } | Node::Mul { a, b } | Node::Div { a, b } | Node::Pow { a, b } | Node::Dot { a, b } | Node::Append { a, b } => vec![*a, *b],
        Node::Lerp { a, b, t } => vec![*a, *b, *t],
        Node::Clamp { x, lo, hi } => vec![*x, *lo, *hi],
        Node::Sat { x, .. } | Node::Un { x, .. } | Node::Trig { x, .. } | Node::Mask { x, .. } => vec![*x],
        Node::Desat { x, pct, .. } => [Some(*x), *pct].into_iter().flatten().collect(),
        Node::Panner { coord, time, .. } | Node::Rotator { coord, time, .. } => [*coord, *time].into_iter().flatten().collect(),
        Node::BumpOffset { coord, height, .. } => vec![*coord, *height],
        Node::Fresnel { n, .. } => n.iter().copied().collect(),
        Node::If { a, b, gt, eq, lt } => vec![*a, *b, *gt, *eq, *lt],
        _ => vec![],
    }
}

#[cfg(test)]
mod refl_tests {
    use super::*;

    /// B-261: a ReflectionVector that sits before the Normal output in node order still sees the Normal's value, and
    /// the tangent-space form is behind a runtime flag.
    #[test]
    fn reflection_vector_uses_the_normal_output_even_when_it_comes_later() {
        let def = MatDef {
            nodes: vec![
                Node::ReflVec,                       // 0: used by the diffuse
                Node::Const { v: [0.0, 0.0, 1.0, 0.0], n: 3 }, // 1: the Normal output
            ],
            diffuse: Some(0),
            normal: Some(1),
            ..Default::default()
        };
        let c = compile(&def, &[]);
        let at = |s: &str| c.body.find(s).unwrap_or_else(|| panic!("missing {s} in {}", c.body));
        assert!(at("let n1:") < at("let n0:"), "normal first:\n{}", c.body);
        assert!(c.body.contains("reflect(-fi.view_ts, normalize(n1.xyz))"));
        assert!(c.body.contains("fi.refl_ts"));
    }

    /// B-277: a Fresnel with a Normal input pairs it with the tangent-space view behind a flag; without one it keeps
    /// the world normal and the world view.
    #[test]
    fn fresnel_with_a_normal_input_uses_the_tangent_space_view() {
        let mut def = MatDef {
            nodes: vec![Node::Const { v: [0.0, 0.0, 1.0, 0.0], n: 3 }, Node::Fresnel { exp: 6.0, n: Some(0) }],
            diffuse: Some(1),
            ..Default::default()
        };
        let c = compile(&def, &[]);
        assert!(c.body.contains("dot(normalize(n0.xyz), fi.view_ts), fi.fresnel_ts"), "{}", c.body);
        def.nodes[1] = Node::Fresnel { exp: 6.0, n: None };
        let c = compile(&def, &[]);
        assert!(c.body.contains("dot(fi.nrm, fi.view)") && !c.body.contains("view_ts"), "{}", c.body);
    }
}
