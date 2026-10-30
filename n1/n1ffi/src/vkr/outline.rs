//! B-277(e) (#24): the red outline the port draws around highlighted enemies.
//!
//! The engine marks a pawn whose mesh component has `bSpecialHighlight` by drawing it with the colour mask 0xF (scene
//! alpha written) instead of 0x7, and a post pass edge-detects that alpha and adds a thin red line (measured on the
//! port: about one pixel wide, additive in linear light before the gamma, K = (0.143, 0.0082, 0.0080); frozen with/without
//! captures in `docs/plans/nextgen/b277-issue24.md`). n1 has no alpha channel to mark, so the marked instances are drawn
//! into an R8 mask after the scene (depth-tested against the scene's own depth, so a pawn behind cover is not outlined
//! through it), and the final post pass (`gamma_out` in post.wgsl) adds the edge of that mask.
//!
//! The mask draw reuses the shadow depth pass's skinned vertex layout and the scene's set 0 (globals, instances, bones).
//! Nothing runs on a frame without a marked pawn. `BOTS_N1_OUTLINE=0` turns it all off; `BOTS_N1_OUTLINE=2` shows the
//! raw mask instead (debugging).

use super::*;

/// The mask shader: position only, the scene's skinning and view-projection, one 8-bit output.
pub const MASK_WGSL: &str = r#"
struct G { view_proj: mat4x4<f32> };
@group(0) @binding(0) var<uniform> g: G;
@group(0) @binding(1) var<storage, read> instances: array<Instance>;
@group(0) @binding(6) var<storage, read> bones: array<mat4x4<f32>>;
struct Instance {
    model: mat4x4<f32>,
    light_base: i32,
    flags: u32,
    p1: u32,
    p2: u32,
    sc0: vec4<f32>,
    sc1: vec4<f32>,
    sc2: vec4<f32>,
    lm_xf: vec4<f32>,
};

@vertex
fn vs_skin(@location(0) pos: vec3<f32>, @location(3) bi: vec4<u32>, @location(4) bw: vec4<f32>,
           @builtin(instance_index) ii: u32) -> @builtin(position) vec4<f32> {
    let base = instances[ii].p1;
    let m = bones[base + bi.x] * bw.x + bones[base + bi.y] * bw.y + bones[base + bi.z] * bw.z + bones[base + bi.w] * bw.w;
    let pu = (m * vec4<f32>(pos, 1.0)).xyz;
    return g.view_proj * vec4<f32>(pu.x, pu.z, pu.y, 1.0);
}

@fragment
fn fs_mask() -> @location(0) vec4<f32> {
    return vec4<f32>(1.0, 0.0, 0.0, 0.0);
}
"#;

pub const MASK_FORMAT: vk::Format = vk::Format::R8_UNORM;

/// The CPU reference of the shader's edge kernel (test only): gain and colour of the blend. Mirrored by the
/// `OUTLINE_*` constants of post.wgsl (`constants_match_the_shader` checks the numbers).
#[cfg(test)]
pub const GAIN: f32 = 0.3;
#[cfg(test)]
pub const COLOR: [f32; 3] = [1.1, 0.05, 0.04];

/// The edge strength at a pixel: max - min of the mask over the 3x3 neighbourhood. `mask(dx, dy)` is the mask at an
/// offset from the pixel (0 or 1). A straight silhouette edge gives two pixels of 1, one inside and one outside;
/// mirrors `outline_edge` in post.wgsl.
#[cfg(test)]
pub fn edge_strength(mask: impl Fn(i32, i32) -> f32) -> f32 {
    let (mut lo, mut hi) = (1.0f32, 0.0f32);
    for j in -1..=1i32 {
        for i in -1..=1i32 {
            let m = mask(i, j);
            lo = lo.min(m);
            hi = hi.max(m);
        }
    }
    hi - lo
}

/// The blended channel: `c` lerped toward the outline colour by `GAIN * edge` (post.wgsl `with_outline`).
#[cfg(test)]
pub fn blend(c: f32, edge: f32, channel: usize) -> f32 {
    let t = (GAIN * edge).clamp(0.0, 1.0);
    c + t * (COLOR[channel] - c)
}

pub(super) struct Outline {
    module: vk::ShaderModule,
    layout: vk::PipelineLayout,
    pipe: vk::Pipeline,
}

