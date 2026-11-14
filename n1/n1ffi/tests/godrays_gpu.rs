//! B-313: the real `fs_god` shader (post.wgsl, naga -> SPIR-V) on a GPU against `godref`, the decoded game shader on the
//! CPU. Synthetic masks (and a captured one, N1_GOD_MASK=file.pgm, 128x128 8-bit) are fed through the depth texture the
//! way n1 feeds its own sky mask: depth 0 is sky, depth 1 (view distance = near plane) is geometry. Skips, passing, when
//! no Vulkan 1.3 device loads (CI runners); N1_TEST_VULKAN=/path/to/libMoltenVK.dylib names the library to load.
//! Prints the maximum and mean error per case (`cargo test --test godrays_gpu -- --nocapture`).
#![allow(unsafe_op_in_unsafe_fn)] // a thin Vulkan harness: every call is unsafe, an extra block each adds nothing

use ash::vk;
use n1ffi::godref::{self, Mask, Params, GRID};
use std::ffi::CStr;

const POST_FLOATS: usize = n1ffi::light::POST_VECS * 4;
const POST_BYTES: u64 = (POST_FLOATS * std::mem::size_of::<f32>()) as u64;

struct Gpu {
    entry_keep: ash::Entry,
    instance: ash::Instance,
    pd: vk::PhysicalDevice,
    device: ash::Device,
    queue: vk::Queue,
    family: u32,
    mem: vk::PhysicalDeviceMemoryProperties,
}

fn init() -> Option<Gpu> {
    unsafe {
        let entry = match std::env::var("N1_TEST_VULKAN") {
            Ok(p) => ash::Entry::load_from(p).ok()?,
            Err(_) => ash::Entry::load().ok()?,
        };
        let exts = entry.enumerate_instance_extension_properties(None).ok()?;
        let has = |n: &CStr| exts.iter().any(|e| e.extension_name_as_c_str().is_ok_and(|x| x == n));
        let mut names: Vec<*const i8> = vec![];
        let mut flags = vk::InstanceCreateFlags::empty();
        if has(c"VK_KHR_portability_enumeration") {
            names.push(c"VK_KHR_portability_enumeration".as_ptr());
            flags |= vk::InstanceCreateFlags::ENUMERATE_PORTABILITY_KHR;
        }
        if has(c"VK_KHR_get_physical_device_properties2") {
            names.push(c"VK_KHR_get_physical_device_properties2".as_ptr());
        }
        let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_3);
        let instance = entry
            .create_instance(
                &vk::InstanceCreateInfo::default().application_info(&app).enabled_extension_names(&names).flags(flags),
                None,
            )
            .ok()?;
        for pd in instance.enumerate_physical_devices().ok()? {
            let props = instance.get_physical_device_properties(pd);
            if props.api_version < vk::API_VERSION_1_3 {
                continue;
            }
            let Some(family) = instance
                .get_physical_device_queue_family_properties(pd)
                .iter()
                .position(|q| q.queue_flags.contains(vk::QueueFlags::GRAPHICS))
            else {
                continue;
            };
            let dexts = instance.enumerate_device_extension_properties(pd).ok()?;
            let mut dnames: Vec<*const i8> = vec![];
            if dexts.iter().any(|e| e.extension_name_as_c_str().is_ok_and(|x| x == c"VK_KHR_portability_subset")) {
                dnames.push(c"VK_KHR_portability_subset".as_ptr());
            }
            let prio = [1.0f32];
            let qi = [vk::DeviceQueueCreateInfo::default().queue_family_index(family as u32).queue_priorities(&prio)];
            let mut f13 = vk::PhysicalDeviceVulkan13Features::default().dynamic_rendering(true);
            let info = vk::DeviceCreateInfo::default()
                .queue_create_infos(&qi)
                .enabled_extension_names(&dnames)
                .push_next(&mut f13);
            let Ok(device) = instance.create_device(pd, &info, None) else { continue };
            let queue = device.get_device_queue(family as u32, 0);
            let mem = instance.get_physical_device_memory_properties(pd);
            return Some(Gpu { entry_keep: entry, instance, pd, device, queue, family: family as u32, mem });
        }
        None
    }
}

