//! The scene shader's PWL gamma decode (`xe_pwl3` in scene.wgsl, naga -> SPIR-V) on a GPU against the select form 1.15 to 1.18
//! shipped (`ab::PWL_LEGACY`), bit for bit over every f32 from 0 to 1 (1.06 billion inputs, three channels each, about a
//! second on a desktop GPU). The shipping form must report 0 mismatches; the other forms below are what was tried while
//! making it exact (printed, not asserted; `-- --nocapture`). Skips, passing, when no Vulkan 1.1 device loads;
//! N1_TEST_VULKAN=/path/to/libMoltenVK.dylib names the library to load.
#![allow(unsafe_op_in_unsafe_fn)] // a thin Vulkan harness: every call is unsafe, an extra block each adds nothing

use ash::vk;
use std::ffi::CStr;

const SCENE: &str = include_str!("../../matdef/src/scene.wgsl");
const ONE_BITS: u32 = 0x3F80_0000;
const CHUNK: u32 = 1 << 23;
const RECORDS: usize = 64;

/// Other decode forms, as `fn cand(c: vec3<f32>) -> vec3<f32>`, with why each was tried.
const CANDIDATES: &[(&str, &str)] = &[
    // !189: slopes folded into the 255 (s * 510 - 64): the GPU fuses the multiply-add into one rounding.
    ("189-folded", "fn cand(c: vec3<f32>) -> vec3<f32> {
    let s = saturate(c);
    let l = max(max(s * 255.0, s * 510.0 - 64.0), max(s * 1020.0 - 256.0, s * 2040.0 - 1024.0));
    let t = floor(l * select(vec3<f32>(1.0 / 256.0), vec3<f32>(1.0 / 128.0), l >= vec3<f32>(512.0)));
    return (l + t) * (1.0 / 1023.0);
}"),
    // The slopes pulled out as (g - 32) * 2 etc.: the same value (scaling by a power of two commutes with rounding).
    ("lines-minus-first", "fn cand(c: vec3<f32>) -> vec3<f32> {
    let g = saturate(c) * 255.0;
    let l = max(max(g, (g - 32.0) * 2.0), max((g - 64.0) * 4.0, (g - 128.0) * 8.0));
    let t = floor(l * select(vec3<f32>(1.0 / 256.0), vec3<f32>(1.0 / 128.0), l >= vec3<f32>(512.0)));
    return (l + t) * (1.0 / 1023.0);
}"),
    // The slope and offset from three 0/1 steps as arithmetic (k = 1 + a + 2b + 4d), then the old g * k + o.
    ("k-arith", "fn cand(c: vec3<f32>) -> vec3<f32> {
    let g = saturate(c) * 255.0;
    let a = select(vec3<f32>(0.0), vec3<f32>(1.0), g >= vec3<f32>(64.0));
    let b = select(vec3<f32>(0.0), vec3<f32>(1.0), g >= vec3<f32>(96.0));
    let d = select(vec3<f32>(0.0), vec3<f32>(1.0), g >= vec3<f32>(192.0));
    let k = 1.0 + a + 2.0 * b + 4.0 * d;
    let o = -64.0 * a - 192.0 * b - 768.0 * d;
    let l = g * k + o;
    return (l + trunc(l * k * (1.0 / 1024.0))) * (1.0 / 1023.0);
}"),
    // g capped at 255 (a no-op for g = 255 s <= 255) between the multiply and the four lines, to stop the GPU folding the
    // slope into the 255 (and fusing s * 510 - 64 into one rounding).
    ("min-barrier", "fn cand(c: vec3<f32>) -> vec3<f32> {
    let g = min(saturate(c) * 255.0, vec3<f32>(255.0));
    let l = max(max(g, g * 2.0 - 64.0), max(g * 4.0 - 256.0, g * 8.0 - 1024.0));
    let t = floor(l * select(vec3<f32>(1.0 / 256.0), vec3<f32>(1.0 / 128.0), l >= vec3<f32>(512.0)));
    return (l + t) * (1.0 / 1023.0);
}"),
    // The same with the round-down term as the old form had it: only the slope-folding of g is in question.
    ("min-barrier-fma", "fn cand(c: vec3<f32>) -> vec3<f32> {
    let g = min(saturate(c) * 255.0, vec3<f32>(255.0));
    let l = max(max(g, fma(g, vec3<f32>(2.0), vec3<f32>(-64.0))), max(fma(g, vec3<f32>(4.0), vec3<f32>(-256.0)), fma(g, vec3<f32>(8.0), vec3<f32>(-1024.0))));
    let t = floor(l * select(vec3<f32>(1.0 / 256.0), vec3<f32>(1.0 / 128.0), l >= vec3<f32>(512.0)));
    return (l + t) * (1.0 / 1023.0);
}"),
];

