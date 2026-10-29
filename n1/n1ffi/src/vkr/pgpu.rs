//! B-186: the Vulkan side of N9's particles. Sprites are instanced quads expanded in the vertex shader (80-byte
//! instances); mesh particles are instanced meshes. Materials come from the loader thread as SPIR-V plus textures.
//!
//! Where they draw: mesh particles inside the scene pass (they test and write depth like geometry); sprites in a
//! second pass over the same HDR target with the depth buffer bound read-only (tested, never written) and sampled
//! for soft particles. Both draw after the opaque and blended scene geometry and before post.

use super::*;
use crate::particles::{LoadedMat, PDraw, PFrame, PNONE, tex_desc};
use crate::store::{Origin, PartKey};
use assetdb::MeshAsset;

/// Bytes of the globals block at the start of a slot's particle buffer; instances start at `P_BASE`.
const G_SIZE: u64 = 144 + LIT_BYTES;
const P_BASE: u64 = 512;
/// B-250: the lit-mesh block after the globals (`u.lit` in shader.rs): 20 vec4.
const LIT_BYTES: u64 = 320;

#[repr(C)]
#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]
struct PGlobals {
    vp: [f32; 16],
    right: [f32; 4],
    up: [f32; 4],
    fwd: [f32; 4],
    cam: [f32; 4],
    time: f32,
    near: f32,
    vw: f32,
    vh: f32,
    lit: [[f32; 4]; 20],
}

struct PMat {
    /// Store module ids.
    sprite: u32,
    mesh: u32,
    set: vk::DescriptorSet,
    blend: u8,
    two_sided: bool,
}

struct PMesh {
    vbuf: Buf,
    ibuf: Buf,
    index_count: u32,
}

pub(super) struct PGpu {
    pub g_layout: vk::DescriptorSetLayout,
    m_layout: vk::DescriptorSetLayout,
    pub(super) pipe_layout: vk::PipelineLayout,
    mats: Vec<PMat>,
    meshes: Vec<PMesh>,
    /// Finished pipelines by (material, variant): a lock-free copy of the store's table.
    pub(super) ready: HashMap<(u32, u8), vk::Pipeline>,
    tex_ids: HashMap<String, u32>,
}

impl PGpu {
    pub(super) unsafe fn new(device: &ash::Device) -> Result<PGpu> {
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
                b(1, vk::DescriptorType::SAMPLED_IMAGE, fr),
            ];
            let g_layout = device.create_descriptor_set_layout(
                &vk::DescriptorSetLayoutCreateInfo::default().bindings(&g),
                None,
            )?;
            let m = [
                b(0, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(1, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(2, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(3, vk::DescriptorType::SAMPLED_IMAGE, fr),
                b(4, vk::DescriptorType::SAMPLER, fr),
            ];
            let m_layout = device.create_descriptor_set_layout(
                &vk::DescriptorSetLayoutCreateInfo::default().bindings(&m),
                None,
            )?;
            let ls = [g_layout, m_layout];
            let pipe_layout = device.create_pipeline_layout(
                &vk::PipelineLayoutCreateInfo::default().set_layouts(&ls),
                None,
            )?;
            Ok(PGpu {
                g_layout,
                m_layout,
                pipe_layout,
                mats: Vec::new(),
                meshes: Vec::new(),
                ready: HashMap::new(),
                tex_ids: HashMap::new(),
            })
        }
    }

    /// Layouts (pipelines and shader modules belong to the store); buffers go through the renderer's allocator (`drop_buffers`).
    pub(super) unsafe fn destroy(&mut self, device: &ash::Device) {
        unsafe {
            self.ready.clear();
            self.mats.clear();
            device.destroy_pipeline_layout(self.pipe_layout, None);
            device.destroy_descriptor_set_layout(self.g_layout, None);
            device.destroy_descriptor_set_layout(self.m_layout, None);
        }
    }
}

impl Renderer {
    pub(super) fn pg_drop_buffers(&mut self) {
        for m in std::mem::take(&mut self.pg.meshes) {
            self.free(Garbage::Buffer(m.vbuf));
            self.free(Garbage::Buffer(m.ibuf));
        }
    }

