//! Suspect-draw detector (log-only). Each frame the resolver and the skinned build count draws that look wrong:
//! a material missing for good, a material that is the engine's default look (`WorldGrid`, `DefaultMaterial`), a
//! skinned section drawn with the grey stand-in, a skinned mesh whose light environment n1 has no record of. This
//! module keeps the session totals and writes one `n1: suspicious draw ...` line the first time a frame has any, then
//! at most one more per distinct (mesh, material, kind) every two seconds, up to `MAX_LINES` a session. It changes
//! nothing that is drawn. `BOTS_N1_SUSPECT=0` turns it off (no counting, no lines).

use std::collections::{HashMap, HashSet};
use std::time::{Duration, Instant};

use scenekit::{Resolver, SuspectFrame, NONE};

use crate::skin::Skins;

const MAX_LINES: u32 = 32;
const MIN_GAP: Duration = Duration::from_secs(2);

#[derive(Default)]
pub struct Suspect {
    /// Frames with any suspect draw, and the totals of each kind over the session.
    pub frames: u64,
    pub missing: u64,
    pub default_look: u64,
    pub skin_grey: u64,
    pub skin_env: u64,
    pub skin_zero: u64,
    pub lines: u32,
    seen: HashSet<(&'static str, bool, u32, u32, u32)>,
    last: Option<Instant>,
    frame_no: u64,
}

impl Suspect {
    /// Adds a frame's counts; returns the log line when one is due.
    pub fn note(
        &mut self,
        res: &Resolver,
        skins: &Skins,
        zero_envs: &HashMap<usize, String>,
        f: &SuspectFrame,
    ) -> Option<String> {
        self.frame_no += 1;
        if f.total() == 0 {
            return None;
        }
        self.frames += 1;
        self.missing += u64::from(f.missing);
        self.default_look += u64::from(f.default_look);
        self.skin_grey += u64::from(f.skin_grey);
        self.skin_env += u64::from(f.skin_env);
        self.skin_zero += u64::from(f.skin_zero);
        let s = f.sample?;
        if self.lines >= MAX_LINES || !self.seen.insert((s.kind, s.skel, s.mesh, s.material, s.env)) {
            return None;
        }
        let now = Instant::now();
        if self.last.is_some_and(|t| now.duration_since(t) < MIN_GAP) {
            self.seen.remove(&(s.kind, s.skel, s.mesh, s.material, s.env));
            return None;
        }
        self.last = Some(now);
        self.lines += 1;
        let mesh = if s.skel { skins.mesh_path(s.mesh) } else { res.mesh_path(s.mesh) }.unwrap_or("?");
        let material = if s.material == NONE { "none" } else { res.material_path(s.material).unwrap_or("?") };
        // The mesh's package is its path up to the first dot (the level or content package it was cooked in).
        let package = mesh.split('.').next().unwrap_or("?");
        let env = zero_envs.get(&(s.env as usize)).map(|d| format!(" env: {d}")).unwrap_or_default();
        Some(format!(
            "n1: suspicious draw ({}{}): mesh={mesh} material={material} package={package}{env} frame_counts=missing:{} \
             default:{} skin_grey:{} skin_env:{} skin_zero:{} scene_frame={} (line {} of {MAX_LINES}; BOTS_N1_SUSPECT=0 \
             turns this off)",
            s.kind,
            if s.skel { ", skinned" } else { "" },
            f.missing,
            f.default_look,
            f.skin_grey,
            f.skin_env,
            f.skin_zero,
            self.frame_no,
            self.lines,
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_frame_counts_nothing() {
        let f = SuspectFrame::default();
        assert_eq!(f.total(), 0);
    }

    #[test]
    fn frame_total_adds_all_kinds() {
        let f = SuspectFrame { missing: 1, default_look: 2, skin_grey: 3, skin_env: 4, skin_zero: 5, sample: None };
        assert_eq!(f.total(), 15);
    }
}
