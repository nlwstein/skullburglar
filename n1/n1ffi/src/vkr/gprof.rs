//! GPU profile of n1's frame: timestamps between its passes, `BOTS_N1_GPU_PROF=1` (off by default).
//!
//! Each `record` writes `MARKS` timestamps into one ring entry of a query pool; the entry is read back (without
//! waiting) the next time the ring comes round, `RING` frames later, and skipped if the GPU has not finished it.
//! Every `REPORT` read frames it prints the mean ms per segment to stderr (the host's run log):
//! `n1 gpu prof: N frames: shadows a, opaque b, trees+mesh particles c, sprites d, post e, post mats g (B-266), total f ms`.
use ash::vk;

pub const MARKS: u32 = 7;
const RING: u32 = 4;
const REPORT: u32 = 300;
const NAMES: [&str; MARKS as usize - 1] = [
    "shadows",
    "opaque",
    "trees+mesh particles",
    "sprites",
    "post",
    "post mats",
];

pub struct GpuProf {
    pool: vk::QueryPool,
    period_ns: f64,
    valid_mask: u64,
    frame: u64,
    written: [bool; RING as usize],
    acc: [f64; MARKS as usize - 1],
    n: u32,
    cur: u32,
}

impl GpuProf {
    /// `None` unless BOTS_N1_GPU_PROF=1 and the first graphics queue family (the host's) has timestamps.
    pub fn from_env(
        instance: &ash::Instance,
        device: &ash::Device,
        phys: vk::PhysicalDevice,
    ) -> Option<Self> {
        if std::env::var("BOTS_N1_GPU_PROF")
            .map(|v| v == "0" || v.is_empty())
            .unwrap_or(true)
        {
            return None;
        }
        let props = unsafe { instance.get_physical_device_properties(phys) };
        let fams = unsafe { instance.get_physical_device_queue_family_properties(phys) };
        let bits = fams
            .iter()
            .find(|f| f.queue_flags.contains(vk::QueueFlags::GRAPHICS))
            .map(|f| f.timestamp_valid_bits)
            .unwrap_or(0);
        if bits == 0 || props.limits.timestamp_period <= 0.0 {
            eprintln!("n1 gpu prof: no timestamps on this queue");
            return None;
        }
        let pool = unsafe {
            device.create_query_pool(
                &vk::QueryPoolCreateInfo::default()
                    .query_type(vk::QueryType::TIMESTAMP)
                    .query_count(MARKS * RING),
                None,
            )
        }
        .ok()?;
        eprintln!(
            "n1 gpu prof: on, period {} ns, {} valid bits",
            props.limits.timestamp_period, bits
        );
        Some(Self {
            pool,
            period_ns: f64::from(props.limits.timestamp_period),
            valid_mask: if bits >= 64 {
                u64::MAX
            } else {
                (1u64 << bits) - 1
            },
            frame: 0,
            written: [false; RING as usize],
            acc: [0.0; MARKS as usize - 1],
            n: 0,
            cur: 0,
        })
    }

    /// Call at the start of the frame's GPU work, outside any render pass: reads the ring entry this frame reuses
    /// (if the GPU is done with it), resets it and writes mark 0.
    pub fn begin(&mut self, device: &ash::Device, cmd: vk::CommandBuffer) {
        let e = (self.frame % u64::from(RING)) as u32;
        self.frame += 1;
        self.cur = e;
        if self.written[e as usize] {
            let mut ts = [[0u64; 2]; MARKS as usize];
            let ok = unsafe {
                device.get_query_pool_results(
                    self.pool,
                    e * MARKS,
                    &mut ts,
                    vk::QueryResultFlags::TYPE_64 | vk::QueryResultFlags::WITH_AVAILABILITY,
                )
            };
            if ok.is_ok() && ts.iter().all(|t| t[1] != 0) {
                for k in 0..NAMES.len() {
                    let d = ts[k + 1][0].wrapping_sub(ts[k][0]) & self.valid_mask;
                    self.acc[k] += d as f64 * self.period_ns * 1e-6;
                }
                self.n += 1;
                if self.n == REPORT {
                    let total: f64 = self.acc.iter().sum();
                    let parts: Vec<String> = NAMES
                        .iter()
                        .zip(self.acc.iter())
                        .map(|(name, a)| format!("{name} {:.3}", a / f64::from(self.n)))
                        .collect();
                    eprintln!(
                        "n1 gpu prof: {} frames: {}, total {:.3} ms",
                        self.n,
                        parts.join(", "),
                        total / f64::from(self.n)
                    );
                    self.acc = [0.0; MARKS as usize - 1];
                    self.n = 0;
                }
            }
        }
        unsafe {
            device.cmd_reset_query_pool(cmd, self.pool, e * MARKS, MARKS);
        }
        self.written[e as usize] = true;
        self.mark(device, cmd, 0);
    }

    /// Writes mark `k` (1..MARKS-1) once all earlier work in `cmd` has finished.
    pub fn mark(&self, device: &ash::Device, cmd: vk::CommandBuffer, k: u32) {
        unsafe {
            device.cmd_write_timestamp(
                cmd,
                vk::PipelineStageFlags::BOTTOM_OF_PIPE,
                self.pool,
                self.cur * MARKS + k,
            );
        }
    }

    pub fn destroy(&self, device: &ash::Device) {
        unsafe { device.destroy_query_pool(self.pool, None) };
    }
}