    /// Takes a material the loader thread finished: shader modules, textures, the descriptor set.
    pub fn pmat_add(&mut self, m: &LoadedMat) -> Result<u32> {
        unsafe {
            let spv_key = |w: &[u32]| crate::plan::fnv(bytemuck::cast_slice(w));
            let sprite = self
                .store
                .module_from_spv(spv_key(&m.sprite_spv), &m.sprite_spv, Origin::Live)
                .ok_or_else(|| anyhow!("particle sprite shader {}", m.name))?;
            let mesh = self
                .store
                .module_from_spv(spv_key(&m.mesh_spv), &m.mesh_spv, Origin::Live)
                .ok_or_else(|| anyhow!("particle mesh shader {}", m.name))?;
            let mut ids = [TEX_GREY; 4];
            for (k, t) in m.textures.iter().take(4).enumerate() {
                let Some(t) = t else { continue };
                if let Some(&id) = self.pg.tex_ids.get(&t.name) {
                    ids[k] = id;
                    continue;
                }
                let mips: Vec<&[u8]> = t.mips.iter().map(|x| x.as_slice()).collect();
                let id = self.try_add_texture(&tex_desc(t, &mips, crate::particles::tex_raw(t)))?;
                self.pg.tex_ids.insert(t.name.clone(), id);
                ids[k] = id;
            }
            let set = self.alloc_set(self.pg.m_layout)?;
            let infos: Vec<[vk::DescriptorImageInfo; 1]> = ids
                .iter()
                .map(|&id| {
                    [vk::DescriptorImageInfo {
                        sampler: vk::Sampler::null(),
                        image_view: self.tex_view(id, TEX_GREY),
                        image_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                    }]
                })
                .collect();
            let smp = [vk::DescriptorImageInfo {
                sampler: self.sampler,
                image_view: vk::ImageView::null(),
                image_layout: vk::ImageLayout::UNDEFINED,
            }];
            let mut writes: Vec<vk::WriteDescriptorSet> = infos
                .iter()
                .enumerate()
                .map(|(k, i)| {
                    vk::WriteDescriptorSet::default()
                        .dst_set(set)
                        .dst_binding(k as u32)
                        .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                        .image_info(i)
                })
                .collect();
            writes.push(
                vk::WriteDescriptorSet::default()
                    .dst_set(set)
                    .dst_binding(4)
                    .descriptor_type(vk::DescriptorType::SAMPLER)
                    .image_info(&smp),
            );
            self.device.update_descriptor_sets(&writes, &[]);
            self.pg.mats.push(PMat {
                sprite,
                mesh,
                set,
                blend: blend_of(m.blend),
                two_sided: m.two_sided,
            });
            Ok(self.pg.mats.len() as u32 - 1)
        }
    }

    /// A mesh particle's mesh: 32-byte vertices (position, normal, uv) in Y-up, winding swapped for the reflection.
    pub fn pmesh_add(&mut self, m: &MeshAsset) -> Result<u32> {
        let mut verts: Vec<f32> = Vec::with_capacity(m.positions.len() * 8);
        for (i, p) in m.positions.iter().enumerate() {
            let n = m.normals.get(i).copied().unwrap_or([0.0, 0.0, 1.0]);
            let uv = m.uvs.get(i).copied().unwrap_or([0.0, 0.0]);
            verts.extend_from_slice(&[p[0], p[2], p[1], n[0], n[2], n[1], uv[0], uv[1]]);
        }
        let mut idx = m.indices.clone();
        for t in idx.chunks_exact_mut(3) {
            t.swap(1, 2);
        }
        let vb: &[u8] = bytemuck::cast_slice(&verts);
        let ib: &[u8] = bytemuck::cast_slice(&idx);
        let mut vbuf = self.buffer(
            vb.len() as u64,
            vk::BufferUsageFlags::VERTEX_BUFFER,
            "n1 pmesh v",
        )?;
        vbuf.bytes()[..vb.len()].copy_from_slice(vb);
        let mut ibuf = self.buffer(
            ib.len() as u64,
            vk::BufferUsageFlags::INDEX_BUFFER,
            "n1 pmesh i",
        )?;
        ibuf.bytes()[..ib.len()].copy_from_slice(ib);
        self.pg.meshes.push(PMesh {
            vbuf,
            ibuf,
            index_count: idx.len() as u32,
        });
        Ok(self.pg.meshes.len() as u32 - 1)
    }

    /// The store's key for (material, variant: 0 sprite, 1 velocity-aligned sprite, 2 mesh).
    pub(super) fn part_key(&self, mat: u32, variant: u8) -> PartKey {
        let m = &self.pg.mats[mat as usize];
        PartKey {
            module: if variant == 2 { m.mesh } else { m.sprite },
            blend: m.blend,
            // Only mesh particles cull.
            two_sided: m.two_sided || variant != 2,
            variant,
        }
    }