struct Buf {
    buf: vk::Buffer,
    mem: vk::DeviceMemory,
}

struct Img {
    img: vk::Image,
    view: vk::ImageView,
    mem: vk::DeviceMemory,
}

impl Gpu {
    fn find_mem(&self, bits: u32, want: vk::MemoryPropertyFlags) -> u32 {
        (0..self.mem.memory_type_count)
            .find(|&i| bits & (1 << i) != 0 && self.mem.memory_types[i as usize].property_flags.contains(want))
            .expect("memory type")
    }

    unsafe fn buffer(&self, size: u64, usage: vk::BufferUsageFlags) -> Buf {
        let d = &self.device;
        let buf = d.create_buffer(&vk::BufferCreateInfo::default().size(size).usage(usage), None).unwrap();
        let r = d.get_buffer_memory_requirements(buf);
        let mem = d
            .allocate_memory(
                &vk::MemoryAllocateInfo::default().allocation_size(r.size).memory_type_index(self.find_mem(
                    r.memory_type_bits,
                    vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
                )),
                None,
            )
            .unwrap();
        d.bind_buffer_memory(buf, mem, 0).unwrap();
        Buf { buf, mem }
    }

    unsafe fn image(&self, fmt: vk::Format, w: u32, h: u32, usage: vk::ImageUsageFlags, aspect: vk::ImageAspectFlags) -> Img {
        let d = &self.device;
        let img = d
            .create_image(
                &vk::ImageCreateInfo::default()
                    .image_type(vk::ImageType::TYPE_2D)
                    .format(fmt)
                    .extent(vk::Extent3D { width: w, height: h, depth: 1 })
                    .mip_levels(1)
                    .array_layers(1)
                    .samples(vk::SampleCountFlags::TYPE_1)
                    .usage(usage),
                None,
            )
            .unwrap();
        let r = d.get_image_memory_requirements(img);
        let mem = d
            .allocate_memory(
                &vk::MemoryAllocateInfo::default()
                    .allocation_size(r.size)
                    .memory_type_index(self.find_mem(r.memory_type_bits, vk::MemoryPropertyFlags::DEVICE_LOCAL)),
                None,
            )
            .unwrap();
        d.bind_image_memory(img, mem, 0).unwrap();
        let view = d
            .create_image_view(
                &vk::ImageViewCreateInfo::default()
                    .image(img)
                    .view_type(vk::ImageViewType::TYPE_2D)
                    .format(fmt)
                    .subresource_range(vk::ImageSubresourceRange {
                        aspect_mask: aspect,
                        base_mip_level: 0,
                        level_count: 1,
                        base_array_layer: 0,
                        layer_count: 1,
                    }),
                None,
            )
            .unwrap();
        Img { img, view, mem }
    }
}

fn barrier(
    img: vk::Image,
    aspect: vk::ImageAspectFlags,
    old: vk::ImageLayout,
    new: vk::ImageLayout,
    src: vk::AccessFlags,
    dst: vk::AccessFlags,
) -> vk::ImageMemoryBarrier<'static> {
    vk::ImageMemoryBarrier::default()
        .src_access_mask(src)
        .dst_access_mask(dst)
        .old_layout(old)
        .new_layout(new)
        .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
        .image(img)
        .subresource_range(vk::ImageSubresourceRange {
            aspect_mask: aspect,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: 0,
            layer_count: 1,
        })
}

