//! B-054: anti-aliasing and sharpening for n1's final image.
//!
//! The post chain's last pass used to write straight into the host's target. With AA or sharpening on, it writes into
//! an intermediate (`ldr`, the target's format, the size of the view) instead, and these passes filter that into the
//! target. The host draws the HUD, menus and text into the target afterwards, so none of it is filtered, and the chain
//! (grade, DOF, bloom, post materials) is untouched: AA sees the finished scene, as it should.
//!
//! Modes (`Mode`): FXAA (one pass), SMAA 1x (edge, weights, blend: three passes with the area and search lookup
//! textures, MIT, `aa/LICENSE-SMAA.txt`). Sharpening is a separate CAS pass after the AA. `aa.wgsl` has the shaders and
//! the credits. Nothing here runs, and nothing is allocated, while both are off.

use super::*;

pub const AA_WGSL: &str = include_str!("../aa.wgsl");
const AREA_LUT: &[u8] = include_bytes!("../aa/smaa_area.lut"); // 160 x 560 RG8
const SEARCH_LUT: &[u8] = include_bytes!("../aa/smaa_search.lut"); // 64 x 16 R8

/// The anti-aliasing filter, as the host's setting numbers it (0, 1, 2).
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Mode {
    #[default]
    Off,
    Fxaa,
    Smaa,
}

impl Mode {
    /// Anything unknown is off.
    pub fn from_setting(v: u32) -> Self {
        match v {
            1 => Mode::Fxaa,
            2 => Mode::Smaa,
            _ => Mode::Off,
        }
    }
}

/// What a frame does for the current settings.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Plan {
    pub mode: Mode,
    /// 0 = no sharpening pass, else the strength in (0, 1].
    pub sharpen: f32,
}

impl Plan {
    pub fn new(mode: Mode, sharpen: f32) -> Self {
        // NaN and anything under a hundredth is off; the slider is 0 to 1.
        let s = if sharpen.is_finite() && sharpen >= 0.01 { sharpen.min(1.0) } else { 0.0 };
        Plan { mode, sharpen: s }
    }
    /// Does the final image need the intermediate target and these passes?
    pub fn active(&self) -> bool {
        self.mode != Mode::Off || self.sharpen > 0.0
    }
    /// The anti-aliasing result goes to a second target when the sharpening pass follows it.
    pub fn needs_second(&self) -> bool {
        self.mode != Mode::Off && self.sharpen > 0.0
    }
}

/// CAS's `peak` for a strength in [0, 1]: -1 / lerp(8, 5, strength). The strongest setting (1) lets the weight reach
/// -0.2, the gentlest (0+) -0.125.
pub fn cas_peak(sharpen: f32) -> f32 {
    -1.0 / (8.0 - 3.0 * sharpen.clamp(0.0, 1.0))
}

/// The push constants of `Pc` in aa.wgsl, in order.
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct Pc {
    inv_size: [f32; 2],
    size: [f32; 2],
    offset: [f32; 2],
    flags: f32,
    peak: f32,
    fxaa_sub: f32,
    fxaa_thresh: f32,
    fxaa_floor: f32,
    _pad: f32,
}

pub const FLAG_SRGB: f32 = 1.0;
pub const FLAG_HDR: f32 = 2.0;

/// The shader's tuning for FXAA. Sub-pixel amount 0.75, edge threshold 1/8, floor 1/32: the published defaults of the
/// high-quality preset.
pub const FXAA_SUB: f32 = 0.75;
pub const FXAA_THRESHOLD: f32 = 0.125;
pub const FXAA_FLOOR: f32 = 0.0312;

struct Sets {
    fxaa: vk::DescriptorSet,
    cas: vk::DescriptorSet,    // reads ldr
    cas2: vk::DescriptorSet,   // reads ldr2
    edge: vk::DescriptorSet,
    weights: vk::DescriptorSet,
    blend: vk::DescriptorSet,
}

struct Targets {
    w: u32,
    h: u32,
    second: bool,
    smaa: bool,
    ldr: Rt,
    ldr2: Option<Rt>,
    edges: Option<Rt>,
    weights: Option<Rt>,
    sets: Sets,
}