    /// Picks up the pipelines the workers finished and asks for the rest (never builds one here); draws without a
    /// pipeline wait for it.
    pub(super) fn pg_resolve(&mut self, pf: &PFrame) {
        for d in &pf.draws {
            let k = (d.mat, d.variant);
            if self.pg.ready.contains_key(&k) {
                continue;
            }
            let pk = self.part_key(d.mat, d.variant);
            match self.store.pipe_lookup(crate::store::AnyKeyRef::Part(pk)) {
                Some(p) => {
                    self.pg.ready.insert(k, p);
                }
                None => self.store.want_part_pipe(pk),
            }
        }
    }

    /// B-250: the lighting block for lit mesh particles: sun, sky hemisphere and the four dynamic lights that light
    /// this frame's mesh instances most. All zero (flat 0.7, the old look) when there are no mesh particles, no feed
    /// lighting yet or `BOTS_N1_MESH_LIT=0`.
    fn mesh_lit_block(&self, pf: &PFrame) -> [[f32; 4]; 20] {
        let mut out = [[0.0f32; 4]; 20];
        // lit[1].w: clamp the mesh instances' colour to 1 (the original's instance colour is 8-bit unorm).
        let clamp_w = if self.knobs.mesh_vcol_clamp { 1.0 } else { 0.0 };
        out[1][3] = clamp_w;
        if !self.knobs.mesh_lit || !pf.has_meshes() || self.lighting.frames == 0 {
            return out;
        }
        let e = &self.lighting.env;
        let v4 = |c: [f32; 3], w: f32| [c[0], c[1], c[2], w];
        out[0] = v4(e.sun_dir, if e.sun_enabled { 2.0 } else { 1.0 });
        out[1] = v4(e.sun_col, clamp_w);
        out[2] = v4(e.amb_up, 0.0);
        out[3] = v4(e.amb_dn, 0.0);
        // The instances' positions (column 3 of each model matrix).
        let pos: Vec<[f32; 3]> = pf
            .meshes
            .chunks_exact(20)
            .map(|m| [m[12], m[13], m[14]])
            .collect();
        let lights = &self.lighting.dyn_frame.lights;
        let mut cand: Vec<(f32, usize)> = Vec::new();
        for (li, l) in lights.iter().enumerate() {
            let rad = 1.0 / l[3].max(1e-9);
            let luma = l[4] * 0.3 + l[5] * 0.59 + l[6] * 0.11;
            let mut best = 0.0f32;
            for p in &pos {
                let d =
                    ((l[0] - p[0]).powi(2) + (l[1] - p[1]).powi(2) + (l[2] - p[2]).powi(2)).sqrt();
                best += luma * lightfeed::point_attenuation(d, rad, l[7].max(1.0));
            }
            if best > 1e-3 {
                cand.push((best, li));
            }
        }
        cand.sort_by(|a, b| b.0.total_cmp(&a.0));
        for (k, (_, li)) in cand.iter().take(4).enumerate() {
            let l = &lights[*li];
            for j in 0..4 {
                out[4 + 4 * k + j] = [l[4 * j], l[4 * j + 1], l[4 * j + 2], l[4 * j + 3]];
            }
        }
        out
    }

