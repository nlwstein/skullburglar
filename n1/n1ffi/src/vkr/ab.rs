//! Same-frame A/B parity, the GPU side (`crate::ab` has the switches, hooks and diff). On a chosen frame `record` runs
//! its scene and post passes three times from the one frame state: variant A into offscreen target A, variant B into
//! B, and the shipping path into the host's target (so the player sees what a normal frame gives). The two offscreen
//! targets are copied into host-readable buffers; once the host reports the submission complete, a later `record` compares
//! them on the CPU and logs the result (`summary: n1: ab ...`), and writes `ab.csv` and PNGs when `BOTS_N1_AB_DIR` is set.
//!
//! Cost when off: none. `Renderer::ab` is `None`, `record` runs its one pass, and no shader, layout or pipeline differs.
//! Frames in an A/B run that are not captured cost nothing extra either; the captured frame costs three renders.

use super::*;
use crate::ab::{self, Layout, Stats};

/// The state of an A/B run (present only when `BOTS_N1_AB` is set and valid).
pub(super) struct AbState {
    cfg: &'static ab::Config,
    /// Scene frames seen (frames with draws).
    frames: u64,
    trigger_was_set: bool,
    targets: Option<[Rt; 2]>,
    readback: Option<[Buf; 2]>,
    pending: Option<Pending>,
    seq: u32,
    csv_started: bool,
    /// All compared frames: how many, the worst difference, the pixels that differed.
    done: u64,
    worst: f32,
    differ_total: u64,
    layout: Option<(Layout, bool)>,
    /// `BOTS_N1_AB_TIME`: GPU timestamps around each variant (`TimePool`).
    tpool: Option<TimePool>,
}

/// Timestamps for `BOTS_N1_AB_TIME=1`: per frame four marks (begin and end of variant 0 = A and variant 1 = B), a ring of
/// `TRING` frames read back without waiting. The variants' order alternates every frame (`swap`) so a position effect
/// (the second pass finding warm caches, or a boost state) cancels in the pair statistics. Logged 20 pairs to a line,
/// `summary: n1: abtime <order><A ms>/<B ms> ...` (order 0 = A first, 1 = B first).
struct TimePool {
    pool: vk::QueryPool,
    period_ms: f64,
    mask: u64,
    frame: u64,
    ring: [Option<bool>; TRING as usize],
    out: Vec<String>,
}
const TRING: u32 = 8;

/// Where `record` writes the stamps of this frame (`AbFrame::time`).
#[derive(Clone, Copy)]
pub(super) struct TimeSlot {
    pool: vk::QueryPool,
    base: u32,
}

struct Pending {
    serial: u64,
    seq: u32,
    frame: u64,
    w: u32,
    h: u32,
    draws: usize,
}

/// What `record` needs for the frame it runs A/B on: the two targets and the push-constant flags of each variant.
#[derive(Clone, Copy)]
pub(super) struct AbFrame {
    pub images: [vk::Image; 2],
    pub views: [vk::ImageView; 2],
    pub flags: [u32; 2],
    pub rect: vk::Rect2D,
    /// Timing mode: stamps go here and nothing is copied or compared.
    pub time: Option<TimeSlot>,
    /// The variants run B first on this frame (timing mode alternates).
    pub swap: bool,
}

/// Byte layout of a target format for the compare, and whether the red channel is the third byte.
fn layout_of(f: vk::Format) -> Option<(Layout, bool)> {
    match f {
        vk::Format::R8G8B8A8_UNORM | vk::Format::R8G8B8A8_SRGB => Some((Layout::Bytes4, false)),
        vk::Format::B8G8R8A8_UNORM | vk::Format::B8G8R8A8_SRGB => Some((Layout::Bytes4, true)),
        vk::Format::R16G16B16A16_SFLOAT => Some((Layout::Half4, false)),
        _ => None,
    }
}

impl AbState {
    pub(super) fn from_env(instance: &ash::Instance, device: &ash::Device, phys: vk::PhysicalDevice) -> Option<AbState> {
        let cfg = ab::config()?;
        let tpool = if cfg.time { TimePool::new(instance, device, phys) } else { None };
        if cfg.time && tpool.is_none() {
            n1log!(log::WARN, "summary: n1: ab: no GPU timestamps on this queue; BOTS_N1_AB_TIME off");
        }
        n1log!(
            log::INFO,
            "summary: n1: same-frame A/B on: {} (A flags {}, B flags {}, every {} frames, on request: debug bit 24, dir {})",
            cfg.names,
            cfg.flags_a,
            cfg.flags_b,
            cfg.every,
            cfg.dir.as_ref().map_or("none".to_string(), |d| d.display().to_string())
        );
        Some(AbState {
            cfg,
            frames: 0,
            trigger_was_set: false,
            targets: None,
            readback: None,
            pending: None,
            seq: 0,
            csv_started: false,
            done: 0,
            worst: 0.0,
            differ_total: 0,
            layout: None,
            tpool,
        })
    }
}

