//! B-198 / "shader work up front": one place that sees every pipeline and shader compile n1 does, says when it
//! happened (the startup step = the first-run shader screen, a loading screen, or live play) and keeps the totals.
//!
//! The phase is derived, not signalled: `Startup` until the startup step ends, then `Play` while scene frames are being
//! recorded (the last `n1_record` was under `PLAY_WINDOW_MS` ago) and `Loading` otherwise (loading screens, the
//! preload hold, menus without a scene). A compile in `Play` is a bug to fix: the line is always logged
//! (`compile-in-play`), whatever `BOTS_N1_COMPILE_LOG` says. `summary()` is logged when the store is destroyed and is
//! what `tools/instr/compile_gate.py` reads to fail a warm-cache run that compiled in play.

use crate::log::{self, n1log};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed};
use std::sync::OnceLock;
use std::time::Instant;

/// A scene frame this recent means we are in play (a frame is 16 ms; a loading screen records none).
pub const PLAY_WINDOW_MS: u64 = 250;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Phase {
    Startup,
    Loading,
    Play,
}

impl Phase {
    pub fn name(self) -> &'static str {
        match self {
            Phase::Startup => "startup",
            Phase::Loading => "loading",
            Phase::Play => "play",
        }
    }
}

/// The pure rule: which phase a compile at `now_ms` belongs to.
pub fn phase_at(startup_done: bool, last_scene_ms: Option<u64>, now_ms: u64) -> Phase {
    if !startup_done {
        return Phase::Startup;
    }
    match last_scene_ms {
        Some(t) if now_ms.saturating_sub(t) <= PLAY_WINDOW_MS => Phase::Play,
        _ => Phase::Loading,
    }
}

static START: OnceLock<Instant> = OnceLock::new();
static STARTUP_DONE: AtomicBool = AtomicBool::new(false);
/// ms since `START` of the last scene frame, plus one (0 = none yet).
static LAST_SCENE: AtomicU64 = AtomicU64::new(0);

#[derive(Default)]
struct Bucket {
    n: AtomicU64,
    us: AtomicU64,
    max_us: AtomicU64,
    /// Compiles that held the render thread (post, AA, tree pipelines): each one is a frame hitch.
    stalls: AtomicU64,
    stall_us: AtomicU64,
}

static BUCKETS: [Bucket; 3] = [
    Bucket { n: AtomicU64::new(0), us: AtomicU64::new(0), max_us: AtomicU64::new(0), stalls: AtomicU64::new(0), stall_us: AtomicU64::new(0) },
    Bucket { n: AtomicU64::new(0), us: AtomicU64::new(0), max_us: AtomicU64::new(0), stalls: AtomicU64::new(0), stall_us: AtomicU64::new(0) },
    Bucket { n: AtomicU64::new(0), us: AtomicU64::new(0), max_us: AtomicU64::new(0), stalls: AtomicU64::new(0), stall_us: AtomicU64::new(0) },
];

fn now_ms() -> u64 {
    START.get_or_init(Instant::now).elapsed().as_millis() as u64
}

/// Called by the startup step when it is over.
pub fn startup_done() {
    STARTUP_DONE.store(true, Relaxed);
}

/// Called for every scene frame (`n1_record`).
pub fn scene_frame() {
    LAST_SCENE.store(now_ms() + 1, Relaxed);
}

pub fn phase() -> Phase {
    let last = LAST_SCENE.load(Relaxed).checked_sub(1);
    phase_at(STARTUP_DONE.load(Relaxed), last, now_ms())
}

/// One finished compile. `kind`: "scene_pipe", "scene_shader", "part_pipe", "part_module", "post_pipe", "aa_pipe",
/// "tree_pipe", ... `label` says which. `stall`: it ran on the render thread, so the frame waited for it.
pub fn record(kind: &str, label: &str, us: u64, stall: bool) -> Phase {
    let p = phase();
    let b = &BUCKETS[p as usize];
    b.n.fetch_add(1, Relaxed);
    b.us.fetch_add(us, Relaxed);
    b.max_us.fetch_max(us, Relaxed);
    if stall {
        b.stalls.fetch_add(1, Relaxed);
        b.stall_us.fetch_add(us, Relaxed);
    }
    if p == Phase::Play {
        n1log!(
            log::WARN,
            "n1ffi: compile-in-play t={:.3}s kind={kind} {label} {:.1} ms{}",
            now_ms() as f64 / 1e3,
            us as f64 / 1e3,
            if stall { " (render thread)" } else { " (worker)" }
        );
    } else if std::env::var_os("BOTS_N1_COMPILE_LOG").is_some() {
        eprintln!("n1ffi: compile t={:.3}s phase={} kind={kind} {label} {:.1} ms", now_ms() as f64 / 1e3, p.name(), us as f64 / 1e3);
    }
    p
}

/// Times `f` and records it: for the renderer's own pipelines, which are made inline.
pub fn timed<T>(kind: &str, label: &str, stall: bool, f: impl FnOnce() -> T) -> T {
    let t = Instant::now();
    let r = f();
    record(kind, label, t.elapsed().as_micros() as u64, stall);
    r
}

/// The totals line: `compiles startup=N/Mms loading=N/Mms play=N/Mms (render-thread N/Mms, max Mms)`.
pub fn summary() -> String {
    let f = |p: Phase| {
        let b = &BUCKETS[p as usize];
        (b.n.load(Relaxed), b.us.load(Relaxed) as f64 / 1e3, b.max_us.load(Relaxed) as f64 / 1e3, b.stalls.load(Relaxed), b.stall_us.load(Relaxed) as f64 / 1e3)
    };
    let (s, l, p) = (f(Phase::Startup), f(Phase::Loading), f(Phase::Play));
    format!(
        "summary: n1: compiles startup={}/{:.0}ms loading={}/{:.0}ms play={}/{:.0}ms play-render-thread={}/{:.0}ms play-max={:.1}ms",
        s.0, s.1, l.0, l.1, p.0, p.1, p.3, p.4, p.2
    )
}

pub fn log_summary() {
    n1log!(log::INFO, "{}", summary());
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn startup_until_the_step_ends() {
        assert_eq!(phase_at(false, Some(10), 20), Phase::Startup);
        assert_eq!(phase_at(false, None, 20), Phase::Startup);
    }

    #[test]
    fn scene_frames_mean_play_and_their_absence_means_loading() {
        assert_eq!(phase_at(true, None, 5000), Phase::Loading);
        assert_eq!(phase_at(true, Some(1000), 1100), Phase::Play);
        assert_eq!(phase_at(true, Some(1000), 1000 + PLAY_WINDOW_MS), Phase::Play);
        assert_eq!(phase_at(true, Some(1000), 1000 + PLAY_WINDOW_MS + 1), Phase::Loading);
    }

    #[test]
    fn records_land_in_the_current_phase_and_the_summary_says_so() {
        // Globals: this is the only test that touches them.
        scene_frame();
        startup_done();
        assert_eq!(phase(), Phase::Play);
        record("aa_pipe", "fs_cas", 2500, true);
        let s = summary();
        assert!(s.contains("play=1/3ms") || s.contains("play=1/2ms"), "{s}");
        assert!(s.contains("play-render-thread=1/"), "{s}");
    }
}
