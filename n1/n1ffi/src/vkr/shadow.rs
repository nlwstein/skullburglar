//! B-189: the port's light-environment modulated shadows.
//!
//! UE3's DynamicLightEnvironment gives each character or mover a zero-brightness point light that casts a
//! modulated per-object shadow: a perspective depth map of the owner seen from that light, projected onto everything
//! inside the light's radius and multiplied into the scene colour (capture f1938: a 512x512 depth pass per shadow,
//! then a modulate-blended projection volume). Static geometry casts nothing here: its shadows are in the lightmaps.
//!
//! n1 renders one depth map per shadow into a layer of a `D32` array (casters = the opaque draws lit by that
//! environment), then the scene shader's `shadow_mod` (`shaders::SHADOW_WGSL`) multiplies the base, lightmap,
//! emissive and key-light terms by it; the environment's SH term is added after, as the port draws it after the
//! shadows. Each shadow's map size follows UE3's per-object rule (2 texels per screen pixel of the caster's bounds,
//! 32..512) inside its layer, and it fades in between 32 and 64 texels as UE3 fades small shadows.
//!
//! The pipelines (static and skinned depth-only) are made with the renderer, from the driver cache when warm, so
//! nothing is compiled in play. `BOTS_N1_SHADOW=0` turns it all off (the shader then skips its loop).

use super::*;
use gpu_allocator::vulkan::Allocator;

/// UE3 per-object shadow sizing (BaseEngine.ini [Engine.Engine]): MinShadowResolution, ShadowFadeResolution,
/// ShadowTexelsPerPixel, ModShadowFadeDistanceExponent.
const MIN_RES: f32 = 32.0;
const FADE_RES: f32 = 64.0;
const TEXELS_PER_PIXEL: f32 = 2.0;
const FADE_EXP: f32 = 0.2;

/// B-205: a screen rectangle that covers every pixel, for `BOTS_N1_SHADOW_MASK=0`.
const FULL_RECT: [f32; 4] = [-1e9, -1e9, 1e9, 1e9];

/// `Shadow` in `shaders::SHADOW_WGSL` (144 bytes).
#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable, Default)]
pub(super) struct GpuShadow {
    pub vp: [f32; 16],
    /// Light position (Y-up), 1/radius.
    pub pos: [f32; 4],
    /// ModShadowColor, ModShadowFadeoutExponent.
    pub col: [f32; 4],
    /// Fade scale, fade bias, ShadowFalloffExponent, texel world size per unit distance.
    pub par: [f32; 4],
    /// Layer, strength (screen-size fade), used part of the layer (0..1), 1/layer size.
    pub misc: [f32; 4],
    /// B-205: the screen rectangle (pixels: min x, min y, max x, max y) around the pixels this shadow can darken.
    pub rect: [f32; 4],
}

/// B-231: the volume a shadow can darken (the truncated pyramid of `hull_polygon`, inside the light's radius), for
/// the per-instance receiver masks (`Renderer::shadow_receivers`).
#[derive(Clone, Copy)]
pub(super) struct Cull {
    light: [f32; 3],
    radius: f32,
    f: [f32; 3],
    s: [f32; 3],
    u: [f32; 3],
    /// Pyramid depth range along `f` and its half-width at depth z: `z * t + off * (1 + t)`.
    z0: f32,
    t: f32,
    off: f32,
}

impl Cull {
    /// False only when no point of the sphere (`c`, `r`) can be darkened by this shadow (conservative).
    pub(super) fn touches(&self, c: [f32; 3], r: f32) -> bool {
        let v = sub(c, self.light);
        if dot(v, v) >= (self.radius + r) * (self.radius + r) {
            return false;
        }
        let z = dot(v, self.f);
        if z + r < self.z0 {
            return false;
        }
        // Distance past the pyramid's side planes (x = z t + off (1 + t)) is (x - z t - off (1 + t)) / sqrt(1 + t^2).
        let hw = z * self.t + self.off * (1.0 + self.t) + r * (1.0 + self.t * self.t).sqrt();
        dot(v, self.s).abs() <= hw && dot(v, self.u).abs() <= hw
    }
}

pub(super) struct Shadows {
    pub image: vk::Image,
    alloc: Option<Allocation>,
    pub array_view: vk::ImageView,
    layer_views: Vec<vk::ImageView>,
    pub sampler: vk::Sampler,
    pipe_layout: vk::PipelineLayout,
    module: vk::ShaderModule,
    pipe_static: vk::Pipeline,
    pipe_skin: vk::Pipeline,
    pub layers: u32,
    pub res: u32,
    /// The image's layers are in `DEPTH_STENCIL_READ_ONLY_OPTIMAL` (after the first frame's transition).
    ready: bool,
    /// This frame's shadows (GPU data) and, per shadow, the environment whose draws cast it and its map size.
    pub frame: Vec<GpuShadow>,
    casters: Vec<(u32, u32)>,
    /// B-231: per shadow of this frame, the volume it can darken.
    cull: Vec<Cull>,
    /// B-205: the screen fraction each of this frame's shadows' polygons covers (for the log).
    cover: Vec<f32>,
    /// Shadows drawn: this frame, and cumulative caster draws (for the log).
    pub frames_with: u64,
    pub caster_draws: u64,
}

fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
fn dot(a: [f32; 3], b: [f32; 3]) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}
fn norm(a: [f32; 3]) -> [f32; 3] {
    let l = dot(a, a).sqrt().max(1e-12);
    [a[0] / l, a[1] / l, a[2] / l]
}
/// Column-major a * b.
fn mul(a: &[f32; 16], b: &[f32; 16]) -> [f32; 16] {
    let mut o = [0f32; 16];
    for c in 0..4 {
        for r in 0..4 {
            o[c * 4 + r] = (0..4).map(|k| a[k * 4 + r] * b[c * 4 + k]).sum();
        }
    }
    o
}

/// Right-handed look-at (column-major), looking from `eye` along `f`.
fn look(eye: [f32; 3], f: [f32; 3]) -> [f32; 16] {
    let up = if f[1].abs() > 0.99 {
        [1.0, 0.0, 0.0]
    } else {
        [0.0, 1.0, 0.0]
    };
    let s = norm(cross(f, up));
    let u = cross(s, f);
    [
        s[0],
        u[0],
        -f[0],
        0.0, //
        s[1],
        u[1],
        -f[1],
        0.0, //
        s[2],
        u[2],
        -f[2],
        0.0, //
        -dot(s, eye),
        -dot(u, eye),
        dot(f, eye),
        1.0,
    ]
}

/// Perspective with depth 0 at `near`, 1 at `far` (standard Z: the shadow maps clear to 1 and test LESS_OR_EQUAL).
fn persp(half: f32, near: f32, far: f32) -> [f32; 16] {
    let t = 1.0 / half.tan();
    let mut p = [0f32; 16];
    p[0] = t;
    p[5] = t;
    p[10] = far / (near - far);
    p[11] = -1.0;
    p[14] = near * far / (near - far);
    p
}

/// One shadow's frustum: the caster sphere seen from the light. None when the light is inside the casters.
pub(super) struct Fit {
    pub vp: [f32; 16],
    pub half: f32,
    pub near: f32,
    /// The light's view axes: forward (towards the casters), right, up.
    pub f: [f32; 3],
    pub s: [f32; 3],
    pub u: [f32; 3],
}

pub(super) fn fit(light: [f32; 3], center: [f32; 3], radius: f32) -> Option<Fit> {
    let to = sub(center, light);
    let d = dot(to, to).sqrt();
    if d <= radius * 1.05 {
        return None;
    }
    let half = ((radius / d).asin() * 1.05).min(1.4);
    let near = (d - radius * 1.1).max(1.0);
    let far = d + radius * 1.1;
    let f = norm(to);
    let view = look(light, f);
    Some(Fit {
        vp: mul(&persp(half, near, far), &view),
        half,
        near,
        f,
        s: [view[0], view[4], view[8]],
        u: [view[1], view[5], view[9]],
    })
}

/// B-205: the screen polygon (NDC, the scene shaders' Y-up convention, as a triangle list) holding every pixel
/// `shadow_mod` can darken, or None when no pixel on screen can be. Such a point is within `radius` of the light and,
/// after the shader's receiver offset (at most `offset` world units), inside the shadow frustum beyond its near
/// plane. So it lies in the truncated pyramid along the light's axis from `near - offset` to `radius`, whose
/// half-width at depth z is `z tan(half) + offset (1 + tan(half))`. The polygon is that pyramid clipped to the
/// camera's `w >= min_w` (visible points have w at least the near plane), projected, grown by 1.5 pixels and clipped to
/// the `w` x `h` target plus 2 pixels. Conservative: the shader would `continue` for every pixel outside it.
pub(super) fn hull_polygon(
    fit: &Fit,
    light: [f32; 3],
    radius: f32,
    offset: f32,
    vp: &[f32; 16],
    min_w: f64,
    w: u32,
    h: u32,
) -> Option<Vec<[f32; 2]>> {
    let t = fit.half.tan() as f64 * 1.001;
    let off = offset as f64 * 1.001 + 0.01;
    let z0 = (fit.near as f64 - off).max(0.0);
    let z1 = radius as f64 * 1.001;
    if z1 <= z0 {
        return None;
    }
    let mut corners = [[0f64; 4]; 8];
    for (k, c) in corners.iter_mut().enumerate() {
        let z = if k & 4 == 0 { z0 } else { z1 };
        let hw = z * t + off * (1.0 + t);
        let sx = if k & 1 == 0 { -hw } else { hw };
        let sy = if k & 2 == 0 { -hw } else { hw };
        let p = [0, 1, 2].map(|i| {
            light[i] as f64 + fit.f[i] as f64 * z + fit.s[i] as f64 * sx + fit.u[i] as f64 * sy
        });
        for (r, v) in c.iter_mut().enumerate() {
            *v = vp[r] as f64 * p[0]
                + vp[4 + r] as f64 * p[1]
                + vp[8 + r] as f64 * p[2]
                + vp[12 + r] as f64;
        }
    }
    // The clipped pyramid's vertices: corners in front, and where box edges (corners one bit apart) cross w = min_w.
    // Projected to pixel units from the centre (x right, y up).
    let (hw, hh) = (w as f64 * 0.5, h as f64 * 0.5);
    let mut pts: Vec<[f64; 2]> = Vec::with_capacity(20);
    let mut take = |c: [f64; 4]| pts.push([c[0] / c[3] * hw, c[1] / c[3] * hh]);
    for (a, ca) in corners.iter().enumerate() {
        if ca[3] >= min_w {
            take(*ca);
        }
        for bit in [1usize, 2, 4] {
            let cb = corners[a | bit];
            if a & bit == 0 && (ca[3] >= min_w) != (cb[3] >= min_w) {
                let s = (min_w - ca[3]) / (cb[3] - ca[3]);
                take([0, 1, 2, 3].map(|i| ca[i] + (cb[i] - ca[i]) * s));
            }
        }
    }
    let poly = convex_hull(pts);
    if poly.len() < 3 {
        return None;
    }
    // Grow by 1.5 pixels: each vertex pushed out along both adjacent edge normals (a bevelled offset), then the hull.
    let n = poly.len();
    let mut grown = Vec::with_capacity(2 * n);
    for i in 0..n {
        let (p, q, r) = (poly[(i + n - 1) % n], poly[i], poly[(i + 1) % n]);
        for (a, b) in [(p, q), (q, r)] {
            let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
            let l = (dx * dx + dy * dy).sqrt().max(1e-12);
            grown.push([q[0] + dy / l * 1.5, q[1] - dx / l * 1.5]);
        }
    }
    let mut poly = convex_hull(grown);
    // Clip to the target plus 2 pixels (Sutherland-Hodgman against the four sides).
    for (axis, lim, keep_below) in [
        (0, hw + 2.0, true),
        (0, -hw - 2.0, false),
        (1, hh + 2.0, true),
        (1, -hh - 2.0, false),
    ] {
        let inside = |p: &[f64; 2]| {
            if keep_below {
                p[axis] <= lim
            } else {
                p[axis] >= lim
            }
        };
        let mut out = Vec::with_capacity(poly.len() + 2);
        for i in 0..poly.len() {
            let (a, b) = (poly[i], poly[(i + 1) % poly.len()]);
            if inside(&a) {
                out.push(a);
            }
            if inside(&a) != inside(&b) {
                let s = (lim - a[axis]) / (b[axis] - a[axis]);
                out.push([a[0] + (b[0] - a[0]) * s, a[1] + (b[1] - a[1]) * s]);
            }
        }
        poly = out;
        if poly.len() < 3 {
            return None;
        }
    }
    let ndc = |p: [f64; 2]| [(p[0] / hw) as f32, (p[1] / hh) as f32];
    let mut tris = Vec::with_capacity(3 * (poly.len() - 2));
    for i in 1..poly.len() - 1 {
        tris.extend([ndc(poly[0]), ndc(poly[i]), ndc(poly[i + 1])]);
    }
    Some(tris)
}