impl TimePool {
    fn new(instance: &ash::Instance, device: &ash::Device, phys: vk::PhysicalDevice) -> Option<Self> {
        let props = unsafe { instance.get_physical_device_properties(phys) };
        let fams = unsafe { instance.get_physical_device_queue_family_properties(phys) };
        let bits = fams.iter().find(|f| f.queue_flags.contains(vk::QueueFlags::GRAPHICS)).map_or(0, |f| f.timestamp_valid_bits);
        if bits == 0 || props.limits.timestamp_period <= 0.0 {
            return None;
        }
        let pool = unsafe {
            device.create_query_pool(
                &vk::QueryPoolCreateInfo::default().query_type(vk::QueryType::TIMESTAMP).query_count(4 * TRING),
                None,
            )
        }
        .ok()?;
        Some(Self {
            pool,
            period_ms: f64::from(props.limits.timestamp_period) * 1e-6,
            mask: if bits >= 64 { u64::MAX } else { (1u64 << bits) - 1 },
            frame: 0,
            ring: [None; TRING as usize],
            out: Vec::new(),
        })
    }
}

impl Renderer {
    /// Called at the top of `record`: compares a capture whose submission has completed.
    pub(super) fn ab_poll(&mut self, completed: u64) {
        let Some(ab) = self.ab.as_mut() else { return };
        let Some(p) = ab.pending.as_ref() else { return };
        if completed < p.serial {
            return;
        }
        let p = ab.pending.take().unwrap();
        let Some((layout, swap)) = ab.layout else { return };
        let Some(bufs) = ab.readback.as_mut() else { return };
        let px = (p.w as usize) * (p.h as usize);
        let bytes = px * if layout == Layout::Half4 { 8 } else { 4 };
        let (a, b) = {
            let [ba, bb] = bufs;
            (ba.bytes()[..bytes].to_vec(), bb.bytes()[..bytes].to_vec())
        };
        let st = ab::compare(layout, &a, &b, px);
        ab.done += 1;
        ab.worst = ab.worst.max(st.max);
        ab.differ_total += st.differ;
        n1log!(
            log::INFO,
            "summary: n1: ab {} frame {} ({}x{}, {} draws): max {:.3} mean {:.6} differ {} of {} px, mean B {:.2} | {} compared, worst max {:.3}",
            ab.cfg.names,
            p.frame,
            p.w,
            p.h,
            p.draws,
            st.max,
            st.mean,
            st.differ,
            st.pixels,
            st.mean_b,
            ab.done,
            ab.worst
        );
        if let Some(dir) = ab.cfg.dir.clone() {
            ab_write(ab, &dir, &p, &st, layout, swap, &a, &b);
        }
    }

    /// Decides whether this frame runs A/B, and makes the targets and buffers. `None`: an ordinary frame.
    pub(super) fn ab_begin(&mut self, target: &Target, serial: u64) -> Option<AbFrame> {
        if self.draws.is_empty() || self.gprof.is_some() {
            return None;
        }
        if self.ab.as_ref().is_some_and(|a| a.cfg.time) {
            return self.ab_begin_time(target);
        }
        let rect = target.rect;
        let bits = self.debug_bits & ab::TRIGGER_BIT != 0;
        let fmt = self.color_format;
        let ab = self.ab.as_mut()?;
        ab.frames += 1;
        let edge = bits && !ab.trigger_was_set;
        ab.trigger_was_set = bits;
        let periodic = ab.cfg.every > 0 && ab.frames % ab.cfg.every == 0;
        if !(edge || periodic) || ab.pending.is_some() {
            return None;
        }
        let Some(layout) = layout_of(fmt) else {
            n1log!(log::WARN, "summary: n1: ab: target format {fmt:?} is not supported; same-frame A/B off");
            self.ab = None;
            return None;
        };
        let (w, h) = (rect.extent.width.max(1), rect.extent.height.max(1));
        let bytes = (w as u64) * (h as u64) * if layout.0 == Layout::Half4 { 8 } else { 4 };
        let need_targets = !self.ab.as_ref()?.targets.as_ref().is_some_and(|t| t[0].width == target.width && t[0].height == target.height);
        let need_bufs = !self.ab.as_ref()?.readback.as_ref().is_some_and(|b| b[0].cap >= bytes);
        if need_targets {
            let old = self.ab.as_mut()?.targets.take();
            for r in old.into_iter().flatten() {
                self.defer(Garbage::Image(r.image, r.view, r.alloc));
            }
            let a = self.rt(fmt, target.width, target.height, false, "n1 ab A").ok();
            let b = self.rt(fmt, target.width, target.height, false, "n1 ab B").ok();
            let (Some(a), Some(b)) = (a, b) else {
                n1log!(log::WARN, "summary: n1: ab: could not allocate the targets; same-frame A/B off");
                self.ab = None;
                return None;
            };
            self.ab.as_mut()?.targets = Some([a, b]);
        }
        if need_bufs {
            let old = self.ab.as_mut()?.readback.take();
            for b in old.into_iter().flatten() {
                self.defer(Garbage::Buffer(b));
            }
            let a = self.buffer_in(bytes, vk::BufferUsageFlags::TRANSFER_DST, "n1 ab readback A", MemoryLocation::GpuToCpu).ok();
            let b = self.buffer_in(bytes, vk::BufferUsageFlags::TRANSFER_DST, "n1 ab readback B", MemoryLocation::GpuToCpu).ok();
            let (Some(a), Some(b)) = (a, b) else {
                n1log!(log::WARN, "summary: n1: ab: could not allocate the readback buffers; same-frame A/B off");
                self.ab = None;
                return None;
            };
            self.ab.as_mut()?.readback = Some([a, b]);
        }
        let draws = self.draws.len();
        let ab = self.ab.as_mut()?;
        ab.layout = Some(layout);
        ab.seq += 1;
        ab.pending = Some(Pending { serial, seq: ab.seq, frame: ab.frames, w, h, draws });
        let t = ab.targets.as_ref()?;
        Some(AbFrame {
            images: [t[0].image, t[1].image],
            views: [t[0].view, t[1].view],
            flags: [ab.cfg.flags_a, ab.cfg.flags_b],
            rect,
            time: None,
            swap: false,
        })
    }