pub struct Aa {
    set_layout: vk::DescriptorSetLayout,
    pipe_layout: vk::PipelineLayout,
    module: vk::ShaderModule,
    /// The SPIR-V, kept only where each stage gets a module of its own (`shaders::split_entries`, Adreno).
    words: Vec<u32>,
    split: HashMap<&'static str, vk::ShaderModule>,
    pipes: HashMap<(&'static str, i32), vk::Pipeline>,
    /// Texture slots (area, search) in `Renderer::textures`.
    lut: Option<(u32, u32)>,
    t: Option<Targets>,
}

impl Aa {
    /// The targets, for freeing (the caller defers or frees them).
    pub fn take_targets(&mut self) -> Vec<Rt> {
        match self.t.take() {
            Some(t) => [Some(t.ldr), t.ldr2, t.edges, t.weights].into_iter().flatten().collect(),
            None => Vec::new(),
        }
    }

    pub fn destroy(&mut self, device: &ash::Device) {
        unsafe {
            for (_, p) in self.pipes.drain() {
                device.destroy_pipeline(p, None);
            }
            device.destroy_pipeline_layout(self.pipe_layout, None);
            device.destroy_descriptor_set_layout(self.set_layout, None);
            device.destroy_shader_module(self.module, None);
            for (_, m) in self.split.drain() {
                device.destroy_shader_module(m, None);
            }
        }
    }
}

impl Renderer {
    /// The image and view the post chain's last pass writes while the filters run, when they do.
    pub(super) fn aa_ldr(&self) -> Option<(vk::Image, vk::ImageView)> {
        self.aa.as_ref().and_then(|a| a.t.as_ref()).map(|t| (t.ldr.image, t.ldr.view))
    }

    /// The plan for this frame from the host's settings.
    pub(super) fn aa_plan(&self) -> Plan {
        Plan::new(Mode::from_setting(self.aa_mode), self.aa_sharpen)
    }