impl Outline {
    /// # Safety
    /// `device` must be valid; `set0` is the scene's set 0 layout.
    pub unsafe fn new(
        device: &ash::Device,
        set0: vk::DescriptorSetLayout,
        cache: vk::PipelineCache,
    ) -> Result<Outline> {
        unsafe {
            let sets = [set0];
            let layout = device.create_pipeline_layout(&vk::PipelineLayoutCreateInfo::default().set_layouts(&sets), None)?;
            let words = shaders::spirv(MASK_WGSL)?;
            let module = device.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&words), None)?;
            let stages = [
                vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::VERTEX).module(module).name(c"vs_skin"),
                vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::FRAGMENT).module(module).name(c"fs_mask"),
            ];
            let bindings = [vk::VertexInputBindingDescription {
                binding: 0,
                stride: std::mem::size_of::<SkinVertex>() as u32,
                input_rate: vk::VertexInputRate::VERTEX,
            }];
            let attr = |location, format, offset| vk::VertexInputAttributeDescription { location, binding: 0, format, offset };
            let attrs = [
                attr(0, vk::Format::R32G32B32_SFLOAT, 0),
                attr(3, vk::Format::R8G8B8A8_UINT, 32),
                attr(4, vk::Format::R8G8B8A8_UNORM, 36),
            ];
            let vi = vk::PipelineVertexInputStateCreateInfo::default()
                .vertex_binding_descriptions(&bindings)
                .vertex_attribute_descriptions(&attrs);
            let ia = vk::PipelineInputAssemblyStateCreateInfo::default().topology(vk::PrimitiveTopology::TRIANGLE_LIST);
            let vp = vk::PipelineViewportStateCreateInfo::default().viewport_count(1).scissor_count(1);
            // Both faces, like the shadow pass: the engine marks whatever the pawn's mesh draws. A small bias toward the
            // camera (reverse-Z: larger is nearer) so the pawn's own surface passes against the depth it wrote.
            let rs = vk::PipelineRasterizationStateCreateInfo::default()
                .polygon_mode(vk::PolygonMode::FILL)
                .cull_mode(vk::CullModeFlags::NONE)
                .depth_bias_enable(true)
                .depth_bias_constant_factor(8.0)
                .depth_bias_slope_factor(1.0)
                .line_width(1.0);
            let ms = vk::PipelineMultisampleStateCreateInfo::default().rasterization_samples(vk::SampleCountFlags::TYPE_1);
            let ds = vk::PipelineDepthStencilStateCreateInfo::default()
                .depth_test_enable(true)
                .depth_write_enable(false)
                .depth_compare_op(vk::CompareOp::GREATER_OR_EQUAL);
            let att = [vk::PipelineColorBlendAttachmentState::default().color_write_mask(vk::ColorComponentFlags::R)];
            let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&att);
            let dyns = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
            let dy = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyns);
            let formats = [MASK_FORMAT];
            let mut rendering = vk::PipelineRenderingCreateInfo::default()
                .color_attachment_formats(&formats)
                .depth_attachment_format(DEPTH_FORMAT);
            let info = vk::GraphicsPipelineCreateInfo::default()
                .stages(&stages)
                .vertex_input_state(&vi)
                .input_assembly_state(&ia)
                .viewport_state(&vp)
                .rasterization_state(&rs)
                .multisample_state(&ms)
                .depth_stencil_state(&ds)
                .color_blend_state(&cb)
                .dynamic_state(&dy)
                .layout(layout)
                .push_next(&mut rendering);
            let pipe = device
                .create_graphics_pipelines(cache, &[info], None)
                .map_err(|(_, e)| anyhow!("outline pipeline: {e}"))?[0];
            Ok(Outline { module, layout, pipe })
        }
    }

    /// # Safety
    /// The device must be idle of this pipeline.
    pub unsafe fn destroy(&mut self, device: &ash::Device) {
        unsafe {
            device.destroy_pipeline(self.pipe, None);
            device.destroy_pipeline_layout(self.layout, None);
            device.destroy_shader_module(self.module, None);
        }
    }
}

impl Renderer {
    /// Whether this frame has a marked instance with a drawable opaque section (so a mask is drawn and read).
    pub(super) fn outline_active(&self) -> bool {
        self.knobs.outline != 0
            && self.outline.is_some()
            && self.draws.iter().any(|d| d.key.skin && d.key.blend == 0 && self.inst_outline[d.instance as usize])
    }

