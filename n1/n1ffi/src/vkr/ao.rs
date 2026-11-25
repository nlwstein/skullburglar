use super::{Garbage, Globals, HDR_FORMAT, Renderer, layout_barrier};
use crate::shaders;
use anyhow::{Result, anyhow};
use ash::vk;

const AO_RADIUS: f32 = 25.0;
const FORMAT: vk::Format = vk::Format::R16G16_SFLOAT;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
pub(super) struct Parameters {
    inverse_view_proj: [f32; 16],
    size: [f32; 4],
    params: [f32; 4],
    depth_sample: [f32; 4],
}
const _: () = assert!(std::mem::size_of::<Parameters>() == 112);

/// Test hook `BOTS_N1_AO_FAIL`: `pipelines` makes the AO pipeline build fail, `frame` makes the per-frame setup fail.
fn forced_failure(stage: &str) -> bool {
    static MODE: std::sync::OnceLock<String> = std::sync::OnceLock::new();
    MODE.get_or_init(|| std::env::var("BOTS_N1_AO_FAIL").unwrap_or_default()) == stage
}

/// AO is optional, so a failure while setting it up must cost the AO and nothing else. `prepare_ao` errors used to
/// reach `n1_record`, which makes the host turn the whole new renderer off. The gate logs the first error, stops
/// asking for AO for the rest of the session, and lets the frame go on without it.
#[derive(Default)]
pub(super) struct AoGate {
    failed: bool,
}

impl AoGate {
    pub(super) fn usable(&self) -> bool {
        !self.failed
    }

    /// The result of this frame's AO setup: whether AO is drawn this frame.
    pub(super) fn resolve(&mut self, prepared: Result<bool>) -> bool {
        match prepared {
            Ok(on) => on,
            Err(e) => {
                if !self.failed {
                    eprintln!("n1ffi: ambient occlusion disabled for this session: {e:#}");
                }
                self.failed = true;
                false
            }
        }
    }
}

pub(crate) struct AmbientOcclusion {
    set_layout: vk::DescriptorSetLayout,
    layout: vk::PipelineLayout,
    pipes: [vk::Pipeline; 4],
}

impl AmbientOcclusion {
    pub(crate) fn new(device: &ash::Device, cache: vk::PipelineCache) -> Result<Self> {
        if forced_failure("pipelines") {
            return Err(anyhow!("forced by BOTS_N1_AO_FAIL=pipelines"));
        }
        let words = shaders::spirv(&shaders::ao_source())?;
        unsafe {
            let module = device.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&words), None)?;
            let bindings: Vec<_> = (0..4).map(|b| vk::DescriptorSetLayoutBinding::default()
                .binding(b).descriptor_count(1).stage_flags(vk::ShaderStageFlags::FRAGMENT)
                .descriptor_type(if b < 2 { vk::DescriptorType::UNIFORM_BUFFER } else { vk::DescriptorType::SAMPLED_IMAGE }))
                .collect();
            let set_layout = device.create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings), None)
                .inspect_err(|_| device.destroy_shader_module(module, None))?;
            let layout = device.create_pipeline_layout(&vk::PipelineLayoutCreateInfo::default().set_layouts(&[set_layout]), None)
                .inspect_err(|_| {
                    device.destroy_descriptor_set_layout(set_layout, None);
                    device.destroy_shader_module(module, None);
                })?;
            let mut ao = Self { set_layout, layout, pipes: [vk::Pipeline::null(); 4] };
            let mut modules = vec![module];
            // The modules may contain one entry point each on Adreno, as for the other n1 pipelines.
            let result = (|| -> Result<()> {
                let (vs, owned) = shaders::stage_module(device, module, &words, "vs")?;
                modules.extend(owned);
                for (i, name) in [c"fs_ao", c"fs_blur_h", c"fs_blur_v", c"fs_apply"].iter().enumerate() {
                    let (fs, owned) = shaders::stage_module(device, module, &words, name.to_str().unwrap())?;
                    modules.extend(owned);
                    let stages = [
                        vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::VERTEX).module(vs).name(c"vs"),
                        vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::FRAGMENT).module(fs).name(name),
                    ];
                    let vi = vk::PipelineVertexInputStateCreateInfo::default();
                    let ia = vk::PipelineInputAssemblyStateCreateInfo::default().topology(vk::PrimitiveTopology::TRIANGLE_LIST);
                    let vp = vk::PipelineViewportStateCreateInfo::default().viewport_count(1).scissor_count(1);
                    let rs = vk::PipelineRasterizationStateCreateInfo::default().polygon_mode(vk::PolygonMode::FILL)
                        .cull_mode(vk::CullModeFlags::NONE).line_width(1.0);
                    let ms = vk::PipelineMultisampleStateCreateInfo::default().rasterization_samples(vk::SampleCountFlags::TYPE_1);
                    let ds = vk::PipelineDepthStencilStateCreateInfo::default();
                    let apply = i == 3;
                    let att = [vk::PipelineColorBlendAttachmentState::default()
                        .blend_enable(apply).src_color_blend_factor(vk::BlendFactor::ONE)
                        .dst_color_blend_factor(vk::BlendFactor::SRC_ALPHA).color_blend_op(vk::BlendOp::ADD)
                        .color_write_mask(if apply { vk::ColorComponentFlags::R | vk::ColorComponentFlags::G | vk::ColorComponentFlags::B }
                            else { vk::ColorComponentFlags::R | vk::ColorComponentFlags::G })];
                    let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&att);
                    let dynamic = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
                    let dy = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dynamic);
                    let formats = [if apply { HDR_FORMAT } else { FORMAT }];
                    let mut rendering = vk::PipelineRenderingCreateInfo::default().color_attachment_formats(&formats);
                    let info = vk::GraphicsPipelineCreateInfo::default().stages(&stages).vertex_input_state(&vi)
                        .input_assembly_state(&ia).viewport_state(&vp).rasterization_state(&rs).multisample_state(&ms)
                        .depth_stencil_state(&ds).color_blend_state(&cb).dynamic_state(&dy).layout(layout).push_next(&mut rendering);
                    let pipe = device.create_graphics_pipelines(cache, &[info], None);
                    ao.pipes[i] = pipe.map_err(|(partial, e)| {
                        for p in partial { device.destroy_pipeline(p, None); }
                        anyhow!("ambient occlusion {}: {e}", name.to_string_lossy())
                    })?[0];
                }
                Ok(())
            })();
            for m in modules { device.destroy_shader_module(m, None); }
            if let Err(e) = result {
                ao.destroy(device);
                return Err(e);
            }
            Ok(ao)
        }
    }

    pub(crate) unsafe fn destroy(&self, device: &ash::Device) {
        unsafe {
            for p in self.pipes { device.destroy_pipeline(p, None); }
            device.destroy_pipeline_layout(self.layout, None);
            device.destroy_descriptor_set_layout(self.set_layout, None);
        }
    }
}