struct Gpu {
    _entry: ash::Entry,
    instance: ash::Instance,
    device: ash::Device,
    queue: vk::Queue,
    family: u32,
    mem: vk::PhysicalDeviceMemoryProperties,
    name: String,
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
        let app = vk::ApplicationInfo::default().api_version(vk::API_VERSION_1_1);
        let instance = entry
            .create_instance(&vk::InstanceCreateInfo::default().application_info(&app).enabled_extension_names(&names).flags(flags), None)
            .ok()?;
        for pd in instance.enumerate_physical_devices().ok()? {
            let props = instance.get_physical_device_properties(pd);
            let Some(family) = instance
                .get_physical_device_queue_family_properties(pd)
                .iter()
                .position(|q| q.queue_flags.contains(vk::QueueFlags::COMPUTE))
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
            let Ok(device) = instance.create_device(pd, &vk::DeviceCreateInfo::default().queue_create_infos(&qi).enabled_extension_names(&dnames), None)
            else {
                continue;
            };
            let queue = device.get_device_queue(family as u32, 0);
            let mem = instance.get_physical_device_memory_properties(pd);
            let name = props.device_name_as_c_str().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
            return Some(Gpu { _entry: entry, instance, device, queue, family: family as u32, mem, name });
        }
        None
    }
}

/// The compute shader: every f32 in [base, base + CHUNK) as three channels (c, the next float up, c with its low bit cleared),
/// decoded by `legacy` and by `cand`; any channel whose bits differ is counted and the first few recorded.
fn shader_source(cand: &str) -> String {
    format!(
        "{legacy}\n{cand}\n
struct Params {{ base: u32, last: u32 }}
@group(0) @binding(0) var<storage, read_write> out: array<atomic<u32>>;
@group(0) @binding(1) var<storage, read> prm: Params;

fn rec(bits: u32, ch: u32, a: f32, b: f32) {{
    let i = atomicAdd(&out[0], 1u);
    if (i < {RECORDS}u) {{
        atomicStore(&out[2u + i * 4u], bits);
        atomicStore(&out[3u + i * 4u], bitcast<u32>(a));
        atomicStore(&out[4u + i * 4u], bitcast<u32>(b));
        atomicStore(&out[5u + i * 4u], ch);
    }}
}}

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {{
    let bits = prm.base + id.x;
    if (bits > prm.last) {{ return; }}
    let c = vec3<f32>(bitcast<f32>(bits), bitcast<f32>(min(bits + 1u, {ONE_BITS}u)), bitcast<f32>(bits & 0xfffffffeu));
    let a = xe_pwl3_legacy(c);
    let b = cand(c);
    if (bitcast<u32>(a.x) != bitcast<u32>(b.x)) {{ rec(bits, 0u, a.x, b.x); }}
    if (bitcast<u32>(a.y) != bitcast<u32>(b.y)) {{ rec(bits, 1u, a.y, b.y); }}
    if (bitcast<u32>(a.z) != bitcast<u32>(b.z)) {{ rec(bits, 2u, a.z, b.z); }}
}}
",
        legacy = n1ffi::ab::PWL_LEGACY
    )
}

/// `xe_pwl3` as scene.wgsl ships it, renamed `cand`, without the A/B hook token.
fn shipping() -> String {
    let f = SCENE.split("fn xe_pwl3").nth(1).and_then(|s| s.split("\n}\n").next()).expect("xe_pwl3 in scene.wgsl");
    format!("fn cand{f}\n}}").replace("{{AB_PWL}}", "")
}

struct Run {
    mismatches: u32,
    first: Vec<(f32, u32, f32, f32)>, // input, channel, legacy, candidate
}