    /// Records the mask pass: marked skinned draws into the R8 mask, depth-tested (read-only) against the scene depth.
    /// The depth must be in `DEPTH_STENCIL_READ_ONLY_OPTIMAL` and the mask in UNDEFINED/COLOR_ATTACHMENT_OPTIMAL as the
    /// caller arranged; returns the draws made.
    pub(super) fn record_outline(&self, cmd: vk::CommandBuffer, set0: vk::DescriptorSet, rw: u32, rh: u32) -> u32 {
        let d = &self.device;
        let (Some(ol), Some(mask), Some(depth)) = (self.outline.as_ref(), self.outline_mask.as_ref(), self.depth.as_ref())
        else {
            return 0;
        };
        let full = vk::Rect2D { offset: vk::Offset2D { x: 0, y: 0 }, extent: vk::Extent2D { width: rw, height: rh } };
        let catt = [vk::RenderingAttachmentInfo::default()
            .image_view(mask.view)
            .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
            .load_op(vk::AttachmentLoadOp::CLEAR)
            .store_op(vk::AttachmentStoreOp::STORE)
            .clear_value(vk::ClearValue { color: vk::ClearColorValue { float32: [0.0; 4] } })];
        let datt = vk::RenderingAttachmentInfo::default()
            .image_view(depth.view)
            .image_layout(vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL)
            .load_op(vk::AttachmentLoadOp::LOAD)
            .store_op(vk::AttachmentStoreOp::NONE);
        let mut n = 0;
        unsafe {
            d.cmd_begin_rendering(
                cmd,
                &vk::RenderingInfo::default().render_area(full).layer_count(1).color_attachments(&catt).depth_attachment(&datt),
            );
            d.cmd_set_viewport(
                cmd,
                0,
                &[vk::Viewport { x: 0.0, y: 0.0, width: rw as f32, height: rh as f32, min_depth: 0.0, max_depth: 1.0 }],
            );
            d.cmd_set_scissor(cmd, 0, &[full]);
            d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, ol.pipe);
            d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, ol.layout, 0, &[set0], &[]);
            let mut cur = u32::MAX;
            for dr in &self.draws {
                if !dr.key.skin || dr.key.blend != 0 || !self.inst_outline[dr.instance as usize] {
                    continue;
                }
                if cur != dr.mesh {
                    let m = &self.skin_meshes[dr.mesh as usize];
                    d.cmd_bind_vertex_buffers(cmd, 0, &[m.vbuf.buffer], &[0]);
                    d.cmd_bind_index_buffer(cmd, m.ibuf.buffer, 0, vk::IndexType::UINT32);
                    cur = dr.mesh;
                }
                d.cmd_draw_indexed(cmd, dr.index_count, 1, dr.first_index, 0, dr.instance);
                n += 1;
            }
            d.cmd_end_rendering(cmd);
        }
        n
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Mask of a vertical edge: covered for x >= 0 (offsets are relative to the pixel at `px`).
    fn step(px: i32) -> impl Fn(i32, i32) -> f32 {
        move |dx, _| if px + dx >= 0 { 1.0 } else { 0.0 }
    }

    #[test]
    fn shaders_compile() {
        crate::shaders::spirv(MASK_WGSL).expect("mask shader");
        crate::shaders::spirv(&crate::shaders::post_source()).expect("post shader");
    }

    #[test]
    fn constants_match_the_shader() {
        let src = include_str!("../post.wgsl");
        assert!(src.contains(&format!("OUTLINE_GAIN = {GAIN:?};")));
        assert!(src.contains(&format!("vec3<f32>({:?}, {:?}, {:?})", COLOR[0], COLOR[1], COLOR[2])));
    }

    #[test]
    fn a_straight_edge_is_two_pixels_one_inside_one_outside() {
        // Pixels -5..=4 relative to the edge between x = -1 (clear) and x = 0 (covered).
        let prof: Vec<f32> = (-5..=4).map(|x| edge_strength(step(x))).collect();
        let on: Vec<usize> = (0..prof.len()).filter(|&k| prof[k] > 0.5).collect();
        assert_eq!(on, vec![4, 5], "{prof:?}");
        assert!(prof.iter().all(|v| *v == 0.0 || *v == 1.0));
    }

    #[test]
    fn a_one_pixel_gap_or_speck_is_outlined_all_round() {
        // A single marked pixel: the 3x3 around it is all edge.
        let speck = |dx: i32, dy: i32| if dx == 0 && dy == 0 { 1.0 } else { 0.0 };
        assert_eq!(edge_strength(speck), 1.0);
        assert_eq!(edge_strength(|dx, dy| speck(dx - 1, dy + 1)), 1.0);
        // Two pixels away it is clear.
        assert_eq!(edge_strength(|dx, dy| speck(dx - 2, dy)), 0.0);
    }

    #[test]
    fn nothing_marked_or_all_marked_has_no_edge() {
        assert_eq!(edge_strength(|_, _| 0.0), 0.0);
        assert!(edge_strength(|_, _| 1.0) < 1e-6);
    }

    #[test]
    fn blend_lerps_toward_the_colour() {
        assert_eq!(blend(0.4, 0.0, 0), 0.4);
        // Edge 1 -> t = GAIN.
        assert!((blend(0.0, 1.0, 0) - GAIN * COLOR[0]).abs() < 1e-6);
        // A bright channel is pulled down toward a low outline colour (green on white).
        assert!(blend(1.0, 1.0, 1) < 1.0);
        // Clamped: a huge edge never overshoots the colour.
        assert!((blend(0.2, 100.0, 2) - COLOR[2]).abs() < 1e-6);
    }
}