impl Renderer {
    pub(super) fn prepare_ao(&mut self, si: usize, at: u64, w: u32, h: u32, strength: f32, globals: &Globals) -> Result<bool> {
        if forced_failure("frame") {
            return Err(anyhow!("forced by BOTS_N1_AO_FAIL=frame"));
        }
        let Some(inverse_view_proj) = scenekit::inverse(&globals.view_proj) else { return Ok(false) };
        let (aw, ah) = (w.div_ceil(2), h.div_ceil(2));
        if !self.ao_targets.as_ref().is_some_and(|r| r[0].width == aw && r[0].height == ah) {
            if let Some(old) = self.ao_targets.take() {
                for r in old { self.defer(Garbage::Image(r.image, r.view, r.alloc)); }
            }
            let first = self.rt(FORMAT, aw, ah, false, "n1 ambient occlusion")?;
            let second = match self.rt(FORMAT, aw, ah, false, "n1 ambient occlusion filter") {
                Ok(r) => r,
                Err(e) => {
                    self.free(Garbage::Image(first.image, first.view, first.alloc));
                    return Err(e);
                }
            };
            self.ao_targets = Some([first, second]);
        }
        let [sample_x, sample_y] = self.depth_sample_offset();
        let parameters = Parameters {
            inverse_view_proj,
            size: [w as f32, h as f32, 1.0 / w as f32, 1.0 / h as f32],
            params: [self.near, AO_RADIUS, strength, (self.knobs.features & 8 != 0) as u32 as f32],
            depth_sample: [sample_x, sample_y, 0.0, 0.0],
        };
        let bytes = bytemuck::bytes_of(&parameters);
        self.slots[si].buf.bytes()[at as usize..at as usize + bytes.len()].copy_from_slice(bytes);
        let layout = self.store.ao().unwrap().set_layout;
        for i in 0..2 {
            if self.slots[si].ao_sets[i] == vk::DescriptorSet::null() {
                self.slots[si].ao_sets[i] = self.alloc_set(layout)?;
            }
            let slot = &self.slots[si];
            let set = slot.ao_sets[i];
            let globals = [vk::DescriptorBufferInfo { buffer: slot.buf.buffer, offset: super::GLOBALS_AT, range: std::mem::size_of::<Globals>() as u64 }];
            let parameters = [vk::DescriptorBufferInfo { buffer: slot.buf.buffer, offset: at, range: bytes.len() as u64 }];
            let depth = [vk::DescriptorImageInfo { sampler: vk::Sampler::null(), image_view: self.depth.as_ref().unwrap().view,
                image_layout: vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL }];
            let source = [vk::DescriptorImageInfo { sampler: vk::Sampler::null(), image_view: self.ao_targets.as_ref().unwrap()[i].view,
                image_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL }];
            let writes = [
                vk::WriteDescriptorSet::default().dst_set(set).dst_binding(0).descriptor_type(vk::DescriptorType::UNIFORM_BUFFER).buffer_info(&globals),
                vk::WriteDescriptorSet::default().dst_set(set).dst_binding(1).descriptor_type(vk::DescriptorType::UNIFORM_BUFFER).buffer_info(&parameters),
                vk::WriteDescriptorSet::default().dst_set(set).dst_binding(2).descriptor_type(vk::DescriptorType::SAMPLED_IMAGE).image_info(&depth),
                vk::WriteDescriptorSet::default().dst_set(set).dst_binding(3).descriptor_type(vk::DescriptorType::SAMPLED_IMAGE).image_info(&source),
            ];
            unsafe { self.device.update_descriptor_sets(&writes, &[]) };
        }
        Ok(true)
    }