/// Convex hull (Andrew's monotone chain), counter-clockwise, without collinear points.
fn convex_hull(mut p: Vec<[f64; 2]>) -> Vec<[f64; 2]> {
    p.retain(|q| q[0].is_finite() && q[1].is_finite());
    p.sort_by(|a, b| a[0].total_cmp(&b[0]).then(a[1].total_cmp(&b[1])));
    p.dedup();
    if p.len() < 3 {
        return p;
    }
    let cross = |o: [f64; 2], a: [f64; 2], b: [f64; 2]| {
        (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    };
    let mut hull: Vec<[f64; 2]> = Vec::with_capacity(2 * p.len());
    for pass in 0..2 {
        let start = hull.len();
        let iter: Box<dyn Iterator<Item = &[f64; 2]>> = if pass == 0 {
            Box::new(p.iter())
        } else {
            Box::new(p.iter().rev())
        };
        for &q in iter {
            while hull.len() >= start + 2
                && cross(hull[hull.len() - 2], hull[hull.len() - 1], q) <= 0.0
            {
                hull.pop();
            }
            hull.push(q);
        }
        hull.pop();
    }
    hull
}

impl Shadows {
    /// # Safety
    /// `device` must be valid; `set0` is the scene's set 0 layout.
    pub unsafe fn new(
        device: &ash::Device,
        allocator: &mut Allocator,
        set0: vk::DescriptorSetLayout,
        cache: vk::PipelineCache,
        knobs: &Knobs,
    ) -> Result<Shadows> {
        unsafe {
            // Off: a 1x1 map, still bound (the scene shader declares it), never drawn.
            let (layers, res) = if knobs.shadows {
                (knobs.shadow_max, knobs.shadow_res)
            } else {
                (1, 1)
            };
            let image = device.create_image(
                &vk::ImageCreateInfo::default()
                    .image_type(vk::ImageType::TYPE_2D)
                    .format(DEPTH_FORMAT)
                    .extent(vk::Extent3D {
                        width: res,
                        height: res,
                        depth: 1,
                    })
                    .mip_levels(1)
                    .array_layers(layers)
                    .samples(vk::SampleCountFlags::TYPE_1)
                    .tiling(vk::ImageTiling::OPTIMAL)
                    .usage(
                        vk::ImageUsageFlags::DEPTH_STENCIL_ATTACHMENT
                            | vk::ImageUsageFlags::SAMPLED,
                    ),
                None,
            )?;
            let requirements = device.get_image_memory_requirements(image);
            let alloc = allocator
                .allocate(&AllocationCreateDesc {
                    name: "n1 shadows",
                    requirements,
                    location: MemoryLocation::GpuOnly,
                    linear: false,
                    allocation_scheme: AllocationScheme::GpuAllocatorManaged,
                })
                .map_err(|e| anyhow!("allocate shadows: {e}"))?;
            device.bind_image_memory(image, alloc.memory(), alloc.offset())?;
            let range = |base, count| vk::ImageSubresourceRange {
                aspect_mask: vk::ImageAspectFlags::DEPTH,
                base_mip_level: 0,
                level_count: 1,
                base_array_layer: base,
                layer_count: count,
            };
            let view = |ty, r| {
                device.create_image_view(
                    &vk::ImageViewCreateInfo::default()
                        .image(image)
                        .view_type(ty)
                        .format(DEPTH_FORMAT)
                        .subresource_range(r),
                    None,
                )
            };
            let array_view = view(vk::ImageViewType::TYPE_2D_ARRAY, range(0, layers))?;
            let mut layer_views = Vec::new();
            for k in 0..layers {
                layer_views.push(view(vk::ImageViewType::TYPE_2D, range(k, 1))?);
            }
            let sampler = device.create_sampler(
                &vk::SamplerCreateInfo::default()
                    .mag_filter(vk::Filter::LINEAR)
                    .min_filter(vk::Filter::LINEAR)
                    .mipmap_mode(vk::SamplerMipmapMode::NEAREST)
                    .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
                    .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
                    .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE)
                    .compare_enable(true)
                    .compare_op(vk::CompareOp::LESS_OR_EQUAL)
                    .max_lod(0.0),
                None,
            )?;
            let sets = [set0];
            let pcr = [vk::PushConstantRange {
                stage_flags: vk::ShaderStageFlags::VERTEX,
                offset: 0,
                size: 4,
            }];
            let pipe_layout = device.create_pipeline_layout(
                &vk::PipelineLayoutCreateInfo::default()
                    .set_layouts(&sets)
                    .push_constant_ranges(&pcr),
                None,
            )?;
            let words = shaders::spirv_caps(
                shaders::SHADOW_DEPTH_WGSL,
                naga::valid::Capabilities::IMMEDIATES,
            )?;
            let module = device
                .create_shader_module(&vk::ShaderModuleCreateInfo::default().code(&words), None)?;
            let mk = |skin: bool| -> Result<vk::Pipeline> {
                let stages = [vk::PipelineShaderStageCreateInfo::default()
                    .stage(vk::ShaderStageFlags::VERTEX)
                    .module(module)
                    .name(if skin { c"vs_skin" } else { c"vs_static" })];
                let stride = if skin {
                    std::mem::size_of::<SkinVertex>()
                } else {
                    std::mem::size_of::<Vertex>()
                } as u32;
                let bindings = [vk::VertexInputBindingDescription {
                    binding: 0,
                    stride,
                    input_rate: vk::VertexInputRate::VERTEX,
                }];
                let attr = |location, format, offset| vk::VertexInputAttributeDescription {
                    location,
                    binding: 0,
                    format,
                    offset,
                };
                let attrs: Vec<vk::VertexInputAttributeDescription> = if skin {
                    vec![
                        attr(0, vk::Format::R32G32B32_SFLOAT, 0),
                        attr(3, vk::Format::R8G8B8A8_UINT, 32),
                        attr(4, vk::Format::R8G8B8A8_UNORM, 36),
                    ]
                } else {
                    vec![attr(0, vk::Format::R32G32B32_SFLOAT, 0)]
                };
                let vi = vk::PipelineVertexInputStateCreateInfo::default()
                    .vertex_binding_descriptions(&bindings)
                    .vertex_attribute_descriptions(&attrs);
                let ia = vk::PipelineInputAssemblyStateCreateInfo::default()
                    .topology(vk::PrimitiveTopology::TRIANGLE_LIST);
                let vp = vk::PipelineViewportStateCreateInfo::default()
                    .viewport_count(1)
                    .scissor_count(1);
                // Both faces cast (no leaks through open meshes); slope bias against acne, plus the receiver's
                // normal offset in the scene shader.
                let rs = vk::PipelineRasterizationStateCreateInfo::default()
                    .polygon_mode(vk::PolygonMode::FILL)
                    .cull_mode(vk::CullModeFlags::NONE)
                    .depth_bias_enable(true)
                    .depth_bias_constant_factor(0.0)
                    .depth_bias_slope_factor(1.5)
                    .line_width(1.0);
                let ms = vk::PipelineMultisampleStateCreateInfo::default()
                    .rasterization_samples(vk::SampleCountFlags::TYPE_1);
                let ds = vk::PipelineDepthStencilStateCreateInfo::default()
                    .depth_test_enable(true)
                    .depth_write_enable(true)
                    .depth_compare_op(vk::CompareOp::LESS_OR_EQUAL);
                let cb = vk::PipelineColorBlendStateCreateInfo::default();
                let dyns = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
                let dy = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dyns);
                let mut rendering = vk::PipelineRenderingCreateInfo::default()
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
                    .layout(pipe_layout)
                    .push_next(&mut rendering);
                Ok(device
                    .create_graphics_pipelines(cache, &[info], None)
                    .map_err(|(_, e)| anyhow!("shadow pipeline: {e}"))?[0])
            };
            let pipe_static = mk(false)?;
            let pipe_skin = mk(true)?;
            if knobs.shadows {
                eprintln!(
                    "[n1] shadows: up to {layers} a frame, {res}x{res} (BOTS_N1_SHADOW=0 turns them off)"
                );
            }
            Ok(Shadows {
                image,
                alloc: Some(alloc),
                array_view,
                layer_views,
                sampler,
                pipe_layout,
                module,
                pipe_static,
                pipe_skin,
                layers,
                res,
                ready: false,
                frame: Vec::new(),
                casters: Vec::new(),
                cull: Vec::new(),
                cover: Vec::new(),
                frames_with: 0,
                caster_draws: 0,
            })
        }
    }

    /// # Safety
    /// The GPU must be done with the shadows.
    pub unsafe fn destroy(&mut self, device: &ash::Device, allocator: &mut Allocator) {
        unsafe {
            device.destroy_pipeline(self.pipe_static, None);
            device.destroy_pipeline(self.pipe_skin, None);
            device.destroy_shader_module(self.module, None);
            device.destroy_pipeline_layout(self.pipe_layout, None);
            device.destroy_sampler(self.sampler, None);
            for v in self.layer_views.drain(..) {
                device.destroy_image_view(v, None);
            }
            device.destroy_image_view(self.array_view, None);
            device.destroy_image(self.image, None);
            if let Some(a) = self.alloc.take() {
                let _ = allocator.free(a);
            }
        }
    }
}