    /// Timing mode: reads back the stamps of an earlier frame, then (every `every` frames) makes the offscreen targets and
    /// returns the frame's plan. No readback buffers, no compare.
    fn ab_begin_time(&mut self, target: &Target) -> Option<AbFrame> {
        let fmt = self.color_format;
        let d = self.device.clone();
        let ab = self.ab.as_mut()?;
        ab.frames += 1;
        let every = ab.cfg.every.max(1);
        let (fa, fb) = (ab.cfg.flags_a, ab.cfg.flags_b);
        let tp = ab.tpool.as_mut()?;
        if ab.frames % every != 0 {
            return None;
        }
        let e = (tp.frame % u64::from(TRING)) as usize;
        if let Some(swap) = tp.ring[e].take() {
            let mut ts = [[0u64; 2]; 4];
            let ok = unsafe {
                d.get_query_pool_results(tp.pool, e as u32 * 4, &mut ts, vk::QueryResultFlags::TYPE_64 | vk::QueryResultFlags::WITH_AVAILABILITY)
            };
            if ok.is_ok() && ts.iter().all(|t| t[1] != 0) {
                let ms = |i: usize| ts[2 * i + 1][0].wrapping_sub(ts[2 * i][0]) & tp.mask;
                tp.out.push(format!("{}{:.3}/{:.3}", u8::from(swap), ms(0) as f64 * tp.period_ms, ms(1) as f64 * tp.period_ms));
                if tp.out.len() >= 20 {
                    n1log!(log::INFO, "summary: n1: abtime {} {}", ab.cfg.names, tp.out.join(" "));
                    tp.out.clear();
                }
            }
        }
        let swap = tp.frame % 2 == 1;
        let (pool, base) = (tp.pool, e as u32 * 4);
        tp.ring[e] = Some(swap);
        tp.frame += 1;
        let rect = target.rect;
        let need = !ab.targets.as_ref().is_some_and(|t| t[0].width == target.width && t[0].height == target.height);
        if need {
            let old = ab.targets.take();
            for r in old.into_iter().flatten() {
                self.defer(Garbage::Image(r.image, r.view, r.alloc));
            }
            let a = self.rt(fmt, target.width, target.height, false, "n1 ab A").ok();
            let b = self.rt(fmt, target.width, target.height, false, "n1 ab B").ok();
            let (Some(a), Some(b)) = (a, b) else {
                n1log!(log::WARN, "summary: n1: ab: could not allocate the targets; timing off");
                self.ab = None;
                return None;
            };
            self.ab.as_mut()?.targets = Some([a, b]);
        }
        let t = self.ab.as_ref()?.targets.as_ref()?;
        Some(AbFrame {
            images: [t[0].image, t[1].image],
            views: [t[0].view, t[1].view],
            flags: [fa, fb],
            rect,
            time: Some(TimeSlot { pool, base }),
            swap,
        })
    }