/// Runs fs_god for each case (mask on a depth texture `scale` times 128 across, parameters) and returns the 128x128
/// RGBA32F output of each.
fn run_gpu(gpu: &Gpu, cases: &[(&Mask, &Params)], scale: u32) -> Vec<Vec<[f32; 3]>> {
    unsafe {
        let d = &gpu.device;
        let spv = n1ffi::shaders::spirv(&n1ffi::shaders::post_source()).expect("post.wgsl compiles");
        let module = d.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&spv), None).unwrap();
        let fr = vk::ShaderStageFlags::FRAGMENT;
        let bind = |b, ty| {
            vk::DescriptorSetLayoutBinding::default().binding(b).descriptor_type(ty).descriptor_count(1).stage_flags(fr)
        };
        let types = [
            vk::DescriptorType::UNIFORM_BUFFER,
            vk::DescriptorType::SAMPLER,
            vk::DescriptorType::SAMPLED_IMAGE,
            vk::DescriptorType::SAMPLED_IMAGE,
            vk::DescriptorType::SAMPLED_IMAGE,
            vk::DescriptorType::SAMPLED_IMAGE,
            vk::DescriptorType::SAMPLER,
            vk::DescriptorType::SAMPLED_IMAGE,
            vk::DescriptorType::SAMPLED_IMAGE,
        ];
        let binds: Vec<_> = types.iter().enumerate().map(|(i, t)| bind(i as u32, *t)).collect();
        let dsl = d.create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&binds), None).unwrap();
        let dsls = [dsl];
        let layout = d.create_pipeline_layout(&vk::PipelineLayoutCreateInfo::default().set_layouts(&dsls), None).unwrap();
        let out_fmt = vk::Format::R32G32B32A32_SFLOAT;
        let stages = [
            vk::PipelineShaderStageCreateInfo::default().stage(vk::ShaderStageFlags::VERTEX).module(module).name(c"vs"),
            vk::PipelineShaderStageCreateInfo::default().stage(fr).module(module).name(c"fs_god"),
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
        let fmts = [out_fmt];
        let mut rendering = vk::PipelineRenderingCreateInfo::default().color_attachment_formats(&fmts);
        let pinfo = vk::GraphicsPipelineCreateInfo::default()
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
        let pipe = d.create_graphics_pipelines(vk::PipelineCache::null(), &[pinfo], None).map_err(|e| e.1).unwrap()[0];

        let smp = d
            .create_sampler(
                &vk::SamplerCreateInfo::default()
                    .mag_filter(vk::Filter::LINEAR)
                    .min_filter(vk::Filter::LINEAR)
                    .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
                    .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
                    .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE),
                None,
            )
            .unwrap();
        let dim = GRID as u32 * scale;
        let depth = gpu.image(
            vk::Format::D32_SFLOAT,
            dim,
            dim,
            vk::ImageUsageFlags::SAMPLED | vk::ImageUsageFlags::TRANSFER_DST,
            vk::ImageAspectFlags::DEPTH,
        );
        let dummy = gpu.image(
            vk::Format::R16G16B16A16_SFLOAT,
            1,
            1,
            vk::ImageUsageFlags::SAMPLED | vk::ImageUsageFlags::TRANSFER_DST,
            vk::ImageAspectFlags::COLOR,
        );
        let target = gpu.image(
            out_fmt,
            GRID as u32,
            GRID as u32,
            vk::ImageUsageFlags::COLOR_ATTACHMENT | vk::ImageUsageFlags::TRANSFER_SRC,
            vk::ImageAspectFlags::COLOR,
        );
        let depth_stage = gpu.buffer((dim * dim * 4) as u64, vk::BufferUsageFlags::TRANSFER_SRC);
        let ubo = gpu.buffer(POST_BYTES, vk::BufferUsageFlags::UNIFORM_BUFFER);
        let readback = gpu.buffer((GRID * GRID * 16) as u64, vk::BufferUsageFlags::TRANSFER_DST);

        let pool = d
            .create_descriptor_pool(
                &vk::DescriptorPoolCreateInfo::default().max_sets(1).pool_sizes(&[
                    vk::DescriptorPoolSize { ty: vk::DescriptorType::UNIFORM_BUFFER, descriptor_count: 1 },
                    vk::DescriptorPoolSize { ty: vk::DescriptorType::SAMPLER, descriptor_count: 2 },
                    vk::DescriptorPoolSize { ty: vk::DescriptorType::SAMPLED_IMAGE, descriptor_count: 6 },
                ]),
                None,
            )
            .unwrap();
        let set = d.allocate_descriptor_sets(&vk::DescriptorSetAllocateInfo::default().descriptor_pool(pool).set_layouts(&dsls)).unwrap()[0];
        let ub = [vk::DescriptorBufferInfo { buffer: ubo.buf, offset: 0, range: POST_BYTES }];
        let sm = [vk::DescriptorImageInfo { sampler: smp, image_view: vk::ImageView::null(), image_layout: vk::ImageLayout::UNDEFINED }];
        let img = |v| [vk::DescriptorImageInfo { sampler: vk::Sampler::null(), image_view: v, image_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL }];
        let (di, du) = (img(dummy.view), [vk::DescriptorImageInfo {
            sampler: vk::Sampler::null(),
            image_view: depth.view,
            image_layout: vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        }]);
        fn wimg<'a>(set: vk::DescriptorSet, b: u32, info: &'a [vk::DescriptorImageInfo]) -> vk::WriteDescriptorSet<'a> {
            vk::WriteDescriptorSet::default().dst_set(set).dst_binding(b).descriptor_type(vk::DescriptorType::SAMPLED_IMAGE).image_info(info)
        }
        let writes = [
            vk::WriteDescriptorSet::default().dst_set(set).dst_binding(0).descriptor_type(vk::DescriptorType::UNIFORM_BUFFER).buffer_info(&ub),
            vk::WriteDescriptorSet::default().dst_set(set).dst_binding(1).descriptor_type(vk::DescriptorType::SAMPLER).image_info(&sm),
            vk::WriteDescriptorSet::default().dst_set(set).dst_binding(6).descriptor_type(vk::DescriptorType::SAMPLER).image_info(&sm),
            wimg(set, 2, &di),
            wimg(set, 3, &di),
            wimg(set, 4, &di),
            wimg(set, 5, &du),
            wimg(set, 7, &di),
            wimg(set, 8, &di),
        ];
        d.update_descriptor_sets(&writes, &[]);

        let cpool = d
            .create_command_pool(&vk::CommandPoolCreateInfo::default().queue_family_index(gpu.family), None)
            .unwrap();
        let cmd = d
            .allocate_command_buffers(&vk::CommandBufferAllocateInfo::default().command_pool(cpool).command_buffer_count(1))
            .unwrap()[0];
        let fence = d.create_fence(&vk::FenceCreateInfo::default(), None).unwrap();

        let mut results = vec![];
        let mut first = true;
        for (mask, p) in cases {
            // Depth texture: texel (x, y) of the mask covers scale x scale depth texels.
            let ptr = d.map_memory(depth_stage.mem, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty()).unwrap() as *mut f32;
            for y in 0..dim as usize {
                for x in 0..dim as usize {
                    let m = mask.0[(y / scale as usize) * GRID + x / scale as usize];
                    *ptr.add(y * dim as usize + x) = if m > 0.0 { 0.0 } else { 1.0 };
                }
            }
            d.unmap_memory(depth_stage.mem);
            // P: g0 = light, magnitude, decay; g1 = colour, exposure; dof2.z = near plane 1; comp.x = sky distance 5e4.
            let mut blk = [0f32; POST_FLOATS];
            blk[4 * 2 + 2] = 1.0; // dof2.z
            blk[4 * 4] = 5.0e4; // comp.x
            blk[4 * 5..4 * 5 + 4].copy_from_slice(&[p.light[0], p.light[1], p.magnitude, p.decay]);
            blk[4 * 6..4 * 6 + 4].copy_from_slice(&[p.color[0], p.color[1], p.color[2], p.exposure]);
            let up = d.map_memory(ubo.mem, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty()).unwrap() as *mut f32;
            std::ptr::copy_nonoverlapping(blk.as_ptr(), up, blk.len());
            d.unmap_memory(ubo.mem);

            d.reset_command_buffer(cmd, vk::CommandBufferResetFlags::empty()).unwrap();
            d.begin_command_buffer(cmd, &vk::CommandBufferBeginInfo::default().flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT)).unwrap();
            let all = vk::PipelineStageFlags::ALL_COMMANDS;
            let dep = vk::ImageAspectFlags::DEPTH;
            let col = vk::ImageAspectFlags::COLOR;
            let l = vk::ImageLayout::UNDEFINED;
            let mut pre = vec![
                barrier(depth.img, dep, if first { l } else { vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL }, vk::ImageLayout::TRANSFER_DST_OPTIMAL, vk::AccessFlags::MEMORY_READ | vk::AccessFlags::MEMORY_WRITE, vk::AccessFlags::TRANSFER_WRITE),
                barrier(target.img, col, l, vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL, vk::AccessFlags::empty(), vk::AccessFlags::COLOR_ATTACHMENT_WRITE),
            ];
            if first {
                pre.push(barrier(dummy.img, col, l, vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL, vk::AccessFlags::empty(), vk::AccessFlags::SHADER_READ));
            }
            d.cmd_pipeline_barrier(cmd, all, all, vk::DependencyFlags::empty(), &[], &[], &pre);
            d.cmd_copy_buffer_to_image(
                cmd,
                depth_stage.buf,
                depth.img,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                &[vk::BufferImageCopy::default()
                    .image_subresource(vk::ImageSubresourceLayers { aspect_mask: dep, mip_level: 0, base_array_layer: 0, layer_count: 1 })
                    .image_extent(vk::Extent3D { width: dim, height: dim, depth: 1 })],
            );
            d.cmd_pipeline_barrier(
                cmd, all, all, vk::DependencyFlags::empty(), &[], &[],
                &[barrier(depth.img, dep, vk::ImageLayout::TRANSFER_DST_OPTIMAL, vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL, vk::AccessFlags::TRANSFER_WRITE, vk::AccessFlags::SHADER_READ)],
            );
            let rect = vk::Rect2D { offset: vk::Offset2D { x: 0, y: 0 }, extent: vk::Extent2D { width: GRID as u32, height: GRID as u32 } };
            let catt = [vk::RenderingAttachmentInfo::default()
                .image_view(target.view)
                .image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL)
                .load_op(vk::AttachmentLoadOp::DONT_CARE)
                .store_op(vk::AttachmentStoreOp::STORE)];
            d.cmd_begin_rendering(cmd, &vk::RenderingInfo::default().render_area(rect).layer_count(1).color_attachments(&catt));
            d.cmd_set_viewport(cmd, 0, &[vk::Viewport { x: 0.0, y: 0.0, width: GRID as f32, height: GRID as f32, min_depth: 0.0, max_depth: 1.0 }]);
            d.cmd_set_scissor(cmd, 0, &[rect]);
            d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pipe);
            d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::GRAPHICS, layout, 0, &[set], &[]);
            d.cmd_draw(cmd, 3, 1, 0, 0);
            d.cmd_end_rendering(cmd);
            d.cmd_pipeline_barrier(
                cmd, all, all, vk::DependencyFlags::empty(), &[], &[],
                &[barrier(target.img, col, vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL, vk::ImageLayout::TRANSFER_SRC_OPTIMAL, vk::AccessFlags::COLOR_ATTACHMENT_WRITE, vk::AccessFlags::TRANSFER_READ)],
            );
            d.cmd_copy_image_to_buffer(
                cmd,
                target.img,
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                readback.buf,
                &[vk::BufferImageCopy::default()
                    .image_subresource(vk::ImageSubresourceLayers { aspect_mask: col, mip_level: 0, base_array_layer: 0, layer_count: 1 })
                    .image_extent(vk::Extent3D { width: GRID as u32, height: GRID as u32, depth: 1 })],
            );
            d.end_command_buffer(cmd).unwrap();
            let cmds = [cmd];
            d.queue_submit(gpu.queue, &[vk::SubmitInfo::default().command_buffers(&cmds)], fence).unwrap();
            d.wait_for_fences(&[fence], true, u64::MAX).unwrap();
            d.reset_fences(&[fence]).unwrap();
            first = false;
            let rp = d.map_memory(readback.mem, 0, vk::WHOLE_SIZE, vk::MemoryMapFlags::empty()).unwrap() as *const f32;
            let px: Vec<[f32; 3]> = (0..GRID * GRID).map(|i| [*rp.add(i * 4), *rp.add(i * 4 + 1), *rp.add(i * 4 + 2)]).collect();
            d.unmap_memory(readback.mem);
            results.push(px);
        }
        d.device_wait_idle().unwrap();
        results
    }
}

