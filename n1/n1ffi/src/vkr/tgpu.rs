//! B-187: the Vulkan side of the SpeedTree trees. One vertex/index buffer per captured asset, one storage buffer of
//! 288-byte instances per frame slot, drawn inside the scene pass (they test and write depth like geometry) with the
//! renderer's shared light-word buffer for the trees' baked vertex light. Set 0: globals, instances, light words;
//! set 1 (per asset material): diffuse texture and sampler.

use super::*;
use crate::particles::{PFrame, tex_desc};
use crate::trees::{GDraw, Kind, LoadedTree, STRIDE, TFrame, TInst};

/// Bytes of the globals block at the start of a slot's tree buffer; instances start at `T_BASE`.
const T_BASE: u64 = 256;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct TGlobals {
    vp: [f32; 16],
    cam: [f32; 4],
    right: [f32; 4],
    up: [f32; 4],
    fwd: [f32; 4],
    sun_dir: [f32; 4],
    sun_col: [f32; 4],
    amb_up: [f32; 4],
    amb_dn: [f32; 4],
    fog_col: [f32; 4],
    fog_par: [f32; 4],
    tune: [f32; 4],
}

struct TDraw {
    kind: Kind,
    first: u32,
    count: u32,
    set: vk::DescriptorSet,
    masked: bool,
}

struct TAsset {
    vbuf: Buf,
    ibuf: Buf,
    draws: Vec<TDraw>,
}

struct TSlot {
    buf: Buf,
    set: vk::DescriptorSet,
}

pub(super) struct TGpu {
    g_layout: vk::DescriptorSetLayout,
    m_layout: vk::DescriptorSetLayout,
    pipe_layout: vk::PipelineLayout,
    module: vk::ShaderModule,
    /// Opaque, alpha-tested.
    pipes: [vk::Pipeline; 2],
    assets: Vec<TAsset>,
    slots: Vec<Option<TSlot>>,
    tex_ids: HashMap<String, u32>,
}

impl TGpu {
    pub(super) unsafe fn new(device: &ash::Device) -> Result<TGpu> {
        unsafe {
            let b = |binding, ty, stages| {
                vk::DescriptorSetLayoutBinding::default()
                    .binding(binding)
                    .descriptor_type(ty)
                    .descriptor_count(1)
                    .stage_flags(stages)
            };
            let vf = vk::ShaderStageFlags::VERTEX | vk::ShaderStageFlags::FRAGMENT;
            let fr = vk::ShaderStageFlags::FRAGMENT;
            let g = [
                b(0, vk::DescriptorType::UNIFORM_BUFFER, vf),
                b(1, vk::DescriptorType::STORAGE_BUFFER, vk::ShaderStageFlags::VERTEX),
                b(2, vk::DescriptorType::STORAGE_BUFFER, vk::ShaderStageFlags::VERTEX),
            ];
            let g_layout =
                device.create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&g), None)?;
            let m = [b(0, vk::DescriptorType::SAMPLED_IMAGE, fr), b(1, vk::DescriptorType::SAMPLER, fr)];
            let m_layout =
                device.create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&m), None)?;
            let ls = [g_layout, m_layout];
            let pipe_layout =
                device.create_pipeline_layout(&vk::PipelineLayoutCreateInfo::default().set_layouts(&ls), None)?;
            Ok(TGpu {
                g_layout,
                m_layout,
                pipe_layout,
                module: vk::ShaderModule::null(),
                pipes: [vk::Pipeline::null(); 2],
                assets: Vec::new(),
                slots: Vec::new(),
                tex_ids: HashMap::new(),
            })
        }
    }

    pub(super) unsafe fn destroy(&mut self, device: &ash::Device) {
        unsafe {
            for p in self.pipes {
                if p != vk::Pipeline::null() {
                    device.destroy_pipeline(p, None);
                }
            }
            if self.module != vk::ShaderModule::null() {
                device.destroy_shader_module(self.module, None);
            }
            device.destroy_pipeline_layout(self.pipe_layout, None);
            device.destroy_descriptor_set_layout(self.g_layout, None);
            device.destroy_descriptor_set_layout(self.m_layout, None);
        }
    }
}

