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

/// The edge weight of the mask pixel `c` (0 or 1) given its four side neighbours and four diagonal neighbours (each 0
/// or 1). Mirrors `outline_edge` in post.wgsl: a pixel outside the mask next to it is the line (1), a pixel only
/// diagonal to it is the weak shoulder (`DIAG`), anything else 0. The shader and this function must agree; the tests
/// below pin the shape.
pub fn edge_weight(c: f32, sides: [f32; 4], diags: [f32; 4]) -> f32 {
    let n: f32 = sides.iter().sum();
    let d: f32 = diags.iter().sum();
    let e = if n > 0.0 {
        1.0
    } else if d > 0.0 {
        DIAG
    } else {
        0.0
    };
    (1.0 - c) * e
}

/// The weight of a pixel that touches the mask only at a corner (post.wgsl `OUTLINE_DIAG`).
pub const DIAG: f32 = 0.35;

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

    #[test]
    fn shaders_compile() {
        crate::shaders::spirv(MASK_WGSL).expect("mask shader");
        crate::shaders::spirv(&crate::shaders::post_source()).expect("post shader");
    }

    #[test]
    fn line_is_outside_the_mask() {
        // Inside the mask: nothing, whatever its neighbours.
        assert_eq!(edge_weight(1.0, [1.0, 1.0, 1.0, 1.0], [1.0; 4]), 0.0);
        assert_eq!(edge_weight(1.0, [0.0; 4], [0.0; 4]), 0.0);
        // Outside and touching a covered pixel on a side: the full line.
        assert_eq!(edge_weight(0.0, [1.0, 0.0, 0.0, 0.0], [0.0; 4]), 1.0);
        assert_eq!(edge_weight(0.0, [0.0, 0.0, 0.0, 1.0], [1.0, 1.0, 0.0, 0.0]), 1.0);
    }

    #[test]
    fn corner_only_is_the_weak_shoulder() {
        assert_eq!(edge_weight(0.0, [0.0; 4], [0.0, 0.0, 1.0, 0.0]), DIAG);
        assert_eq!(edge_weight(0.0, [0.0; 4], [1.0; 4]), DIAG);
    }

    #[test]
    fn far_from_the_mask_is_empty() {
        assert_eq!(edge_weight(0.0, [0.0; 4], [0.0; 4]), 0.0);
    }

    #[test]
    fn a_one_pixel_dot_gets_a_ring_one_pixel_wide() {
        // 7x7 grid, one covered pixel in the middle: the 4 side neighbours are 1, the 4 corners are DIAG, nothing else.
        let m = |x: i32, y: i32| if x == 3 && y == 3 { 1.0 } else { 0.0 };
        let w = |x: i32, y: i32| {
            edge_weight(
                m(x, y),
                [m(x - 1, y), m(x + 1, y), m(x, y - 1), m(x, y + 1)],
                [m(x - 1, y - 1), m(x + 1, y - 1), m(x - 1, y + 1), m(x + 1, y + 1)],
            )
        };
        let mut sum = 0.0;
        for y in 0..7 {
            for x in 0..7 {
                sum += w(x, y);
            }
        }
        assert!((sum - (4.0 + 4.0 * DIAG)).abs() < 1e-6);
        assert_eq!(w(3, 3), 0.0);
        assert_eq!(w(3, 2), 1.0);
        assert_eq!(w(2, 2), DIAG);
        assert_eq!(w(3, 1), 0.0);
    }
}