unsafe fn run_gpu(gpu: &Gpu, cand: &str) -> Run {
    let d = &gpu.device;
    let spv = n1ffi::shaders::spirv(&shader_source(cand)).unwrap_or_else(|e| panic!("shader: {e}"));
    let module = d.create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&spv), None).unwrap();
    let cs = vk::ShaderStageFlags::COMPUTE;
    let binds = [0, 1].map(|b| vk::DescriptorSetLayoutBinding::default().binding(b).descriptor_type(vk::DescriptorType::STORAGE_BUFFER).descriptor_count(1).stage_flags(cs));
    let dsl = d.create_descriptor_set_layout(&vk::DescriptorSetLayoutCreateInfo::default().bindings(&binds), None).unwrap();
    let dsls = [dsl];
    let layout = d.create_pipeline_layout(&vk::PipelineLayoutCreateInfo::default().set_layouts(&dsls), None).unwrap();
    let stage = vk::PipelineShaderStageCreateInfo::default().stage(cs).module(module).name(c"main");
    let pipe = d
        .create_compute_pipelines(vk::PipelineCache::null(), &[vk::ComputePipelineCreateInfo::default().stage(stage).layout(layout)], None)
        .map_err(|e| e.1)
        .unwrap()[0];

    let buffer = |size: u64| {
        let buf = d.create_buffer(&vk::BufferCreateInfo::default().size(size).usage(vk::BufferUsageFlags::STORAGE_BUFFER), None).unwrap();
        let r = d.get_buffer_memory_requirements(buf);
        let want = vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT;
        let ty = (0..gpu.mem.memory_type_count)
            .find(|&i| r.memory_type_bits & (1 << i) != 0 && gpu.mem.memory_types[i as usize].property_flags.contains(want))
            .expect("memory type");
        let mem = d.allocate_memory(&vk::MemoryAllocateInfo::default().allocation_size(r.size).memory_type_index(ty), None).unwrap();
        d.bind_buffer_memory(buf, mem, 0).unwrap();
        let ptr = d.map_memory(mem, 0, size, vk::MemoryMapFlags::empty()).unwrap() as *mut u32;
        (buf, ptr)
    };
    let out_words = 2 + RECORDS * 4;
    let (out_buf, out_ptr) = buffer((out_words * 4) as u64);
    let (prm_buf, prm_ptr) = buffer(16);
    std::ptr::write_bytes(out_ptr, 0, out_words);

    let pool = d
        .create_descriptor_pool(
            &vk::DescriptorPoolCreateInfo::default().max_sets(1).pool_sizes(&[vk::DescriptorPoolSize { ty: vk::DescriptorType::STORAGE_BUFFER, descriptor_count: 2 }]),
            None,
        )
        .unwrap();
    let set = d.allocate_descriptor_sets(&vk::DescriptorSetAllocateInfo::default().descriptor_pool(pool).set_layouts(&dsls)).unwrap()[0];
    let bi = [
        [vk::DescriptorBufferInfo { buffer: out_buf, offset: 0, range: vk::WHOLE_SIZE }],
        [vk::DescriptorBufferInfo { buffer: prm_buf, offset: 0, range: vk::WHOLE_SIZE }],
    ];
    let writes = [0, 1].map(|i| {
        vk::WriteDescriptorSet::default().dst_set(set).dst_binding(i as u32).descriptor_type(vk::DescriptorType::STORAGE_BUFFER).buffer_info(&bi[i])
    });
    d.update_descriptor_sets(&writes, &[]);
    let cpool = d.create_command_pool(&vk::CommandPoolCreateInfo::default().queue_family_index(gpu.family), None).unwrap();
    let cmd = d.allocate_command_buffers(&vk::CommandBufferAllocateInfo::default().command_pool(cpool).command_buffer_count(1)).unwrap()[0];
    let fence = d.create_fence(&vk::FenceCreateInfo::default(), None).unwrap();

    let mut base = 0u32;
    loop {
        let last = (base as u64 + CHUNK as u64 - 1).min(ONE_BITS as u64) as u32;
        *prm_ptr = base;
        *prm_ptr.add(1) = last;
        d.begin_command_buffer(cmd, &vk::CommandBufferBeginInfo::default()).unwrap();
        d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::COMPUTE, pipe);
        d.cmd_bind_descriptor_sets(cmd, vk::PipelineBindPoint::COMPUTE, layout, 0, &[set], &[]);
        d.cmd_dispatch(cmd, (last - base + 1).div_ceil(256), 1, 1);
        d.end_command_buffer(cmd).unwrap();
        let cmds = [cmd];
        d.queue_submit(gpu.queue, &[vk::SubmitInfo::default().command_buffers(&cmds)], fence).unwrap();
        d.wait_for_fences(&[fence], true, u64::MAX).unwrap();
        d.reset_fences(&[fence]).unwrap();
        if last == ONE_BITS {
            break;
        }
        base = last + 1;
    }
    let n = *out_ptr;
    let mut first = vec![];
    for i in 0..(n as usize).min(RECORDS) {
        let r = |k: usize| *out_ptr.add(2 + i * 4 + k);
        first.push((f32::from_bits(r(0)), r(3), f32::from_bits(r(1)), f32::from_bits(r(2))));
    }
    d.destroy_fence(fence, None);
    d.destroy_command_pool(cpool, None);
    d.destroy_descriptor_pool(pool, None);
    d.destroy_pipeline(pipe, None);
    d.destroy_pipeline_layout(layout, None);
    d.destroy_descriptor_set_layout(dsl, None);
    d.destroy_shader_module(module, None);
    d.destroy_buffer(out_buf, None);
    d.destroy_buffer(prm_buf, None);
    Run { mismatches: n, first }
}

#[test]
fn shipping_decode_is_bit_exact_on_the_gpu() {
    let Some(gpu) = init() else {
        eprintln!("pwl_gpu: no Vulkan device, skipped");
        return;
    };
    let mut all: Vec<(String, String)> = CANDIDATES.iter().map(|(n, s)| (n.to_string(), s.to_string())).collect();
    all.push(("shipping".to_string(), shipping()));
    let mut shipped = u32::MAX;
    for (name, src) in &all {
        let r = unsafe { run_gpu(&gpu, src) };
        eprintln!("pwl_gpu [{}] {name}: {} mismatching channels of 3.2 billion", gpu.name, r.mismatches);
        for (c, ch, a, b) in r.first.iter().take(6) {
            eprintln!("    c = {c:e} ({:#010x}) ch {ch}: legacy {a:e} ({:#010x}), this {b:e} ({:#010x})", c.to_bits(), a.to_bits(), b.to_bits());
        }
        if name == "shipping" {
            shipped = r.mismatches;
        }
    }
    unsafe {
        gpu.device.destroy_device(None);
        gpu.instance.destroy_instance(None);
    }
    assert_eq!(shipped, 0, "the shipping decode differs from 1.18.0's on {}", gpu.name);
}
