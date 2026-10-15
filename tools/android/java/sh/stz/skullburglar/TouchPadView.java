package sh.stz.skullburglar;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.util.SparseArray;
import android.view.DisplayCutout;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

// The on-screen controller, a port of the iOS one (src/ios/ios_app.mm) with the same layout and behaviour: Xbox Cloud
// Gaming's standard touch controls, which mirror the controller. The left stick high on the left with the d-pad below
// and inside it; A/B/X/Y high on the right with the right stick below and inside them; triggers over bumpers in the top
// corners; View and Menu at the top centre. A stick starts where the thumb lands near it and returns home on release;
// a double tap holds the stick's click (L3 / R3) for as long as the thumb stays down. Shown while no game controller
// is connected; the state goes to the game through GameNative.setPad.
final class TouchPadView extends View implements InputManager.InputDeviceListener {
    // XINPUT_GAMEPAD_* bits.
    private static final int UP = 0x0001, DOWN = 0x0002, LEFT = 0x0004, RIGHT = 0x0008;
    private static final int START = 0x0010, BACK = 0x0020, LS = 0x0040, RS = 0x0080;
    private static final int LB = 0x0100, RB = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    private static final int LT = 1, RT = 2;  // triggers, kept apart from the button bits

    // Sizes in dp (iOS points).
    private static final float STICK_RADIUS = 62;
    private static final float KNOB_RADIUS = 30;
    private static final float FACE = 56;
    private static final float HIT_SLOP = 14;
    private static final long DOUBLE_TAP_MS = 300;

    private static final int ICON_NONE = 0, ICON_VIEW = 1, ICON_MENU = 2;

    private static final class Btn {
        final String text;
        final int icon;
        final int bit;
        final int trigger;
        final float glyph;  // text size, dp
        final RectF frame = new RectF();
        float corner;
        boolean down;

        Btn(String text, int icon, int bit, int trigger, float glyph) {
            this.text = text;
            this.icon = icon;
            this.bit = bit;
            this.trigger = trigger;
            this.glyph = glyph;
        }
    }

    // A stick with a home position. A touch inside its zone recentres it under the thumb; release sends it home.
    private static final class Stick {
        final boolean right;
        final RectF zone = new RectF();
        float homeX, homeY, originX, originY, dx, dy;
        int pointer = -1;
        boolean click;
        long lastTap = -DOUBLE_TAP_MS;

        Stick(boolean right) {
            this.right = right;
        }
    }

    // Order matters: layout() places them by index.
    private final Btn[] buttons = {
        new Btn("A", ICON_NONE, A, 0, 24), new Btn("B", ICON_NONE, B, 0, 24),
        new Btn("X", ICON_NONE, X, 0, 24), new Btn("Y", ICON_NONE, Y, 0, 24),
        new Btn("LB", ICON_NONE, LB, 0, 16), new Btn("RB", ICON_NONE, RB, 0, 16),
        new Btn("LT", ICON_NONE, 0, LT, 16), new Btn("RT", ICON_NONE, 0, RT, 16),
        new Btn(null, ICON_VIEW, BACK, 0, 14), new Btn(null, ICON_MENU, START, 0, 14),
    };
    private final Stick[] sticks = {new Stick(false), new Stick(true)};
    private final SparseArray<Btn> buttonPointers = new SparseArray<>();
    // D-pad: one control, eight directions from the angle around its centre.
    private float dpadX, dpadY, dpadRadius;
    private int dpadPointer = -1;
    private int dpadBits;
    private int lx, ly, rx, ry;
    private int lastButtons, lastLt, lastRt;

    private final float d;  // px per dp
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint outline = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint glyph = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF scratch = new RectF();
    private final InputManager inputManager;
    private boolean shown;

    TouchPadView(Context context) {
        super(context);
        d = context.getResources().getDisplayMetrics().density;
        outline.setStyle(Paint.Style.STROKE);
        outline.setStrokeWidth(1.5f * d);
        outline.setColor(Color.argb(140, 255, 255, 255));
        glyph.setColor(Color.argb(235, 255, 255, 255));
        glyph.setTextAlign(Paint.Align.CENTER);
        glyph.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        glyph.setStrokeCap(Paint.Cap.ROUND);
        glyph.setStrokeJoin(Paint.Join.ROUND);
        inputManager = (InputManager) context.getSystemService(Context.INPUT_SERVICE);
        inputManager.registerInputDeviceListener(this, new Handler(Looper.getMainLooper()));
        updateVisibility();
    }

    void release() {
        inputManager.unregisterInputDeviceListener(this);
    }