/// Index count drawn for a draw under a leaf density (whole quads trimmed off the end of leaf draws).
fn drawn_count(d: &TDraw, density: f32) -> u32 {
    if d.kind.is_leaf() {
        let q = (d.count / 6) as f32 * density.clamp(0.0, 1.0);
        (q.round() as u32 * 6).min(d.count)
    } else {
        d.count
    }
}

impl Renderer {
    pub(super) fn tg_drop_buffers(&mut self) {
        for a in std::mem::take(&mut self.tg.assets) {
            self.free(Garbage::Buffer(a.vbuf));
            self.free(Garbage::Buffer(a.ibuf));
        }
        for s in std::mem::take(&mut self.tg.slots).into_iter().flatten() {
            self.free(Garbage::Buffer(s.buf));
        }
    }

    /// Appends baked vertex light words (three per vertex) to the shared buffer; returns the base in vertices.
    pub fn tree_light_words(&mut self, words: &[u32]) -> u32 {
        self.push_words(words)
    }

    /// A captured tree asset: geometry buffers and one descriptor set per material.
    pub fn tree_add(&mut self, t: &LoadedTree) -> Result<u32> {
        let g = &t.geom;
        let vb: &[u8] = bytemuck::cast_slice(&g.vertices);
        let ib: &[u8] = bytemuck::cast_slice(&g.indices);
        let mut vbuf = self.buffer(vb.len() as u64, vk::BufferUsageFlags::VERTEX_BUFFER, "n1 tree v")?;
        vbuf.bytes()[..vb.len()].copy_from_slice(vb);
        let mut ibuf = self.buffer(ib.len() as u64, vk::BufferUsageFlags::INDEX_BUFFER, "n1 tree i")?;
        ibuf.bytes()[..ib.len()].copy_from_slice(ib);
        // One set per material (branch, frond, leaf).
        let mut sets = [vk::DescriptorSet::null(); 3];
        for (k, m) in t.mats.iter().enumerate() {
            let id = match &m.tex {
                Some(tex) => match self.tg.tex_ids.get(&tex.name) {
                    Some(&id) => id,
                    None => {
                        let mips: Vec<&[u8]> = tex.mips.iter().map(|x| x.as_slice()).collect();
                        let id = self.try_add_texture(&tex_desc(tex, &mips, false))?;
                        self.tg.tex_ids.insert(tex.name.clone(), id);
                        id
                    }
                },
                None => TEX_GREY,
            };
            let set = self.alloc_set(self.tg.m_layout)?;
            let img = [vk::DescriptorImageInfo {
                sampler: vk::Sampler::null(),
                image_view: self.tex_view(id, TEX_GREY),
                image_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            }];
            let smp = [vk::DescriptorImageInfo {
                sampler: self.sampler,
                image_view: vk::ImageView::null(),
                image_layout: vk::ImageLayout::UNDEFINED,
            }];
            let writes = [
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(0)
                    .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                    .image_info(&img),
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(1)
                    .descriptor_type(vk::DescriptorType::SAMPLER)
                    .image_info(&smp),
            ];
            unsafe { self.device.update_descriptor_sets(&writes, &[]) };
            sets[k] = set;
        }
        let draws = g
            .draws
            .iter()
            .map(|d: &GDraw| TDraw {
                kind: d.kind,
                first: d.first,
                count: d.count,
                set: sets[d.kind.material()],
                masked: t.mats[d.kind.material()].masked,
            })
            .collect();
        self.tg.assets.push(TAsset { vbuf, ibuf, draws });
        Ok(self.tg.assets.len() as u32 - 1)
    }