    pub(super) fn record_ao(&self, cmd: vk::CommandBuffer, si: usize, w: u32, h: u32) {
        let d = &self.device;
        let ao = self.store.ao().unwrap();
        let r = self.ao_targets.as_ref().unwrap();
        let depth = self.depth.as_ref().unwrap();
        let frag = vk::PipelineStageFlags::FRAGMENT_SHADER;
        let color = vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT;
        let tests = vk::PipelineStageFlags::EARLY_FRAGMENT_TESTS | vk::PipelineStageFlags::LATE_FRAGMENT_TESTS;
        unsafe {
            d.cmd_pipeline_barrier(cmd, tests, frag, vk::DependencyFlags::empty(), &[], &[], &[
                layout_barrier(depth.image, vk::ImageAspectFlags::DEPTH, vk::ImageLayout::DEPTH_ATTACHMENT_OPTIMAL,
                    vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL, vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE, vk::AccessFlags::SHADER_READ),
            ]);
            for (i, (target, set)) in [(0, 0), (1, 0), (0, 1), (0, 0)].iter().copied().enumerate() {
                let apply = i == 3;
                let rt = if apply { self.hdr.as_ref().unwrap() } else { &r[target] };
                if !apply {
                    d.cmd_pipeline_barrier(cmd, frag, color, vk::DependencyFlags::empty(), &[], &[], &[
                        layout_barrier(rt.image, vk::ImageAspectFlags::COLOR, vk::ImageLayout::UNDEFINED,
                            vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL, vk::AccessFlags::SHADER_READ, vk::AccessFlags::COLOR_ATTACHMENT_WRITE),
                    ]);
                } else {
                    let memory = [vk::MemoryBarrier::default().src_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE)
                        .dst_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_READ | vk::AccessFlags::COLOR_ATTACHMENT_WRITE)];
                    d.cmd_pipeline_barrier(cmd, color, color, vk::DependencyFlags::empty(), &memory, &[], &[]);
                }
                let (width, height) = if apply { (w, h) } else { (rt.width, rt.height) };
                let rect = vk::Rect2D { offset: vk::Offset2D::default(), extent: vk::Extent2D { width, height } };
                let att = [vk::RenderingAttachmentInfo::default().image_view(rt.view).image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
                    .load_op(if apply { vk::AttachmentLoadOp::LOAD } else { vk::AttachmentLoadOp::DONT_CARE }).store_op(vk::AttachmentStoreOp::STORE)];
                d.cmd_begin_rendering(cmd, &vk::RenderingInfo::default().render_area(rect).layer_count(1).color_attachments(&att));
                d.cmd_set_viewport(cmd, 0, &[vk::Viewport { x: 0.0, y: 0.0, width: width as f32, height: height as f32, min_depth: 0.0, max_depth: 1.0 }]);
                d.cmd_set_scissor(cmd, 0, &[rect]);
                d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, ao.pipes[i]);
                d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, ao.layout, 0, &[self.slots[si].ao_sets[set]], &[]);
                d.cmd_draw(cmd, 3, 1, 0, 0);
                d.cmd_end_rendering(cmd);
                if !apply {
                    d.cmd_pipeline_barrier(cmd, color, frag, vk::DependencyFlags::empty(), &[], &[], &[
                        layout_barrier(rt.image, vk::ImageAspectFlags::COLOR, vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL, vk::AccessFlags::COLOR_ATTACHMENT_WRITE, vk::AccessFlags::SHADER_READ),
                    ]);
                }
            }
            // The foliage and remaining scene draws might write depth again.
            d.cmd_pipeline_barrier(cmd, frag | color, tests | color, vk::DependencyFlags::empty(), &[
                vk::MemoryBarrier::default().src_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE)
                    .dst_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_READ | vk::AccessFlags::COLOR_ATTACHMENT_WRITE),
            ], &[], &[
                layout_barrier(depth.image, vk::ImageAspectFlags::DEPTH, vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                    vk::ImageLayout::DEPTH_ATTACHMENT_OPTIMAL, vk::AccessFlags::SHADER_READ,
                    vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_READ | vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE),
            ]);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_setup_error_turns_ao_off_and_stays_off() {
        let mut gate = AoGate::default();
        assert!(gate.usable());
        assert!(gate.resolve(Ok(true)));
        assert!(!gate.resolve(Err(anyhow!("out of memory"))), "the failed frame draws without AO");
        assert!(!gate.usable(), "later frames don't ask for AO again");
    }

    #[test]
    fn a_skipped_frame_is_not_a_failure() {
        let mut gate = AoGate::default();
        assert!(!gate.resolve(Ok(false)), "singular view matrix: no AO this frame");
        assert!(gate.usable(), "the next frame may use AO");
    }
}