    /// Timing mode: stamp `k` (0 = begin, 1 = end) of variant `which`; the first stamp of a frame resets its four slots.
    pub(super) fn ab_stamp(&self, cmd: vk::CommandBuffer, f: &AbFrame, which: usize, k: u32, first: bool) {
        let Some(t) = f.time else { return };
        unsafe {
            if first {
                self.device.cmd_reset_query_pool(cmd, t.pool, t.base, 4);
            }
            self.device.cmd_write_timestamp(cmd, vk::PipelineStageFlags::BOTTOM_OF_PIPE, t.pool, t.base + 2 * which as u32 + k);
        }
    }

    /// Records the copy of offscreen target `which` (A = 0, B = 1) into its readback buffer, after its last pass.
    pub(super) fn ab_copy_out(&self, cmd: vk::CommandBuffer, f: &AbFrame, which: usize) {
        let Some(ab) = self.ab.as_ref() else { return };
        let Some(bufs) = ab.readback.as_ref() else { return };
        let d = &self.device;
        let color = vk::ImageAspectFlags::COLOR;
        unsafe {
            d.cmd_pipeline_barrier(
                cmd,
                vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT,
                vk::PipelineStageFlags::TRANSFER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &[layout_barrier(
                    f.images[which],
                    color,
                    vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
                    vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    vk::AccessFlags::COLOR_ATTACHMENT_WRITE,
                    vk::AccessFlags::TRANSFER_READ,
                )],
            );
            d.cmd_copy_image_to_buffer(
                cmd,
                f.images[which],
                vk::ImageLayout::TRANSFER_SRC_OPTIMAL,
                bufs[which].buffer,
                &[vk::BufferImageCopy {
                    buffer_offset: 0,
                    buffer_row_length: f.rect.extent.width,
                    buffer_image_height: f.rect.extent.height,
                    image_subresource: vk::ImageSubresourceLayers { aspect_mask: color, mip_level: 0, base_array_layer: 0, layer_count: 1 },
                    image_offset: vk::Offset3D { x: f.rect.offset.x, y: f.rect.offset.y, z: 0 },
                    image_extent: vk::Extent3D { width: f.rect.extent.width, height: f.rect.extent.height, depth: 1 },
                }],
            );
            // The copy's result is read by the host after the submission completes (the host's fence makes it visible).
            d.cmd_pipeline_barrier(
                cmd,
                vk::PipelineStageFlags::TRANSFER,
                vk::PipelineStageFlags::HOST,
                vk::DependencyFlags::empty(),
                &[vk::MemoryBarrier::default().src_access_mask(vk::AccessFlags::TRANSFER_WRITE).dst_access_mask(vk::AccessFlags::HOST_READ)],
                &[],
                &[],
            );
        }
    }

    /// Frees the A/B targets and buffers (at teardown).
    pub(super) fn ab_free(&mut self) {
        let Some(mut ab) = self.ab.take() else { return };
        if ab.done > 0 {
            n1log!(
                log::INFO,
                "summary: n1: ab {} total: {} frames compared, worst max {:.3}, {} differing pixels",
                ab.cfg.names,
                ab.done,
                ab.worst,
                ab.differ_total
            );
        }
        if let Some(tp) = ab.tpool.take() {
            if !tp.out.is_empty() {
                n1log!(log::INFO, "summary: n1: abtime {} {}", ab.cfg.names, tp.out.join(" "));
            }
            unsafe { self.device.destroy_query_pool(tp.pool, None) };
        }
        for r in ab.targets.take().into_iter().flatten() {
            self.free(Garbage::Image(r.image, r.view, r.alloc));
        }
        for b in ab.readback.take().into_iter().flatten() {
            self.free(Garbage::Buffer(b));
        }
    }
}

#[allow(clippy::too_many_arguments)]
fn ab_write(ab: &mut AbState, dir: &std::path::Path, p: &Pending, st: &Stats, layout: Layout, swap: bool, a: &[u8], b: &[u8]) {
    use std::io::Write;
    if std::fs::create_dir_all(dir).is_err() {
        return;
    }
    let csv = dir.join("ab.csv");
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&csv) {
        if !ab.csv_started {
            let _ = writeln!(f, "seq,frame,width,height,draws,max,mean,differ,pixels,mean_b");
            ab.csv_started = true;
        }
        let _ = writeln!(f, "{},{},{},{},{},{:.4},{:.8},{},{},{:.3}", p.seq, p.frame, p.w, p.h, p.draws, st.max, st.mean, st.differ, st.pixels, st.mean_b);
    }
    if st.differ > 0 || ab.cfg.save_all {
        let px = (p.w as usize) * (p.h as usize);
        let (ra, rb) = (ab::to_rgb8(layout, a, px, swap), ab::to_rgb8(layout, b, px, swap));
        let diff = ab::diff_rgb8(&ra, &rb);
        for (name, rgb) in [("a", &ra), ("b", &rb), ("diff", &diff)] {
            let _ = std::fs::write(dir.join(format!("ab{:04}-{name}.png", p.seq)), ab::png_rgb8(p.w, p.h, rgb));
        }
    }
}