    fn tg_make_pipelines(&mut self) -> Result<()> {
        if self.tg.pipes[0] != vk::Pipeline::null() {
            return Ok(());
        }
        unsafe {
            if self.tg.module == vk::ShaderModule::null() {
                let spv = crate::shaders::spirv(include_str!("tree.wgsl"))?;
                self.tg.module =
                    self.device.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&spv), None)?;
            }
            let binds = [vk::VertexInputBindingDescription {
                binding: 0,
                stride: (STRIDE * 4) as u32,
                input_rate: vk::VertexInputRate::VERTEX,
            }];
            let attr = |location, format, offset| vk::VertexInputAttributeDescription { location, binding: 0, format, offset };
            let attrs = [
                attr(0, vk::Format::R32G32B32_SFLOAT, 0),
                attr(1, vk::Format::R32G32_SFLOAT, 12),
                attr(2, vk::Format::R32G32B32_SFLOAT, 20),
                attr(3, vk::Format::R32G32B32_SFLOAT, 32),
                attr(4, vk::Format::R32G32_SFLOAT, 44),
            ];
            let vi = vk::PipelineVertexInputStateCreateInfo::default()
                .vertex_binding_descriptions(&binds)
                .vertex_attribute_descriptions(&attrs);
            let ia = vk::PipelineInputAssemblyStateCreateInfo::default().topology(vk::PrimitiveTopology::TRIANGLE_LIST);
            let vp = vk::PipelineViewportStateCreateInfo::default().viewport_count(1).scissor_count(1);
            // Foliage is two-sided and the swap of Y and Z mirrors the winding, so nothing is culled.
            let rs = vk::PipelineRasterizationStateCreateInfo::default()
                .polygon_mode(vk::PolygonMode::FILL)
                .cull_mode(vk::CullModeFlags::NONE)
                .front_face(vk::FrontFace::COUNTER_CLOCKWISE)
                .line_width(1.0);
            let ms = vk::PipelineMultisampleStateCreateInfo::default().rasterization_samples(vk::SampleCountFlags::TYPE_1);
            let ds = vk::PipelineDepthStencilStateCreateInfo::default()
                .depth_test_enable(true)
                .depth_write_enable(true)
                .depth_compare_op(if std::env::var("BOTS_N1_TREE_DEBUG").as_deref() == Ok("2") { vk::CompareOp::ALWAYS } else { vk::CompareOp::GREATER });
            let att = [vk::PipelineColorBlendAttachmentState::default().color_write_mask(vk::ColorComponentFlags::RGBA)];
            let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&att);
            let dyns = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
            let dy = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyns);
            let formats = [HDR_FORMAT];
            for (k, fs) in [c"fs_opaque", c"fs_masked"].iter().enumerate() {
                let stages = [
                    vk::PipelineShaderStageCreateInfo::default()
                        .stage(vk::ShaderStageFlags::VERTEX)
                        .module(self.tg.module)
                        .name(c"vs"),
                    vk::PipelineShaderStageCreateInfo::default()
                        .stage(vk::ShaderStageFlags::FRAGMENT)
                        .module(self.tg.module)
                        .name(fs),
                ];
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
                    .layout(self.tg.pipe_layout)
                    .push_next(&mut rendering);
                let p = self
                    .device
                    .create_graphics_pipelines(vk::PipelineCache::null(), &[info], None)
                    .map_err(|(_, e)| anyhow!("tree pipeline {k}: {e}"))?[0];
                self.tg.pipes[k] = p;
            }
        }
        Ok(())
    }

    /// Makes the tree pipelines when the frame has trees to draw.
    pub(super) fn tg_pipelines(&mut self, tf: &TFrame) {
        if tf.calls.is_empty() {
            return;
        }
        if let Err(e) = self.tg_make_pipelines() {
            eprintln!("n1ffi: {e:#}");
        }
    }

    /// Writes this frame's globals and instances into the slot's tree buffer and its descriptor set.
    pub(super) fn tg_stage(&mut self, si: usize, tf: &TFrame, g: &crate::light::Globals, pf: &PFrame) -> Result<()> {
        if tf.calls.is_empty() {
            return Ok(());
        }
        while self.tg.slots.len() <= si {
            self.tg.slots.push(None);
        }
        let need = T_BASE + (tf.insts.len() * std::mem::size_of::<TInst>()) as u64;
        if self.tg.slots[si].is_none() {
            let buf = self.buffer(
                need.next_power_of_two().max(64 << 10),
                vk::BufferUsageFlags::UNIFORM_BUFFER | vk::BufferUsageFlags::STORAGE_BUFFER,
                "n1 trees",
            )?;
            let set = self.alloc_set(self.tg.g_layout)?;
            self.tg.slots[si] = Some(TSlot { buf, set });
        }
        if self.tg.slots[si].as_ref().unwrap().buf.cap < need {
            let buf = self.buffer(
                need.next_power_of_two(),
                vk::BufferUsageFlags::UNIFORM_BUFFER | vk::BufferUsageFlags::STORAGE_BUFFER,
                "n1 trees",
            )?;
            let old = std::mem::replace(&mut self.tg.slots[si].as_mut().unwrap().buf, buf);
            self.free(Garbage::Buffer(old)); // the slot's last use has completed
        }
        let v = |a: [f32; 3]| [a[0], a[1], a[2], 0.0];
        let tg = TGlobals {
            vp: g.view_proj,
            cam: g.cam_pos,
            right: v(pf.right),
            up: v(pf.up),
            fwd: v(pf.fwd),
            sun_dir: g.sun_dir,
            sun_col: g.sun_col,
            amb_up: g.amb_up,
            amb_dn: g.amb_dn,
            fog_col: g.fog_col,
            fog_par: g.fog_par,
            // vertex-light gain, the game's tree light gamma, alpha-test threshold
            tune: [g.tune[2], 2.2, 0.333, if std::env::var_os("BOTS_N1_TREE_DEBUG").is_some() { 1.0 } else { 0.0 }],
        };
        let slot = self.tg.slots[si].as_mut().unwrap();
        let b = slot.buf.bytes();
        b[..std::mem::size_of::<TGlobals>()].copy_from_slice(bytemuck::bytes_of(&tg));
        let ib: &[u8] = bytemuck::cast_slice(&tf.insts);
        b[T_BASE as usize..T_BASE as usize + ib.len()].copy_from_slice(ib);
        let (buffer, set) = (slot.buf.buffer, slot.set);
        let ub = [vk::DescriptorBufferInfo { buffer, offset: 0, range: std::mem::size_of::<TGlobals>() as u64 }];
        let sb = [vk::DescriptorBufferInfo { buffer, offset: T_BASE, range: ib.len().max(16) as u64 }];
        let lb = [vk::DescriptorBufferInfo { buffer: self.light.buffer, offset: 0, range: vk::WHOLE_SIZE }];
        let writes = [
            vk::WriteDescriptorSet::default()
                .dst_set(set)
                .dst_binding(0)
                .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER)
                .buffer_info(&ub),
            vk::WriteDescriptorSet::default()
                .dst_set(set)
                .dst_binding(1)
                .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                .buffer_info(&sb),
            vk::WriteDescriptorSet::default()
                .dst_set(set)
                .dst_binding(2)
                .descriptor_type(vk::DescriptorType::STORAGE_BUFFER)
                .buffer_info(&lb),
        ];
        unsafe { self.device.update_descriptor_sets(&writes, &[]) };
        Ok(())
    }

    /// Records the tree draws inside the scene pass; returns the draw count.
    pub(super) fn tg_record(&self, cmd: vk::CommandBuffer, si: usize, tf: &TFrame) -> u32 {
        if std::env::var_os("BOTS_N1_TREE_LOG").is_some() && !tf.calls.is_empty() {
            use std::sync::atomic::{AtomicU32, Ordering};
            static N: AtomicU32 = AtomicU32::new(0);
            if N.fetch_add(1, Ordering::Relaxed) % 300 == 0 {
                eprintln!("n1ffi: tg_record calls {} insts {} pipe {} slot {}", tf.calls.len(), tf.insts.len(), self.tg.pipes[0] != vk::Pipeline::null(), matches!(self.tg.slots.get(si), Some(Some(_))));
            }
        }
        if tf.calls.is_empty() || self.tg.pipes[0] == vk::Pipeline::null() {
            return 0;
        }
        let Some(Some(slot)) = self.tg.slots.get(si) else { return 0 };
        let d = &self.device;
        let mut n = 0;
        unsafe {
            d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, self.tg.pipe_layout, 0, &[slot.set], &[]);
            let mut cur_pipe = usize::MAX;
            for call in &tf.calls {
                let Some(a) = self.tg.assets.get(call.asset as usize) else { continue };
                d.cmd_bind_vertex_buffers(cmd, 0, &[a.vbuf.buffer], &[0]);
                d.cmd_bind_index_buffer(cmd, a.ibuf.buffer, 0, vk::IndexType::UINT32);
                for dr in &a.draws {
                    let count = drawn_count(dr, tf.leaf_density);
                    if count == 0 {
                        continue;
                    }
                    let pipe = dr.masked as usize;
                    if cur_pipe != pipe {
                        d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, self.tg.pipes[pipe]);
                        cur_pipe = pipe;
                    }
                    d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, self.tg.pipe_layout, 1, &[dr.set], &[]);
                    d.cmd_draw_indexed(cmd, count, call.count, dr.first, 0, call.first);
                    n += 1;
                }
            }
        }
        n
    }
}
