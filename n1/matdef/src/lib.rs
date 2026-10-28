//! Material IR types shared by the extractor (ue3) and the renderer (n8gfx). See `ue3::matir` for how they are built.

use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(tag = "op")]
pub enum Node {
    /// `n`: component count when fixed (a vector parameter: its value can change at run time, and a count inferred
    /// from the value would change the shader with it); 0 infers it from `v`.
    Const {
        v: [f32; 4],
        #[serde(default)]
        n: u8,
    },
    /// Sample texture `tex` (index into `MatDef::textures`) at `uv` (node) or the mesh UV0.
    Tex { tex: u32, uv: Option<u32> },
    TexCoord { set: u32, u: f32, v: f32 },
    VColor,
    Add { a: u32, b: u32 },
    Sub { a: u32, b: u32 },
    Mul { a: u32, b: u32 },
    Div { a: u32, b: u32 },
    Pow { a: u32, b: u32 },
    Dot { a: u32, b: u32 },
    Lerp { a: u32, b: u32, t: u32 },
    Clamp { x: u32, lo: u32, hi: u32 },
    Sat { x: u32, lo: f32, hi: f32 },
    /// Unary: one of oneminus, sin, cos, frac, floor, ceil, abs, normalize, half (x*0.5+0.5), sqrt
    Un { f: String, x: u32 },
    /// sin/cos with a period (x * 2pi / period)
    Trig { cos: bool, x: u32, period: f32 },
    Mask { x: u32, m: [bool; 4] },
    Append { a: u32, b: u32 },
    Desat { x: u32, pct: Option<u32>, w: [f32; 3] },
    Time,
    Panner { coord: Option<u32>, time: Option<u32>, sx: f32, sy: f32 },
    Rotator { coord: Option<u32>, time: Option<u32>, cx: f32, cy: f32, speed: f32 },
    BumpOffset { coord: u32, height: u32, ratio: f32, plane: f32 },
    CameraVec,
    /// B-189: UE3 `LightVector`, the key light's direction in tangent space (the sun, or the light environment's
    /// directional).
    LightVec,
    ReflVec,
    Fresnel { exp: f32, n: Option<u32> },
    WorldPos,
    PixelDepth,
    If { a: u32, b: u32, gt: u32, eq: u32, lt: u32 },
    /// Unsupported expression, evaluated as a constant (named for the coverage report).
    Unsup { class: String },
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct TexRef {
    pub path: String,
    /// Texture holds a tangent-space normal map.
    pub normal: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct MatDef {
    pub path: String,
    pub class: String,
    pub blend: String,
    pub lighting: String,
    pub two_sided: bool,
    pub clip: f32,
    pub nodes: Vec<Node>,
    pub textures: Vec<TexRef>,
    pub diffuse: Option<u32>,
    pub specular: Option<u32>,
    pub spec_power: Option<u32>,
    pub normal: Option<u32>,
    pub emissive: Option<u32>,
    pub opacity: Option<u32>,
    pub opacity_mask: Option<u32>,
    /// Constants for unconnected inputs.
    pub diffuse_const: Option<[f32; 4]>,
    pub spec_power_const: Option<f32>,
    pub opacity_const: Option<f32>,
    /// Classes this material uses that the IR can't express.
    pub unsupported: Vec<String>,
    /// Uses vertex colour anywhere (mesh colour stream needed).
    pub uses_vcolor: bool,
    /// Uses the second UV set (TexCoord set >= 1).
    pub uses_uv1: bool,
}


pub mod codegen;
pub mod shader;

#[cfg(test)]
mod tests {
    use super::*;

    fn def(nodes: Vec<Node>, diffuse: u32) -> MatDef {
        MatDef { nodes, diffuse: Some(diffuse), ..Default::default() }
    }

    #[test]
    fn const_without_n_reads_as_inferred() {
        let n: Node = serde_json::from_str(r#"{"op":"Const","v":[1.0,2.0,3.0,1.0]}"#).unwrap();
        assert_eq!(n, Node::Const { v: [1.0, 2.0, 3.0, 1.0], n: 0 });
    }

    /// B-203: a run-time vector parameter's value must not change the shader.
    #[test]
    fn fixed_width_const_key_ignores_value() {
        let k = |v: [f32; 4]| {
            let d = def(vec![Node::Const { v, n: 3 }, Node::Const { v: [0.0; 4], n: 1 }, Node::Mul { a: 0, b: 1 }], 2);
            codegen::compile(&d, &[]).key
        };
        assert_eq!(k([0.2, 0.4, 0.6, 1.0]), k([0.0; 4]));
        assert_eq!(k([0.2, 0.4, 0.6, 1.0]), k([0.5, 0.5, 0.0, 0.0]));
    }
}