    /// Makes (once) the layouts and the shader module the filters share.
    fn aa_init(&mut self) -> Result<()> {
        if self.aa.is_some() {
            return Ok(());
        }
        let d = &self.device;
        let b = |i: u32, ty| {
            vk::DescriptorSetLayoutBinding::default()
                .binding(i)
                .descriptor_type(ty)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT)
        };
        let bindings = [
            b(0, vk::DescriptorType::SAMPLER),
            b(1, vk::DescriptorType::SAMPLED_IMAGE),
            b(2, vk::DescriptorType::SAMPLED_IMAGE),
            b(3, vk::DescriptorType::SAMPLED_IMAGE),
            b(4, vk::DescriptorType::SAMPLED_IMAGE),
        ];
        let (set_layout, pipe_layout, module, words) = unsafe {
            let set_layout =
                d.create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&bindings), None)?;
            let sets = [set_layout];
            let pcr = [vk::PushConstantRange {
                stage_flags: vk::ShaderStageFlags::FRAGMENT,
                offset: 0,
                size: std::mem::size_of::<Pc>() as u32,
            }];
            let pipe_layout = d.create_pipeline_layout(
                &vk::PipelineLayoutCreateInfo::default().set_layouts(&sets).push_constant_ranges(&pcr),
                None,
            )?;
            let words = shaders::spirv_caps(AA_WGSL, naga::valid::Capabilities::IMMEDIATES)?;
            let module = d.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&words), None)?;
            (set_layout, pipe_layout, module, words)
        };
        self.aa = Some(Aa {
            set_layout,
            pipe_layout,
            module,
            words: if shaders::split_entries() { words } else { Vec::new() },
            split: HashMap::new(),
            pipes: HashMap::new(),
            lut: None,
            t: None,
        });
        Ok(())
    }

    /// B-198: builds every filter pipeline (FXAA, SMAA's three, the sharpener) at startup, so neither the first scene
    /// frame nor a later switch of the anti-aliasing setting compiles anything.
    pub(super) fn aa_prewarm(&mut self) -> Result<()> {
        self.aa_init()?;
        let cf = self.color_format;
        for (entry, f) in [
            ("fs_fxaa", cf),
            ("fs_smaa_edge", vk::Format::R8G8_UNORM),
            ("fs_smaa_weights", vk::Format::R8G8B8A8_UNORM),
            ("fs_smaa_blend", cf),
            ("fs_cas", cf),
        ] {
            self.aa_pipe(entry, f)?;
        }
        Ok(())
    }

    /// Makes (once) the device objects, and (per size and plan) the targets and descriptor sets. Frees the targets when
    /// the plan is off. Call before the frame borrows the renderer; `aa_record` then needs no `&mut`.
    pub(super) fn aa_ensure(&mut self, w: u32, h: u32, plan: Plan) -> Result<()> {
        if !plan.active() {
            // Keep the cheap device objects, drop the targets (deferred: the GPU may still read them).
            self.aa_drop_targets();
            return Ok(());
        }
        self.aa_init()?;
        let smaa = plan.mode == Mode::Smaa;
        if smaa && self.aa.as_ref().unwrap().lut.is_none() {
            let area = self.upload_texture(vk::Format::R8G8_UNORM, 160, 560, &[AREA_LUT])?;
            let search = self.upload_texture(vk::Format::R8_UNORM, 64, 16, &[SEARCH_LUT])?;
            self.aa.as_mut().unwrap().lut = Some((area, search));
        }
        let cf = self.color_format;
        let want: &[(&'static str, vk::Format)] = match plan.mode {
            Mode::Off => &[],
            Mode::Fxaa => &[("fs_fxaa", vk::Format::UNDEFINED)],
            Mode::Smaa => &[
                ("fs_smaa_edge", vk::Format::R8G8_UNORM),
                ("fs_smaa_weights", vk::Format::R8G8B8A8_UNORM),
                ("fs_smaa_blend", vk::Format::UNDEFINED),
            ],
        };
        for &(entry, f) in want {
            self.aa_pipe(entry, if f == vk::Format::UNDEFINED { cf } else { f })?;
        }
        if plan.sharpen > 0.0 {
            self.aa_pipe("fs_cas", cf)?;
        }
        let a = self.aa.as_ref().unwrap();
        let fits = a
            .t
            .as_ref()
            .is_some_and(|t| t.w == w && t.h == h && t.second == plan.needs_second() && t.smaa == smaa);
        if fits {
            return Ok(());
        }
        self.aa_drop_targets();
        let second = plan.needs_second();
        let ldr = self.rt(cf, w, h, false, "n1 aa ldr")?;
        let ldr2 = if second { Some(self.rt(cf, w, h, false, "n1 aa ldr2")?) } else { None };
        let edges = if smaa { Some(self.rt(vk::Format::R8G8_UNORM, w, h, false, "n1 smaa edges")?) } else { None };
        let weights = if smaa { Some(self.rt(vk::Format::R8G8B8A8_UNORM, w, h, false, "n1 smaa weights")?) } else { None };
        let layout = self.aa.as_ref().unwrap().set_layout;
        let (area_v, search_v) = match self.aa.as_ref().unwrap().lut {
            Some((a, s)) => (self.tex_view(a, TEX_BLACK), self.tex_view(s, TEX_BLACK)),
            // Without SMAA the lookups are never read; any sampled image fills the slot.
            None => (self.tex_view(TEX_BLACK, TEX_BLACK), self.tex_view(TEX_BLACK, TEX_BLACK)),
        };
        // (tex0, tex1) of each set; the slots a pass doesn't read hold the lookups' views (always readable).
        let sets = [
            (ldr.view, search_v),
            (ldr.view, search_v),
            (ldr2.as_ref().map_or(ldr.view, |r| r.view), search_v),
            (ldr.view, search_v),
            (edges.as_ref().map_or(ldr.view, |r| r.view), search_v),
            (ldr.view, weights.as_ref().map_or(ldr.view, |r| r.view)),
        ];
        let mut made = Vec::new();
        for (t0, t1) in sets {
            let set = self.alloc_set(layout)?;
            let smp = [vk::DescriptorImageInfo { sampler: self.post_sampler, ..Default::default() }];
            let img = |v| {
                [vk::DescriptorImageInfo {
                    sampler: vk::Sampler::null(),
                    image_view: v,
                    image_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                }]
            };
            let (i0, i1, i2, i3) = (img(t0), img(t1), img(area_v), img(search_v));
            fn w<'a>(
                set: vk::DescriptorSet,
                binding: u32,
                ty: vk::DescriptorType,
                info: &'a [vk::DescriptorImageInfo],
            ) -> vk::WriteDescriptorSet<'a> {
                vk::WriteDescriptorSet::default().dst_set(set).dst_binding(binding).descriptor_type(ty).image_info(info)
            }
            unsafe {
                self.device.update_descriptor_sets(
                    &[
                        w(set, 0, vk::DescriptorType::SAMPLER, &smp),
                        w(set, 1, vk::DescriptorType::SAMPLED_IMAGE, &i0),
                        w(set, 2, vk::DescriptorType::SAMPLED_IMAGE, &i1),
                        w(set, 3, vk::DescriptorType::SAMPLED_IMAGE, &i2),
                        w(set, 4, vk::DescriptorType::SAMPLED_IMAGE, &i3),
                    ],
                    &[],
                );
            }
            made.push(set);
        }
        let sets = Sets { fxaa: made[0], cas: made[1], cas2: made[2], edge: made[3], weights: made[4], blend: made[5] };
        self.aa.as_mut().unwrap().t = Some(Targets { w, h, second, smaa, ldr, ldr2, edges, weights, sets });
        Ok(())
    }

    fn aa_drop_targets(&mut self) {
        let rts = self.aa.as_mut().map(|a| a.take_targets()).unwrap_or_default();
        for r in rts {
            self.defer(Garbage::Image(r.image, r.view, r.alloc));
        }
    }

    fn aa_pipe(&mut self, entry: &'static str, out: vk::Format) -> Result<()> {
        let k = (entry, out.as_raw());
        if self.aa.as_ref().unwrap().pipes.contains_key(&k) {
            return Ok(());
        }
        let t0 = std::time::Instant::now();
        let layout = self.aa.as_ref().unwrap().pipe_layout;
        let mut stage = |name: &'static str| -> Result<vk::ShaderModule> {
            let a = self.aa.as_mut().unwrap();
            if let Some(&m) = a.split.get(name) {
                return Ok(m);
            }
            let (m, own) = unsafe { shaders::stage_module(&self.device, a.module, &a.words, name)? };
            if let Some(own) = own {
                a.split.insert(name, own);
            }
            Ok(m)
        };
        let (vs_module, fs_module) = (stage("vs")?, stage(entry)?);
        let p = unsafe {
            let name = std::ffi::CString::new(entry).unwrap();
            let stages = [
                vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::VERTEX).module(vs_module).name(c"vs"),
                vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::FRAGMENT).module(fs_module).name(&name),
            ];
            let vi = vk::PipelineVertexInputStateCreateInfo::default();
            let ia = vk::PipelineInputAssemblyStateCreateInfo::default().topology(vk::PrimitiveTopology::TRIANGLE_LIST);
            let vp = vk::PipelineViewportStateCreateInfo::default().viewport_count(1).scissor_count(1);
            let rs = vk::PipelineRasterizationStateCreateInfo::default()
                .polygon_mode(vk::PolygonMode::FILL)
                .cull_mode(vk::CullModeFlags::NONE)
                .line_width(1.0);
            let ms = vk::PipelineMultisampleStateCreateInfo::default().rasterization_samples(vk::SampleCountFlags::TYPE_1);
            let ds = vk::PipelineDepthStencilStateCreateInfo::default();
            let att = [vk::PipelineColorBlendAttachmentState::default().color_write_mask(vk::ColorComponentFlags::RGBA)];
            let cb = vk::PipelineColorBlendStateCreateInfo::default().attachments(&att);
            let dyns = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
            let dy = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyns);
            let formats = [out];
            let mut rendering = vk::PipelineRenderingCreateInfo::default().color_attachment_formats(&formats);
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
            let _use = self.store.cache_use.read().unwrap_or_else(|p| p.into_inner());
            self.device
                .create_graphics_pipelines(self.store.cache, &[info], None)
                .map_err(|(_, e)| anyhow!("aa pipeline {entry}: {e}"))?[0]
        };
        self.aa.as_mut().unwrap().pipes.insert(k, p);
        crate::compile_log::record("aa_pipe", &format!("{entry}/{}", out.as_raw()), t0.elapsed().as_micros() as u64, true);
        Ok(())
    }

    /// Records the filters from `ldr` into `target`. `aa_ensure` ran with the same plan and size; the post chain left
    /// `ldr` in the sampled layout.
    pub(super) fn aa_record(&self, cmd: vk::CommandBuffer, target: &Target, plan: Plan) -> bool {
        let mut subs = false;
        let a = self.aa.as_ref().expect("aa_ensure first");
        let t = a.t.as_ref().expect("aa_ensure first");
        let d = &self.device;
        let (w, h) = (t.w, t.h);
        let full = vk::Rect2D { offset: vk::Offset2D { x: 0, y: 0 }, extent: vk::Extent2D { width: w, height: h } };
        let cf = self.color_format;
        let color = vk::ImageAspectFlags::COLOR;
        let frag = vk::PipelineStageFlags::FRAGMENT_SHADER;
        let cao = vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT;
        let to_att = |img| {
            layout_barrier(
                img,
                color,
                vk::ImageLayout::UNDEFINED,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                vk::AccessFlags::empty(),
                vk::AccessFlags::COLOR_ATTACHMENT_WRITE,
            )
        };
        let to_read = |img| {
            layout_barrier(
                img,
                color,
                vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                vk::AccessFlags::COLOR_ATTACHMENT_WRITE,
                vk::AccessFlags::SHADER_READ,
            )
        };
        let barrier = |src, dst, b: &[vk::ImageMemoryBarrier]| unsafe {
            d.cmd_pipeline_barrier(cmd, src, dst, vk::DependencyFlags::empty(), &[], &[], b)
        };
        let flags = if is_srgb(cf) { FLAG_SRGB } else { 0.0 } + if self.hdr_output { FLAG_HDR } else { 0.0 };
        let pc = |off: [f32; 2]| Pc {
            inv_size: [1.0 / w as f32, 1.0 / h as f32],
            size: [w as f32, h as f32],
            offset: off,
            flags,
            peak: cas_peak(plan.sharpen),
            fxaa_sub: FXAA_SUB,
            fxaa_thresh: FXAA_THRESHOLD,
            fxaa_floor: FXAA_FLOOR,
            _pad: 0.0,
        };
        let draw = |view: vk::ImageView, layout: vk::ImageLayout, rect: vk::Rect2D, entry: &'static str, fmt: vk::Format, set, clear: bool, off: [f32; 2]| unsafe {
            let pipe = a.pipes[&(entry, fmt.as_raw())];
            let att = [vk::RenderingAttachmentInfo::default()
                .image_view(view)
                .image_layout(layout)
                .load_op(if clear { vk::AttachmentLoadOp::CLEAR } else { vk::AttachmentLoadOp::DONT_CARE })
                .store_op(vk::AttachmentStoreOp::STORE)
                .clear_value(vk::ClearValue { color: vk::ClearColorValue { float32: [0.0; 4] } })];
            d.cmd_begin_rendering(
                cmd,
                &vk::RenderingInfo::default().render_area(rect).layer_count(1).color_attachments(&att),
            );
            d.cmd_set_viewport(
                cmd,
                0,
                &[vk::Viewport {
                    x: rect.offset.x as f32,
                    y: rect.offset.y as f32,
                    width: rect.extent.width as f32,
                    height: rect.extent.height as f32,
                    min_depth: 0.0,
                    max_depth: 1.0,
                }],
            );
            d.cmd_set_scissor(cmd, 0, &[rect]);
            d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pipe);
            d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, a.pipe_layout, 0, &[set], &[]);
            d.cmd_push_constants(cmd, a.pipe_layout, vk::ShaderStageFlags::FRAGMENT, 0, bytemuck::bytes_of(&pc(off)));
            d.cmd_draw(cmd, 3, 1, 0, 0);
            d.cmd_end_rendering(cmd);
        };
        let att = vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL;
        let tgt_off = [target.rect.offset.x as f32, target.rect.offset.y as f32];
        // Where the anti-aliasing result goes: the target, or the second image when the sharpening pass follows.
        let (aa_view, aa_layout, aa_rect, aa_off) = match &t.ldr2 {
            Some(r) if plan.needs_second() => (r.view, att, full, [0.0, 0.0]),
            _ => (target.view, target.layout, target.rect, tgt_off),
        };
        let mut cas_set = t.sets.cas;
        match plan.mode {
            Mode::Off => {}
            Mode::Fxaa => {
                if let Some(r) = t.ldr2.as_ref().filter(|_| plan.needs_second()) {
                    barrier(frag, cao, &[to_att(r.image)]);
                }
                draw(aa_view, aa_layout, aa_rect, "fs_fxaa", cf, t.sets.fxaa, false, aa_off);
            }
            Mode::Smaa => {
                let (e, wt) = (t.edges.as_ref().unwrap(), t.weights.as_ref().unwrap());
                barrier(frag, cao, &[to_att(e.image)]);
                draw(e.view, att, full, "fs_smaa_edge", vk::Format::R8G8_UNORM, t.sets.edge, true, [0.0, 0.0]);
                if let Some(g) = &self.gprof {
                    g.mark(d, cmd, 9);
                }
                barrier(cao, frag, &[to_read(e.image), to_att(wt.image)]);
                draw(wt.view, att, full, "fs_smaa_weights", vk::Format::R8G8B8A8_UNORM, t.sets.weights, true, [0.0, 0.0]);
                if let Some(g) = &self.gprof {
                    g.mark(d, cmd, 10);
                    subs = true;
                }
                barrier(cao, frag, &[to_read(wt.image)]);
                if let Some(r) = t.ldr2.as_ref().filter(|_| plan.needs_second()) {
                    barrier(frag, cao, &[to_att(r.image)]);
                }
                draw(aa_view, aa_layout, aa_rect, "fs_smaa_blend", cf, t.sets.blend, false, aa_off);
            }
        }
        if plan.needs_second() {
            let r = t.ldr2.as_ref().unwrap();
            barrier(cao, frag, &[to_read(r.image)]);
            cas_set = t.sets.cas2;
        }
        if plan.sharpen > 0.0 {
            draw(target.view, target.layout, target.rect, "fs_cas", cf, cas_set, false, tgt_off);
        }
        subs
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_map_to_modes() {
        assert_eq!(Mode::from_setting(0), Mode::Off);
        assert_eq!(Mode::from_setting(1), Mode::Fxaa);
        assert_eq!(Mode::from_setting(2), Mode::Smaa);
        assert_eq!(Mode::from_setting(3), Mode::Off);
        assert_eq!(Mode::from_setting(u32::MAX), Mode::Off);
    }

    #[test]
    fn off_and_zero_do_nothing() {
        assert!(!Plan::new(Mode::Off, 0.0).active());
        assert!(!Plan::new(Mode::Off, f32::NAN).active());
        assert!(!Plan::new(Mode::Off, 0.005).active());
        assert!(Plan::new(Mode::Fxaa, 0.0).active());
        assert!(Plan::new(Mode::Off, 0.5).active());
    }

    #[test]
    fn sharpening_clamps_to_one() {
        assert_eq!(Plan::new(Mode::Off, 7.0).sharpen, 1.0);
        assert_eq!(Plan::new(Mode::Off, -1.0).sharpen, 0.0);
        assert_eq!(Plan::new(Mode::Smaa, f32::INFINITY).sharpen, 0.0);
    }

    #[test]
    fn second_target_only_when_both_run() {
        assert!(!Plan::new(Mode::Fxaa, 0.0).needs_second());
        assert!(!Plan::new(Mode::Off, 0.5).needs_second());
        assert!(Plan::new(Mode::Smaa, 0.5).needs_second());
    }

    #[test]
    fn cas_peak_follows_the_published_range() {
        // CAS: peak = -1 / lerp(8, 5, sharpness)
        assert!((cas_peak(0.0) + 0.125).abs() < 1e-6);
        assert!((cas_peak(1.0) + 0.2).abs() < 1e-6);
        assert!((cas_peak(0.5) + 1.0 / 6.5).abs() < 1e-6);
        assert_eq!(cas_peak(9.0), cas_peak(1.0));
    }

    #[test]
    fn push_constants_are_48_bytes() {
        // aa.wgsl's `Pc` is three vec4s.
        assert_eq!(std::mem::size_of::<Pc>(), 48);
    }

    #[test]
    fn lookup_textures_have_the_published_sizes() {
        assert_eq!(AREA_LUT.len(), 160 * 560 * 2);
        assert_eq!(SEARCH_LUT.len(), 64 * 16);
    }

    #[test]
    fn shaders_compile_and_have_every_entry_point() {
        let w = shaders::spirv_caps(AA_WGSL, naga::valid::Capabilities::IMMEDIATES).expect("aa.wgsl");
        let names: Vec<String> = shaders::entry_points(&w).into_iter().map(|e| e.1).collect();
        for n in ["vs", "fs_fxaa", "fs_cas", "fs_smaa_edge", "fs_smaa_weights", "fs_smaa_blend"] {
            assert!(names.iter().any(|x| x == n), "missing {n}: {names:?}");
        }
    }
}