/// Area of an NDC triangle list (the full screen is 4).
fn tri_area(t: &[[f32; 2]]) -> f32 {
    t.chunks(3)
        .map(|v| {
            ((v[1][0] - v[0][0]) * (v[2][1] - v[0][1]) - (v[2][0] - v[0][0]) * (v[1][1] - v[0][1]))
                .abs()
                * 0.5
        })
        .sum()
}

impl Renderer {
    /// Picks this frame's shadows: environments with a modulated shadow light and opaque draws lit by them, largest
    /// on screen first. `vp` is the Y-up view-projection, `vw` x `h` the view in pixels. B-205: each shadow gets the
    /// screen rectangle around the polygon it can darken (`hull_polygon`), and shadows that can't darken any pixel on screen are dropped
    /// (`BOTS_N1_SHADOW_MASK=0` turns both off: every shadow covers the whole screen, as before, for A/B).
    pub(super) fn select_shadows(&mut self, vp: &[f32; 16], vw: u32, h: u32) {
        let masks = self.knobs.shadow_mask;
        let min_w = (self.near as f64 * 0.5).clamp(1e-3, 1.0);
        let sh = &mut self.shadows;
        sh.frame.clear();
        sh.casters.clear();
        sh.cull.clear();
        if !self.knobs.shadows {
            return;
        }
        let envs = &self.lighting.dyn_frame.env_shadows;
        // Caster bounds per environment: union of the spheres of its instances that have an opaque draw.
        let mut bounds: HashMap<u32, ([f32; 3], f32)> = HashMap::new();
        let mut has_opaque = vec![false; self.gpu_insts.len()];
        for d in &self.draws {
            if d.key.blend == 0 {
                has_opaque[d.instance as usize] = true;
            }
        }
        for (i, &e) in self.inst_env.iter().enumerate() {
            if e == NONE || !has_opaque[i] || !matches!(envs.get(e as usize), Some(Some(_))) {
                continue;
            }
            let (c, r) = self.spheres[i];
            bounds
                .entry(e)
                .and_modify(|(bc, br)| {
                    let v = sub(c, *bc);
                    let dd = dot(v, v).sqrt();
                    if dd + r > *br {
                        if *br + dd <= r {
                            *bc = c;
                            *br = r;
                        } else {
                            let nr = (dd + r + *br) * 0.5;
                            let t = (nr - *br) / dd.max(1e-6);
                            *bc = [bc[0] + v[0] * t, bc[1] + v[1] * t, bc[2] + v[2] * t];
                            *br = nr;
                        }
                    }
                })
                .or_insert((c, r));
        }
        // Screen radius in pixels: |row 1| of the view-projection is the projection's y scale.
        let ys = (vp[1] * vp[1] + vp[5] * vp[5] + vp[9] * vp[9]).sqrt();
        let mut cand: Vec<(f32, u32, GpuShadow, f32, Cull)> = Vec::new();
        for (&e, &(c, r)) in &bounds {
            let s = envs[e as usize].unwrap();
            let w = vp[3] * c[0] + vp[7] * c[1] + vp[11] * c[2] + vp[15];
            let px = r * ys / w.max(1.0) * h as f32 * 0.5;
            let want = px * TEXELS_PER_PIXEL;
            if want <= MIN_RES {
                continue;
            }
            let strength = if want >= FADE_RES {
                1.0
            } else {
                ((want - MIN_RES) / (FADE_RES - MIN_RES)).powf(FADE_EXP)
            };
            let Some(ft) = fit(s.pos, c, r) else { continue };
            let size = want.min(sh.res as f32).max(MIN_RES).round();
            let texel = 2.0 * ft.half.tan() / size;
            let t = ft.half.tan() * 1.001;
            let off = 1.5 * texel * s.radius * 1.001 + 0.01;
            let cull = Cull {
                light: s.pos,
                radius: s.radius * 1.001,
                f: ft.f,
                s: ft.s,
                u: ft.u,
                z0: (ft.near - off).max(0.0),
                t,
                off,
            };
            // The shader offsets receivers by 1.5 texels (at their distance, which is under the radius).
            let (rect, cover) = if masks {
                let Some(t) = hull_polygon(
                    &ft,
                    s.pos,
                    s.radius,
                    1.5 * texel * s.radius,
                    vp,
                    min_w,
                    vw,
                    h,
                ) else {
                    continue; // darkens no pixel on screen: don't spend a layer on it
                };
                // Bounding box in pixels (NDC y up, framebuffer y down), one pixel of margin.
                let (mut lo, mut hi) = ([f32::MAX; 2], [f32::MIN; 2]);
                for p in &t {
                    let q = [
                        (p[0] * 0.5 + 0.5) * vw as f32,
                        (0.5 - p[1] * 0.5) * h as f32,
                    ];
                    for a in 0..2 {
                        lo[a] = lo[a].min(q[a]);
                        hi[a] = hi[a].max(q[a]);
                    }
                }
                (
                    [lo[0] - 1.0, lo[1] - 1.0, hi[0] + 1.0, hi[1] + 1.0],
                    tri_area(&t) / 4.0,
                )
            } else {
                (FULL_RECT, 1.0)
            };
            cand.push((
                px,
                e,
                GpuShadow {
                    vp: ft.vp,
                    pos: [s.pos[0], s.pos[1], s.pos[2], 1.0 / s.radius],
                    col: [s.color[0], s.color[1], s.color[2], s.fade_exp],
                    par: [s.scale, s.bias, s.falloff_exp, texel],
                    misc: [0.0, strength, size / sh.res as f32, 1.0 / sh.res as f32],
                    rect,
                },
                cover,
                cull,
            ));
        }
        cand.sort_by(|a, b| {
            b.0.partial_cmp(&a.0)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then(a.1.cmp(&b.1))
        });
        sh.cover.clear();
        for (k, (_, e, mut g, cover, cull)) in cand.into_iter().take(sh.layers as usize).enumerate()
        {
            sh.cull.push(cull);
            g.misc[0] = k as f32;
            let size = (g.misc[2] * sh.res as f32).round() as u32;
            sh.frame.push(g);
            sh.casters.push((e, size));
            sh.cover.push(cover);
        }
    }

