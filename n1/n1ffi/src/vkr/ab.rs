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
    pub(super) fn from_env() -> Option<AbState> {
        let cfg = ab::config()?;
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
        })
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
