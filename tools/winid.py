"""Print the CGWindowID of an app's largest normal window (empty if none), for `screencapture -l <id>`.

    uv run --with pyobjc-framework-Quartz python tools/winid.py bots

Window-only screenshots: never fall back to a full-desktop capture.
"""
import sys
import Quartz
wins = Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionAll, Quartz.kCGNullWindowID)
best = None
for w in wins:
    if w.get("kCGWindowOwnerName") == sys.argv[1] and w.get("kCGWindowLayer") == 0:
        b = w["kCGWindowBounds"]; area = b["Width"] * b["Height"]
        if not best or area > best[0]: best = (area, w["kCGWindowNumber"])
print(best[1] if best else "")