fn params() -> Params {
    Params { light: [0.93, 0.87], magnitude: 0.025, decay: 0.025, color: [0.97, 0.69, 0.31], exposure: 0.5 }
}

fn hash(x: u32, y: u32) -> f32 {
    let mut h = x.wrapping_mul(374761393).wrapping_add(y.wrapping_mul(668265263)).wrapping_add(7);
    h = (h ^ (h >> 13)).wrapping_mul(1274126177);
    ((h ^ (h >> 16)) >> 8) as f32 / (1u32 << 24) as f32
}

fn read_pgm(path: &str) -> Option<Mask> {
    let b = std::fs::read(path).ok()?;
    // "P5\n128 128\n255\n" then 16384 bytes
    let body = b.len().checked_sub(GRID * GRID)?;
    Some(Mask::new(|x, y| f32::from(b[body + y * GRID + x]) / 255.0))
}

fn stats(a: &[[f32; 3]], b: &[[f32; 3]]) -> (f32, f32, usize) {
    let mut max = 0f32;
    let mut sum = 0f64;
    let mut bad = 0;
    for (p, q) in a.iter().zip(b) {
        let e = (0..3).map(|c| (p[c] - q[c]).abs()).fold(0.0, f32::max);
        max = max.max(e);
        sum += e as f64;
        if e > 1e-3 {
            bad += 1;
        }
    }
    (max, (sum / a.len() as f64) as f32, bad)
}