    // A real game controller hides the pad (and the game ignores it while hidden).
    private static boolean controllerConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual()) {
                continue;
            }
            int sources = device.getSources();
            if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                return true;
            }
        }
        return false;
    }

    private void updateVisibility() {
        shown = !controllerConnected();
        setVisibility(shown ? VISIBLE : GONE);
        if (!shown) {
            releaseAll();
        }
        publish();
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        updateVisibility();
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        updateVisibility();
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        updateVisibility();
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        layout(insets);
        invalidate();
        return insets;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layout(getRootWindowInsets());
    }

    private void put(int i, float cx, float cy, float w, float h) {
        Btn b = buttons[i];
        b.frame.set(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
        b.corner = Math.min(w, h) / 2;
    }

    private void layout(WindowInsets insets) {
        final float width = getWidth(), height = getHeight();
        if (width <= 0 || height <= 0) {
            return;
        }
        // Inside the display cutout, with a small margin.
        float il = 0, it = 0, ir = 0, ib = 0;
        DisplayCutout cutout = insets != null ? insets.getDisplayCutout() : null;
        if (cutout != null) {
            il = cutout.getSafeInsetLeft();
            it = cutout.getSafeInsetTop();
            ir = cutout.getSafeInsetRight();
            ib = cutout.getSafeInsetBottom();
        }
        final float L = il + 12 * d, R = width - ir - 12 * d, T = it + 10 * d, Bm = height - ib - 10 * d;
        final float w = R - L, h = Bm - T;
        final float face = FACE * d;

        // Right hand: A/B/X/Y high and outside, the right stick low and inside.
        final float gap = face * 1.05f;
        final float fx = R - gap - face / 2, fy = T + h * 0.52f;
        put(0, fx, fy + gap, face, face);  // A
        put(1, fx + gap, fy, face, face);  // B
        put(2, fx - gap, fy, face, face);  // X
        put(3, fx, fy - gap, face, face);  // Y
        // Shoulders: the trigger in the corner, the bumper under it.
        final float sw = 88 * d, sh = 40 * d;
        put(6, L + sw / 2, T + sh / 2, sw, sh);      // LT
        put(4, L + sw / 2, T + sh * 1.7f, sw, sh);   // LB
        put(7, R - sw / 2, T + sh / 2, sw, sh);      // RT
        put(5, R - sw / 2, T + sh * 1.7f, sw, sh);   // RB
        put(8, L + w / 2 - 30 * d, T + 20 * d, 40 * d, 40 * d);  // View
        put(9, L + w / 2 + 30 * d, T + 20 * d, 40 * d, 40 * d);  // Menu

        // Sticks: left high and outside, right low and inside, like the controller. Each takes touches in its quarter.
        final float stick = STICK_RADIUS * d;
        Stick left = sticks[0], right = sticks[1];
        left.homeX = L + stick + 26 * d;
        left.homeY = T + h * 0.55f;
        right.homeX = R - w * 0.30f;
        right.homeY = Bm - stick - 4 * d;
        final float top = T + sh * 2.3f;  // below the shoulder buttons
        left.zone.set(0, top, L + w * 0.30f, height);
        right.zone.set(L + w * 0.55f, T + h * 0.45f, L + w * 0.55f + w * 0.45f - gap * 2.2f, height);

        // D-pad: low and inside, under the left stick.
        dpadRadius = 50 * d;
        dpadX = L + w * 0.30f;
        dpadY = Bm - dpadRadius - 2 * d;
    }

    private void setFill(boolean down) {
        fill.setColor(down ? Color.argb(128, 255, 255, 255) : Color.argb(89, 0, 0, 0));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        // Buttons.
        for (Btn b : buttons) {
            canvas.save();
            if (b.down) {
                canvas.scale(0.93f, 0.93f, b.frame.centerX(), b.frame.centerY());
            }
            final int alpha = b.down ? 255 : 204;
            canvas.saveLayerAlpha(b.frame.left - 4 * d, b.frame.top - 4 * d, b.frame.right + 4 * d,
                    b.frame.bottom + 4 * d, alpha);
            setFill(b.down);
            canvas.drawRoundRect(b.frame, b.corner, b.corner, fill);
            canvas.drawRoundRect(b.frame, b.corner, b.corner, outline);
            drawGlyph(canvas, b);
            canvas.restore();
            canvas.restore();
        }

        // D-pad: a dim disc with four arms.
        fill.setColor(Color.argb(51, 0, 0, 0));
        canvas.drawCircle(dpadX, dpadY, dpadRadius + 6 * d, fill);
        canvas.drawCircle(dpadX, dpadY, dpadRadius + 6 * d, outline);
        final float arm = dpadRadius * 0.64f, thick = dpadRadius * 0.62f;
        final int[] bits = {UP, DOWN, LEFT, RIGHT};
        for (int i = 0; i < 4; ++i) {
            switch (i) {
                case 0: scratch.set(dpadX - thick / 2, dpadY - dpadRadius, dpadX + thick / 2, dpadY - dpadRadius + arm);
                    break;
                case 1: scratch.set(dpadX - thick / 2, dpadY + dpadRadius - arm, dpadX + thick / 2, dpadY + dpadRadius);
                    break;
                case 2: scratch.set(dpadX - dpadRadius, dpadY - thick / 2, dpadX - dpadRadius + arm, dpadY + thick / 2);
                    break;
                default: scratch.set(dpadX + dpadRadius - arm, dpadY - thick / 2, dpadX + dpadRadius, dpadY + thick / 2);
                    break;
            }
            final boolean down = (dpadBits & bits[i]) != 0;
            canvas.saveLayerAlpha(scratch.left - 4 * d, scratch.top - 4 * d, scratch.right + 4 * d,
                    scratch.bottom + 4 * d, down ? 255 : 204);
            setFill(down);
            canvas.drawRoundRect(scratch, 6 * d, 6 * d, fill);
            canvas.drawRoundRect(scratch, 6 * d, 6 * d, outline);
            canvas.restore();
        }

        // Sticks.
        final float stick = STICK_RADIUS * d, knob = KNOB_RADIUS * d;
        for (Stick s : sticks) {
            final boolean held = s.pointer >= 0;
            final float cx = held ? s.originX : s.homeX, cy = held ? s.originY : s.homeY;
            final float kx = cx + s.dx, ky = cy + s.dy;
            canvas.saveLayerAlpha(cx - stick - 8 * d, cy - stick - 8 * d, cx + stick + 8 * d, cy + stick + 8 * d,
                    held ? 255 : 191);
            fill.setColor(Color.argb(64, 0, 0, 0));
            canvas.drawCircle(cx, cy, stick, fill);
            canvas.drawCircle(cx, cy, stick, outline);
            fill.setColor(s.click ? Color.argb(140, 255, 255, 255) : Color.argb(153, 38, 38, 38));
            canvas.drawCircle(kx, ky, knob, fill);
            canvas.drawCircle(kx, ky, knob, outline);
            glyph.setStyle(Paint.Style.FILL);
            glyph.setTextSize(15 * d);
            canvas.drawText(s.right ? "R" : "L", kx, ky - (glyph.descent() + glyph.ascent()) / 2, glyph);
            canvas.restore();
        }
    }

    private void drawGlyph(Canvas canvas, Btn b) {
        final float cx = b.frame.centerX(), cy = b.frame.centerY();
        if (b.text != null) {
            glyph.setStyle(Paint.Style.FILL);
            glyph.setTextSize(b.glyph * d);
            canvas.drawText(b.text, cx, cy - (glyph.descent() + glyph.ascent()) / 2, glyph);
            return;
        }
        glyph.setStyle(Paint.Style.STROKE);
        glyph.setStrokeWidth(2 * d);
        final float s = b.glyph * d;  // icon size
        if (b.icon == ICON_VIEW) {
            // Two overlapping rectangles.
            scratch.set(cx - s * 0.55f, cy - s * 0.25f, cx + s * 0.25f, cy + s * 0.45f);
            canvas.drawRoundRect(scratch, 2 * d, 2 * d, glyph);
            canvas.drawLine(cx - s * 0.25f, cy - s * 0.45f, cx + s * 0.55f, cy - s * 0.45f, glyph);
            canvas.drawLine(cx + s * 0.55f, cy - s * 0.45f, cx + s * 0.55f, cy + s * 0.2f, glyph);
        } else {
            // Three lines.
            for (int i = -1; i <= 1; ++i) {
                canvas.drawLine(cx - s * 0.5f, cy + i * s * 0.35f, cx + s * 0.5f, cy + i * s * 0.35f, glyph);
            }
        }
    }

    // The nearest button whose shape (plus slop) holds the point, so neighbours split the gap between them.
    private Btn buttonAt(float x, float y) {
        Btn best = null;
        float bestD = Float.MAX_VALUE;
        for (Btn b : buttons) {
            final RectF f = b.frame;
            final float r = b.corner;
            // Distance from the point to the rounded rect: to its core segment, minus the corner radius.
            final float cx = Math.max(f.left + r, Math.min(x, f.right - r));
            final float cy = Math.max(f.top + r, Math.min(y, f.bottom - r));
            final float dist = (float) Math.hypot(x - cx, y - cy) - r;
            if (dist < HIT_SLOP * d && dist < bestD) {
                best = b;
                bestD = dist;
            }
        }
        return best;
    }

    private boolean inDpad(float x, float y) {
        return Math.hypot(x - dpadX, y - dpadY) < dpadRadius + HIT_SLOP * d;
    }

    private int dpadBitsAt(float x, float y) {
        final float dx = x - dpadX, dy = y - dpadY;
        if (Math.hypot(dx, dy) < dpadRadius * 0.2f) {
            return 0;  // the centre is neutral
        }
        // Eight 45-degree sectors; the diagonal ones press both neighbours.
        final int sector = (int) Math.round(Math.atan2(-dy, dx) / (Math.PI / 4)) & 7;  // 0 = right, counter-clockwise
        final int[] sectors = {RIGHT, RIGHT | UP, UP, UP | LEFT, LEFT, LEFT | DOWN, DOWN, DOWN | RIGHT};
        return sectors[sector];
    }

    private static int axis(float v, float radius) {
        return Math.round(Math.max(-1f, Math.min(1f, v / radius)) * 32767);
    }

    private void moveStick(Stick s, float x, float y) {
        final float radius = STICK_RADIUS * d;
        float dx = x - s.originX, dy = y - s.originY;
        final float len = (float) Math.hypot(dx, dy);
        if (len > radius) {
            dx *= radius / len;
            dy *= radius / len;
        }
        s.dx = dx;
        s.dy = dy;
        if (s.right) {
            rx = axis(dx, radius);
            ry = axis(-dy, radius);  // up is positive
        } else {
            lx = axis(dx, radius);
            ly = axis(-dy, radius);
        }
    }

    private void releaseStick(Stick s) {
        s.pointer = -1;
        s.click = false;
        s.dx = s.dy = 0;
        if (s.right) {
            rx = ry = 0;
        } else {
            lx = ly = 0;
        }
    }

    private void releaseAll() {
        for (Stick s : sticks) {
            releaseStick(s);
        }
        dpadPointer = -1;
        dpadBits = 0;
        buttonPointers.clear();
    }

    private void publish() {
        int bits = dpadBits;
        int lt = 0, rt = 0;
        for (Btn b : buttons) {
            b.down = false;
        }
        for (int i = 0; i < buttonPointers.size(); ++i) {
            Btn b = buttonPointers.valueAt(i);
            b.down = true;
            bits |= b.bit;
            if (b.trigger == LT) {
                lt = 255;
            }
            if (b.trigger == RT) {
                rt = 255;
            }
        }
        if (sticks[0].click) {
            bits |= LS;
        }
        if (sticks[1].click) {
            bits |= RS;
        }
        // A light tap on every new press, like a controller's click.
        if ((bits & ~lastButtons) != 0 || (lt != 0 && lastLt == 0) || (rt != 0 && lastRt == 0)) {
            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
        }
        lastButtons = bits;
        lastLt = lt;
        lastRt = rt;
        GameNative.setPad(shown, bits, lt, rt, lx, ly, rx, ry);
        invalidate();
    }

    private void begin(int pointer, float x, float y, long time) {
        if (dpadPointer < 0 && inDpad(x, y)) {
            dpadPointer = pointer;
            dpadBits = dpadBitsAt(x, y);
            return;
        }
        Btn b = buttonAt(x, y);
        if (b != null) {
            buttonPointers.put(pointer, b);
            return;
        }
        final float radius = STICK_RADIUS * d;
        for (Stick s : sticks) {
            // The stick's own circle counts as its zone too, wherever its quarter ends.
            final boolean onStick = Math.hypot(x - s.homeX, y - s.homeY) < radius + HIT_SLOP * d;
            if (s.pointer >= 0 || !(onStick || s.zone.contains(x, y))) {
                continue;
            }
            s.pointer = pointer;
            // Touching the resting stick grabs it where it is; elsewhere in the zone it recentres under the thumb.
            s.originX = onStick ? s.homeX : x;
            s.originY = onStick ? s.homeY : y;
            s.click = time - s.lastTap < DOUBLE_TAP_MS;
            s.lastTap = time;
            moveStick(s, x, y);
            break;
        }
    }

    private void move(int pointer, float x, float y) {
        if (pointer == dpadPointer) {
            dpadBits = dpadBitsAt(x, y);
            return;
        }
        for (Stick s : sticks) {
            if (pointer == s.pointer) {
                moveStick(s, x, y);
                return;
            }
        }
        // Sliding onto another button presses that one instead (A -> B rolls).
        if (buttonPointers.get(pointer) != null) {
            Btn b = buttonAt(x, y);
            if (b != null) {
                buttonPointers.put(pointer, b);
            }
        }
    }

    private void end(int pointer) {
        for (Stick s : sticks) {
            if (pointer == s.pointer) {
                releaseStick(s);
            }
        }
        if (pointer == dpadPointer) {
            dpadPointer = -1;
            dpadBits = 0;
        }
        buttonPointers.remove(pointer);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        final int index = e.getActionIndex();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                begin(e.getPointerId(index), e.getX(index), e.getY(index), e.getEventTime());
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); ++i) {
                    move(e.getPointerId(i), e.getX(i), e.getY(i));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                end(e.getPointerId(index));
                break;
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                break;
            default:
                return true;
        }
        publish();
        return true;
    }
}
