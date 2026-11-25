//! #56: the pause menu's blurred backdrop is made once per pause, not every frame.
//!
//! The paused picture does not change, so the renderer blurs it (at a quarter of the resolution) into a target that stays between
//! frames, and each paused frame only reads that target. [`PauseCache`] decides, frame by frame, whether the target must be
//! (re)made: on the first paused frame, after a resize or a render scale change (the key changes), and again after the menu
//! has closed.

/// What the key of a filled backdrop holds: the render size and the view's position in the target (a split-screen view has its own).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PauseKey {
    pub size: (u32, u32),
    pub at: (i32, i32),
}

/// What a frame does about the backdrop.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PauseStep {
    /// The pause backdrop isn't in this frame.
    Idle,
    /// Blur the frame into the backdrop target (the first paused frame, or a changed key).
    Fill,
    /// The target holds this pause's blur: only read it.
    Reuse,
}

#[derive(Debug, Default)]
pub struct PauseCache {
    filled: Option<PauseKey>,
    fills: u64,
    reuses: u64,
}

impl PauseCache {
    /// The step for this frame: `blur` whether the frame has the pause backdrop with the blur on, `key` the target's key.
    pub fn step(&mut self, blur: bool, key: PauseKey) -> PauseStep {
        if !blur {
            self.filled = None;
            return PauseStep::Idle;
        }
        if self.filled == Some(key) {
            self.reuses += 1;
            return PauseStep::Reuse;
        }
        self.filled = Some(key);
        self.fills += 1;
        PauseStep::Fill
    }

    /// The target was reallocated (its contents are gone).
    pub fn reset(&mut self) {
        self.filled = None;
    }

    /// (fills, reuses) so far.
    pub fn counts(&self) -> (u64, u64) {
        (self.fills, self.reuses)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const K: PauseKey = PauseKey { size: (853, 480), at: (0, 0) };

    #[test]
    fn blurs_once_then_reuses_until_the_menu_closes() {
        let mut c = PauseCache::default();
        assert_eq!(c.step(false, K), PauseStep::Idle);
        assert_eq!(c.step(true, K), PauseStep::Fill);
        for _ in 0..100 {
            assert_eq!(c.step(true, K), PauseStep::Reuse);
        }
        assert_eq!(c.counts(), (1, 100));
        // The menu closes for a frame: the next pause blurs again.
        assert_eq!(c.step(false, K), PauseStep::Idle);
        assert_eq!(c.step(true, K), PauseStep::Fill);
        assert_eq!(c.step(true, K), PauseStep::Reuse);
    }

    #[test]
    fn a_resize_or_scale_change_blurs_again() {
        let mut c = PauseCache::default();
        assert_eq!(c.step(true, K), PauseStep::Fill);
        let bigger = PauseKey { size: (1280, 720), at: (0, 0) };
        assert_eq!(c.step(true, bigger), PauseStep::Fill);
        assert_eq!(c.step(true, bigger), PauseStep::Reuse);
        // Another view of a split screen has its own backdrop; the two never share one.
        let other_view = PauseKey { size: (1280, 720), at: (0, 360) };
        assert_eq!(c.step(true, other_view), PauseStep::Fill);
        assert_eq!(c.step(true, bigger), PauseStep::Fill);
    }

    #[test]
    fn a_reallocated_target_blurs_again() {
        let mut c = PauseCache::default();
        assert_eq!(c.step(true, K), PauseStep::Fill);
        c.reset();
        assert_eq!(c.step(true, K), PauseStep::Fill);
        assert_eq!(c.step(true, K), PauseStep::Reuse);
    }

    #[test]
    fn the_blur_setting_off_never_fills() {
        let mut c = PauseCache::default();
        for _ in 0..10 {
            assert_eq!(c.step(false, K), PauseStep::Idle);
        }
        assert_eq!(c.counts(), (0, 0));
    }
}