#[test]
fn shader_matches_reference() {
    let Some(gpu) = init() else {
        eprintln!("godrays_gpu: no Vulkan 1.3 device, skipping");
        return;
    };
    let _ = (&gpu.entry_keep, gpu.pd, &gpu.instance);
    let blocks = Mask::new(|x, y| if (x / 9 + y / 7) % 3 == 0 { 0.0 } else { 1.0 });
    let noise = Mask::new(|x, y| if hash(x as u32, y as u32) > 0.6 { 1.0 } else { 0.0 });
    let half = Mask::new(|_, y| if y < 50 { 1.0 } else { 0.0 });
    let disc = Mask::new(|x, y| {
        let (dx, dy) = (x as f32 - 90.0, y as f32 - 80.0);
        if dx * dx + dy * dy < 30.0 * 30.0 { 0.0 } else { 1.0 }
    });
    let full = Mask::new(|_, _| 1.0);
    let mut cases: Vec<(String, Mask, Params)> = vec![];
    let lights = [[0.93, 0.87], [0.5, 0.5], [1.4, -0.3], [-0.2, 0.4], [0.0, 0.0], [0.7, 1.0]];
    for (name, m) in [("blocks", &blocks), ("noise", &noise), ("half", &half), ("disc", &disc), ("full", &full)] {
        for l in lights {
            cases.push((format!("{name} L=({:.2},{:.2})", l[0], l[1]), m.clone(), Params { light: l, ..params() }));
        }
    }
    if let Some(m) = std::env::var("N1_GOD_MASK").ok().and_then(|p| read_pgm(&p)) {
        for l in [[0.93, 0.87], [0.5, 0.2], [1.2, 0.6]] {
            cases.push((format!("captured L=({:.2},{:.2})", l[0], l[1]), m.clone(), Params { light: l, ..params() }));
        }
    }
    for scale in [1u32, 3] {
        let refs: Vec<&(String, Mask, Params)> = cases.iter().collect();
        let got = run_gpu(&gpu, &refs.iter().map(|c| (&c.1, &c.2)).collect::<Vec<_>>(), scale);
        let (mut worst, mut mean_sum, mut bad_sum) = (0f32, 0f32, 0);
        for (c, g) in cases.iter().zip(&got) {
            let want = godref::render(&c.1, &c.2);
            let (max, mean, bad) = stats(g, &want);
            eprintln!("scale {scale} {:<28} max {max:.2e} mean {mean:.2e} pixels over 1e-3: {bad}", c.0);
            worst = worst.max(max);
            mean_sum += mean;
            bad_sum += bad;
            // Measured 4e-8 (float rounding). A flipped predicate or mask step would be about 0.01.
            assert!(max < 1e-5, "{}: max error {max}", c.0);
            assert!(mean < 1e-6, "{}: mean error {mean}", c.0);
        }
        eprintln!(
            "scale {scale}: {} cases, worst max {worst:.2e}, mean of means {:.2e}, pixels over 1e-3 {bad_sum} of {}",
            cases.len(),
            mean_sum / cases.len() as f32,
            cases.len() * GRID * GRID
        );
    }
}