    /// B-231: each instance's receiver mask (`GpuInstance::p2`, bit k = shadow k of this frame can darken some point
    /// of the instance's bounding sphere). The scene shader's `shadow_mod` visits only those shadows, so the pixels of
    /// draws no shadow reaches skip the loop. Conservative (`Cull::touches`): the picture is unchanged.
    /// `BOTS_N1_SHADOW_IMASK=0` sets every bit, as before, for A/B.
    pub(super) fn shadow_receivers(&mut self) {
        let n = self.shadows.frame.len().min(32);
        let all = if n == 0 { 0 } else { u32::MAX >> (32 - n) };
        let cull = &self.shadows.cull;
        let on = self.knobs.shadow_imask && cull.len() == n;
        for (i, g) in self.gpu_insts.iter_mut().enumerate() {
            g.p2 = if !on || n == 0 {
                all
            } else {
                let (c, r) = self.spheres[i];
                cull.iter().enumerate().fold(
                    0u32,
                    |m, (k, v)| if v.touches(c, r) { m | 1 << k } else { m },
                )
            };
        }
    }

    /// Records the shadow depth passes (set 0 must hold this frame's shadows at binding 7) and leaves every layer
    /// ready to sample. Returns the caster draws made.
    pub(super) fn record_shadows(
        &mut self,
        cmd: vk::CommandBuffer,
        set0: vk::DescriptorSet,
    ) -> u32 {
        let d = &self.device;
        let sh = &mut self.shadows;
        let range = |base, count| vk::ImageSubresourceRange {
            aspect_mask: vk::ImageAspectFlags::DEPTH,
            base_mip_level: 0,
            level_count: 1,
            base_array_layer: base,
            layer_count: count,
        };
        let bar = |old, new, src, dst, r| {
            vk::ImageMemoryBarrier::default()
                .src_access_mask(src)
                .dst_access_mask(dst)
                .old_layout(old)
                .new_layout(new)
                .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                .image(sh.image)
                .subresource_range(r)
        };
        let frag = vk::PipelineStageFlags::FRAGMENT_SHADER;
        let tests = vk::PipelineStageFlags::EARLY_FRAGMENT_TESTS
            | vk::PipelineStageFlags::LATE_FRAGMENT_TESTS;
        let ro = vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        let att = vk::ImageLayout::DEPTH_ATTACHMENT_OPTIMAL;
        let n = sh.casters.len() as u32;
        let mut draws = 0;
        unsafe {
            if !sh.ready {
                let b = [bar(
                    vk::ImageLayout::UNDEFINED,
                    ro,
                    vk::AccessFlags::empty(),
                    vk::AccessFlags::SHADER_READ,
                    range(0, sh.layers),
                )];
                d.cmd_pipeline_barrier(
                    cmd,
                    vk::PipelineStageFlags::TOP_OF_PIPE,
                    frag,
                    vk::DependencyFlags::empty(),
                    &[],
                    &[],
                    &b,
                );
                sh.ready = true;
            }
            if n == 0 {
                return 0;
            }
            let b = [bar(
                ro,
                att,
                vk::AccessFlags::SHADER_READ,
                vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE
                    | vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_READ,
                range(0, n),
            )];
            d.cmd_pipeline_barrier(cmd, frag, tests, vk::DependencyFlags::empty(), &[], &[], &b);
            d.cmd_bind_descriptor_sets(
                cmd,
                vk::PipelineBindPoint::GRAPHICS,
                sh.pipe_layout,
                0,
                &[set0],
                &[],
            );
            for (k, &(env, size)) in sh.casters.iter().enumerate() {
                let rect = vk::Rect2D {
                    offset: vk::Offset2D { x: 0, y: 0 },
                    extent: vk::Extent2D {
                        width: size,
                        height: size,
                    },
                };
                let datt = vk::RenderingAttachmentInfo::default()
                    .image_view(sh.layer_views[k])
                    .image_layout(att)
                    .load_op(vk::AttachmentLoadOp::CLEAR)
                    .store_op(vk::AttachmentStoreOp::STORE)
                    .clear_value(vk::ClearValue {
                        depth_stencil: vk::ClearDepthStencilValue {
                            depth: 1.0,
                            stencil: 0,
                        },
                    });
                d.cmd_begin_rendering(
                    cmd,
                    &vk::RenderingInfo::default()
                        .render_area(rect)
                        .layer_count(1)
                        .depth_attachment(&datt),
                );
                d.cmd_set_viewport(
                    cmd,
                    0,
                    &[vk::Viewport {
                        x: 0.0,
                        y: 0.0,
                        width: size as f32,
                        height: size as f32,
                        min_depth: 0.0,
                        max_depth: 1.0,
                    }],
                );
                d.cmd_set_scissor(cmd, 0, &[rect]);
                d.cmd_push_constants(
                    cmd,
                    sh.pipe_layout,
                    vk::ShaderStageFlags::VERTEX,
                    0,
                    &(k as u32).to_ne_bytes(),
                );
                let mut cur: Option<(bool, u32)> = None;
                let mut cur_skin = None;
                for dr in &self.draws {
                    if dr.key.blend != 0 || self.inst_env[dr.instance as usize] != env {
                        continue;
                    }
                    if cur_skin != Some(dr.key.skin) {
                        d.cmd_bind_pipeline(
                            cmd,
                            vk::PipelineBindPoint::GRAPHICS,
                            if dr.key.skin {
                                sh.pipe_skin
                            } else {
                                sh.pipe_static
                            },
                        );
                        cur_skin = Some(dr.key.skin);
                    }
                    if cur != Some((dr.key.skin, dr.mesh)) {
                        let m = if dr.key.skin {
                            &self.skin_meshes[dr.mesh as usize]
                        } else {
                            &self.meshes[dr.mesh as usize]
                        };
                        d.cmd_bind_vertex_buffers(cmd, 0, &[m.vbuf.buffer], &[0]);
                        d.cmd_bind_index_buffer(cmd, m.ibuf.buffer, 0, vk::IndexType::UINT32);
                        cur = Some((dr.key.skin, dr.mesh));
                    }
                    d.cmd_draw_indexed(cmd, dr.index_count, 1, dr.first_index, 0, dr.instance);
                    draws += 1;
                }
                d.cmd_end_rendering(cmd);
            }
            let b = [bar(
                att,
                ro,
                vk::AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE,
                vk::AccessFlags::SHADER_READ,
                range(0, n),
            )];
            d.cmd_pipeline_barrier(cmd, tests, frag, vk::DependencyFlags::empty(), &[], &[], &b);
        }
        sh.frames_with += 1;
        sh.caster_draws += draws as u64;
        if sh.frames_with % 300 == 1 && std::env::var_os("BOTS_N1_SHADOW_LOG").is_some() {
            let envs = self
                .lighting
                .dyn_frame
                .env_shadows
                .iter()
                .filter(|s| s.is_some())
                .count();
            eprintln!(
                "[n1] shadows: {n} drawn ({draws} caster draws, sizes {:?}, screen % covered {:?}); {envs} of {} envs shadowing; {} frames with",
                sh.casters.iter().map(|c| c.1).collect::<Vec<_>>(),
                sh.cover
                    .iter()
                    .map(|c| (c * 100.0).round() as u32)
                    .collect::<Vec<_>>(),
                self.lighting.dyn_frame.env_shadows.len(),
                sh.frames_with
            );
        }
        draws
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn xf(m: &[f32; 16], p: [f32; 3]) -> [f32; 4] {
        let mut o = [0f32; 4];
        for (r, v) in o.iter_mut().enumerate() {
            *v = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
        }
        o
    }

    #[test]
    fn hull_polygon_is_small_for_a_distant_shadow() {
        // Camera at the origin looking down -Z (Y-up), 90 degree fov, square 100x100 target.
        let (n, f) = (1.0f32, 10000.0f32);
        let proj = [
            1.0,
            0.0,
            0.0,
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
            0.0,
            0.0,
            f / (n - f),
            -1.0,
            0.0,
            0.0,
            n * f / (n - f),
            0.0,
        ];
        // A caster 3000 ahead, light 10 radii above it, radius 1.4x the distance.
        let center = [0.0, 0.0, -3000.0];
        let light = [0.0, 1000.0, -3000.0];
        let ft = fit(light, center, 100.0).unwrap();
        let tris = hull_polygon(&ft, light, 1400.0, 1.0, &proj, 0.5, 100, 100).unwrap();
        let area: f32 = tris
            .chunks(3)
            .map(|v| {
                ((v[1][0] - v[0][0]) * (v[2][1] - v[0][1])
                    - (v[2][0] - v[0][0]) * (v[1][1] - v[0][1]))
                    .abs()
                    * 0.5
            })
            .sum();
        assert!(
            area / 4.0 < 0.05,
            "covers {} of the screen: {tris:?}",
            area / 4.0
        );
        // The caster's centre is inside.
        let inside = tris.chunks(3).any(|v| {
            let s = |a: [f32; 2], b: [f32; 2]| {
                (b[0] - a[0]) * (0.0 - a[1]) - (b[1] - a[1]) * (0.0 - a[0])
            };
            let (x, y, z) = (s(v[0], v[1]), s(v[1], v[2]), s(v[2], v[0]));
            (x >= 0.0 && y >= 0.0 && z >= 0.0) || (x <= 0.0 && y <= 0.0 && z <= 0.0)
        });
        assert!(inside);
    }

    #[test]
    fn receiver_cull_keeps_every_darkened_point() {
        // B-231: every point the shader can darken (inside the radius, offset along some unit normal by at most
        // `off_max`, landing inside the shadow frustum past its near plane) must be inside a touching sphere.
        let mut seed = 12345u64;
        let mut rnd = || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            (seed >> 11) as f32 / (1u64 << 53) as f32
        };
        for case in 0..40 {
            let light = [
                rnd() * 400.0 - 200.0,
                300.0 + rnd() * 300.0,
                rnd() * 400.0 - 200.0,
            ];
            let center = [rnd() * 100.0 - 50.0, 50.0, rnd() * 100.0 - 50.0];
            let rad = 30.0 + rnd() * 60.0;
            let Some(ft) = fit(light, center, rad) else {
                continue;
            };
            let radius = 400.0 + rnd() * 1200.0;
            let off_max = 1.5 * (2.0 * ft.half.tan() / 64.0) * radius;
            let t = ft.half.tan() * 1.001;
            let off = off_max * 1.001 + 0.01;
            let cull = Cull {
                light,
                radius: radius * 1.001,
                f: ft.f,
                s: ft.s,
                u: ft.u,
                z0: (ft.near - off).max(0.0),
                t,
                off,
            };
            let mut hits = 0;
            for _ in 0..20000 {
                let w = [
                    light[0] + (rnd() * 2.0 - 1.0) * radius,
                    light[1] + (rnd() * 2.0 - 1.0) * radius,
                    light[2] + (rnd() * 2.0 - 1.0) * radius,
                ];
                let dv = sub(w, light);
                let dist = dot(dv, dv).sqrt();
                if dist >= radius {
                    continue;
                }
                let n = norm([rnd() * 2.0 - 1.0, rnd() * 2.0 - 1.0, rnd() * 2.0 - 1.0]);
                let k = off_max / radius * dist;
                let q = xf(&ft.vp, [w[0] + n[0] * k, w[1] + n[1] * k, w[2] + n[2] * k]);
                if q[3] <= 0.0 {
                    continue;
                }
                let (x, y, z) = (q[0] / q[3], q[1] / q[3], q[2] / q[3]);
                if x.abs() >= 1.0 || y.abs() >= 1.0 || z <= 0.0 {
                    continue;
                }
                hits += 1;
                assert!(
                    cull.touches(w, 0.01),
                    "case {case}: darkened point {w:?} culled"
                );
            }
            assert!(hits > 0 || case > 0);
        }
        // A sphere far outside the cone is culled.
        let light = [0.0, 1000.0, 0.0];
        let ft = fit(light, [0.0, 0.0, 0.0], 50.0).unwrap();
        let cull = Cull {
            light,
            radius: 2000.0,
            f: ft.f,
            s: ft.s,
            u: ft.u,
            z0: ft.near - 1.0,
            t: ft.half.tan(),
            off: 1.0,
        };
        assert!(cull.touches([0.0, 0.0, 0.0], 10.0));
        assert!(!cull.touches([900.0, 0.0, 0.0], 10.0));
        assert!(!cull.touches([0.0, 1500.0, 0.0], 10.0));
    }

    #[test]
    fn fit_frames_the_caster() {
        // Light 10 radii above-ish a 1-radius character at the origin.
        let light = [300.0, 900.0, -200.0];
        let vp = fit(light, [0.0, 50.0, 0.0], 60.0).unwrap().vp;
        let c = xf(&vp, [0.0, 50.0, 0.0]);
        assert!((c[0] / c[3]).abs() < 1e-4 && (c[1] / c[3]).abs() < 1e-4);
        let z = c[2] / c[3];
        assert!(z > 0.0 && z < 1.0, "{z}");
        // A point on the caster's sphere, off-axis, stays inside the map.
        let e = xf(&vp, [60.0, 50.0, 0.0]);
        assert!((e[0] / e[3]).abs() < 1.0 && (e[1] / e[3]).abs() < 1.0);
        // The ground behind the caster (further from the light) has a larger depth than the caster's front.
        let g = xf(&vp, [-30.0, -40.0, 20.0]);
        assert!(g[2] / g[3] > 0.0);
        // Light inside the caster: no shadow.
        assert!(fit([0.0, 0.0, 0.0], [0.0, 10.0, 0.0], 60.0).is_none());
    }
}
