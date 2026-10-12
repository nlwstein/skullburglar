// Ultrawide (GitHub #1): the game renders a wider frame instead of a pillarboxed 16:9 one.
// The 360 game always uses a 1280x720 back buffer. With --aspect_ratio wider than 16:9 the back buffer keeps its
// width and gets shorter (1280x548 for 21:9; the Xenia game patch fixes it at 1280x550), the camera keeps its vertical
// field of view and sees more at the sides (Hor+), and constrained-aspect cameras use the full frame. Most UI is laid out
// in fractions of the screen and stretches; the few screens laid out in 720-line pixels (the Music Player) lose their
// bottom. Pre-rendered movies (the story cutscenes) stay 16:9, pillarboxed. Native renderer only.
#pragma once

#include <cstdint>

namespace rex::ui {
class Window;
}

namespace bots {

// The window whose shape --aspect_ratio=auto follows. Set once the window exists (OnPostSetup).
void SetWidescreenWindow(rex::ui::Window* window);

// The back buffer's height in guest pixels: 720, or less with a wider aspect ratio. Fixed by the time the game
// creates its device (the first call decides it); reading it earlier decides it from the window as it is then.
uint32_t GuestHeight();

// A point in a window of w x h pixels -> a fraction of the game's picture (0..1 each way), which the presenter fits
// into the window with bars (16:9 in a 16:10 window, 21:9 in a 16:9 one). Points on a bar clamp to the picture's edge.
void WindowToPicture(float x, float y, float w, float h, float& fx, float& fy);

}  // namespace bots