    /// Writes this frame's globals and instances into the slot's particle buffer and its descriptor set.
    pub(super) fn pg_stage(
        &mut self,
        si: usize,
        pf: &PFrame,
        view_proj: &[f32; 16],
        eye: [f32; 3],
        vw: u32,
        vh: u32,
    ) -> Result<()> {
        let sprite_bytes = (pf.sprites.len() * 4) as u64;
        let mesh_at = align(P_BASE + sprite_bytes);
        let need = mesh_at + (pf.meshes.len() * 4) as u64;
        if self.slots[si].pbuf.cap < need {
            let buf = self.buffer(
                need.next_power_of_two().max(64 << 10),
                vk::BufferUsageFlags::UNIFORM_BUFFER | vk::BufferUsageFlags::VERTEX_BUFFER,
                "n1 particles",
            )?;
            let old = std::mem::replace(&mut self.slots[si].pbuf, buf);
            self.free(Garbage::Buffer(old)); // the slot's last use has completed
        }
        let v = |a: [f32; 3]| [a[0], a[1], a[2], 0.0];
        let g = PGlobals {
            vp: swz_vp(view_proj),
            right: v(pf.right),
            up: v(pf.up),
            fwd: v(pf.fwd),
            cam: v([eye[0], eye[2], eye[1]]),
            time: self.time(),
            near: pf.near,
            vw: vw as f32,
            vh: vh as f32,
            lit: self.mesh_lit_block(pf),
        };
        let b = self.slots[si].pbuf.bytes();
        b[..G_SIZE as usize].copy_from_slice(bytemuck::bytes_of(&g));
        let s = P_BASE as usize;
        b[s..s + pf.sprites.len() * 4].copy_from_slice(bytemuck::cast_slice(&pf.sprites));
        let m = mesh_at as usize;
        b[m..m + pf.meshes.len() * 4].copy_from_slice(bytemuck::cast_slice(&pf.meshes));
        let ub = [vk::DescriptorBufferInfo {
            buffer: self.slots[si].pbuf.buffer,
            offset: 0,
            range: G_SIZE,
        }];
        let di = [vk::DescriptorImageInfo {
            sampler: vk::Sampler::null(),
            image_view: self.depth.as_ref().unwrap().view,
            image_layout: vk::ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        }];
        let set = self.slots[si].pset;
        let writes = [
            vk::WriteDescriptorSet::default()
                .dst_set(set)
                .dst_binding(0)
                .descriptor_type(vk::DescriptorType::UNIFORM_BUFFER)
                .buffer_info(&ub),
            vk::WriteDescriptorSet::default()
                .dst_set(set)
                .dst_binding(1)
                .descriptor_type(vk::DescriptorType::SAMPLED_IMAGE)
                .image_info(&di),
        ];
        unsafe { self.device.update_descriptor_sets(&writes, &[]) };
        Ok(())
    }

    /// Records the particle draws of one kind (`meshes`: inside the scene pass; otherwise the sprite pass).
    /// Returns the draw count.
    pub(super) fn pg_record(
        &self,
        cmd: vk::CommandBuffer,
        si: usize,
        pf: &PFrame,
        meshes: bool,
    ) -> u32 {
        let d = &self.device;
        let buf = self.slots[si].pbuf.buffer;
        let mesh_at = align(P_BASE + (pf.sprites.len() * 4) as u64);
        let mut n = 0;
        let mut bound = false;
        let mut cur: Option<(u32, u8)> = None;
        let mut cur_mat = u32::MAX;
        for dr in pf.draws.iter().filter(|x| (x.variant == 2) == meshes) {
            let PDraw {
                mat,
                variant,
                mesh,
                first,
                count,
            } = *dr;
            let Some(&pipe) = self.pg.ready.get(&(mat, variant)) else {
                continue;
            };
            unsafe {
                if !bound {
                    d.cmd_bind_descriptor_sets(
                        cmd,
                        vk::PipelineBindPoint::GRAPHICS,
                        self.pg.pipe_layout,
                        0,
                        &[self.slots[si].pset],
                        &[],
                    );
                    if !meshes {
                        d.cmd_bind_vertex_buffers(cmd, 0, &[buf], &[P_BASE]);
                    }
                    bound = true;
                }
                if cur != Some((mat, variant)) {
                    d.cmd_bind_pipeline(cmd, vk::PipelineBindPoint::GRAPHICS, pipe);
                    cur = Some((mat, variant));
                }
                if cur_mat != mat {
                    d.cmd_bind_descriptor_sets(
                        cmd,
                        vk::PipelineBindPoint::GRAPHICS,
                        self.pg.pipe_layout,
                        1,
                        &[self.pg.mats[mat as usize].set],
                        &[],
                    );
                    cur_mat = mat;
                }
                if meshes {
                    if mesh == PNONE {
                        continue;
                    }
                    let Some(m) = self.pg.meshes.get(mesh as usize) else {
                        continue;
                    };
                    d.cmd_bind_vertex_buffers(cmd, 0, &[m.vbuf.buffer, buf], &[0, mesh_at]);
                    d.cmd_bind_index_buffer(cmd, m.ibuf.buffer, 0, vk::IndexType::UINT32);
                    d.cmd_draw_indexed(cmd, m.index_count, count, 0, 0, first);
                } else {
                    d.cmd_draw(cmd, 6, count, 0, first);
                }
            }
            n += 1;
        }
        n
    }
}
