//! Where n1's state messages go. The host (`n1_set_log`) gives a sink that writes them to the game log, which is what the
//! player's diagnostics file reads; without a sink (tests, examples, a host that never set one) they go to stderr as before.
//! Per-frame and debugging output (`BOTS_N1_*_LOG`, draw dumps) stays on stderr: only state a bug report needs goes here.

use std::ffi::{CString, c_char};
use std::sync::atomic::{AtomicUsize, Ordering};

pub const INFO: u32 = 0;
pub const WARN: u32 = 1;
pub const ERROR: u32 = 2;

/// `void (*)(uint32_t level, const char* message)`: level 0 info, 1 warning, 2 error; the message has no newline.
pub type Sink = extern "C" fn(u32, *const c_char);

static SINK: AtomicUsize = AtomicUsize::new(0);

pub fn set_sink(sink: Option<Sink>) {
    SINK.store(sink.map_or(0, |f| f as usize), Ordering::Release);
}

/// One message at `level`: to the sink when there is one, else to stderr.
pub fn emit(level: u32, msg: &str) {
    let raw = SINK.load(Ordering::Acquire);
    if raw != 0 {
        // SAFETY: only `set_sink` stores here, and only a `Sink` (a function pointer) as usize.
        let sink: Sink = unsafe { std::mem::transmute::<usize, Sink>(raw) };
        // A NUL inside the text would end the C string early; the text is ours, so this only drops a stray one.
        if let Ok(c) = CString::new(msg.replace('\0', " ")) {
            sink(level, c.as_ptr());
            return;
        }
    }
    eprintln!("{msg}");
}

/// `n1log!(log::WARN, "n1ffi: {x}")`: `format!` arguments, routed by [`emit`].
macro_rules! n1log {
    ($level:expr, $($arg:tt)*) => { $crate::log::emit($level, &format!($($arg)*)) };
}
pub(crate) use n1log;

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static GOT: Mutex<Vec<(u32, String)>> = Mutex::new(Vec::new());
    // The sink is process-wide, so one test owns it.
    extern "C" fn collect(level: u32, msg: *const c_char) {
        let s = unsafe { std::ffi::CStr::from_ptr(msg) }.to_string_lossy().into_owned();
        GOT.lock().unwrap().push((level, s));
    }

    #[test]
    fn sink_gets_levels_and_text_then_stderr_again() {
        set_sink(Some(collect));
        n1log!(INFO, "summary: n1: aa plan {} at {}x{}", "smaa", 853, 480);
        n1log!(WARN, "n1ffi: texture upload failed: {}", "no memory");
        emit(ERROR, "a\0b");
        set_sink(None);
        emit(INFO, "goes to stderr, not the sink");
        let got = GOT.lock().unwrap();
        assert_eq!(
            *got,
            vec![
                (INFO, "summary: n1: aa plan smaa at 853x480".to_string()),
                (WARN, "n1ffi: texture upload failed: no memory".to_string()),
                (ERROR, "a b".to_string()),
            ]
        );
    }
}
