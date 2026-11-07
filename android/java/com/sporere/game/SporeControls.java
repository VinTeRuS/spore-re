package com.sporere.game;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.InputMethodManager;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Touch controls for Spore, drawn over SDL's surface. The game is played with the mouse; touches become SDL mouse
 * input in one of two modes (settings):
 *   Direct:   the finger is the cursor. Tap = click, drag = left drag, long press = right click.
 *   Touchpad: a cursor on screen; dragging moves it, tap = click where it is, double tap and hold = left drag,
 *             long press = right click. Easier on Spore's small buttons.
 * In both: two fingers drag with the right button held (camera rotation), pinching turns the wheel (zoom).
 * A menu handle (draggable, so it can sit wherever the current stage leaves room) opens a row of buttons: Esc (the
 * game's menu, "back"), Cam (one-finger drags rotate the camera while on), WASD (the movement stick), Undo (Ctrl+Z, the
 * editors), the keyboard (names) and the gear: the settings panel. Engine settings (resolution, textures, frame limit, battery
 * saver) go to sporere.ini and apply on the next start; the rest apply at once.
 */
public class SporeControls extends View {
    private static final int LEFT = MotionEvent.BUTTON_PRIMARY, RIGHT = MotionEvent.BUTTON_SECONDARY;
    private static final int DOWN = MotionEvent.ACTION_DOWN, UP = MotionEvent.ACTION_UP, MOVE = MotionEvent.ACTION_MOVE;
    private static final long LONG_PRESS_MS = 500, LEFT_DELAY_MS = 60, DOUBLE_TAP_MS = 280;
    private static final int TEAL = 0xff40d6c8;

    private final float density;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final SharedPreferences prefs;

    // ---- settings ----
    /** A row of the settings panel; tapping it steps through the values. */
    private static final class Setting {
        final String label, key;
        final String[] names;
        final int[] values;
        final boolean engine;  // written to sporere.ini, applies on the next start
        int index;

        Setting(String label, String key, String[] names, int[] values, int def, boolean engine) {
            this.label = label;
            this.key = key;
            this.names = names;
            this.values = values;
            this.engine = engine;
            index = 0;
            for (int i = 0; i < values.length; ++i) if (values[i] == def) index = i;
        }

        int value() {
            return values[index];
        }
    }

    // Engine settings (*) go to sporere.ini and apply on the next start; the others at once.
    private final Setting sQuality = new Setting("Quality", "gpu_tier",
            new String[] {"Fast", "Balanced", "Pretty"}, new int[] {0, 1, 2}, 0, true);
    private final Setting sResolution = new Setting("UI size", "render_height",
            new String[] {"Large", "Medium", "Small"}, new int[] {540, 600, 720}, 600, true);
    private final Setting sFps = new Setting("FPS limit", "fps_limit",
            new String[] {"Off", "30", "60"}, new int[] {0, 30, 60}, 0, true);
    private final Setting sMode = new Setting("Touch mode", "ui_touchpad",
            new String[] {"Direct", "Touchpad"}, new int[] {0, 1}, 0, false);
    private final Setting sSpeed = new Setting("Camera speed", "ui_speed",
            new String[] {"Slow", "Normal", "Fast"}, new int[] {60, 100, 160}, 100, false);
    private final Setting sVibrate = new Setting("Vibration", "ui_vibrate",
            new String[] {"Off", "On"}, new int[] {0, 1}, 1, false);
    private final Setting sShowFps = new Setting("Show FPS", "ui_fps",
            new String[] {"Off", "On"}, new int[] {0, 1}, 0, false);
    private final Setting[] settings = {sQuality, sResolution, sFps, sMode, sSpeed, sVibrate, sShowFps};
    private final RectF[] settingRects = new RectF[settings.length];
    private boolean engineChanged;

    // ---- side buttons ----
    private static final class Button {
        final String label;
        final Runnable action;
        final RectF rect = new RectF();

        Button(String label, Runnable action) {
            this.label = label;
            this.action = action;
        }
    }

    private final List<Button> buttons = new ArrayList<>();
    private Button pressedButton;
    private boolean camLock;  // one-finger drags hold the right button (camera)
    private SporeStick stick;  // WASD movement stick (creature and tribal stages), shown with the WASD button
    // The menu handle: one round button the player can drag anywhere (Spore's own controls sit in different
    // corners in each stage); tapping it opens a row of buttons. Its place is kept as a fraction of the screen.
    private final RectF handle = new RectF();
    private float handleFx, handleFy;
    private boolean menuOpen, handleDragged;

    void setStick(SporeStick s) {
        stick = s;
    }

    // ---- gesture state ----
    private enum Mode { NONE, PENDING, LEFT_DRAG, CAM_DRAG, CURSOR, TWO_FINGER, BUTTON, PANEL, HANDLE }
    private Mode mode = Mode.NONE;
    private float downX, downY, lastX, lastY, lastMidX, lastMidY, lastSpan, wheelAccum;
    private float camX, camY;            // virtual pointer while rotating the camera (moves at the camera speed)
    private float cursorX = -1, cursorY;  // touchpad cursor
    private long lastTapTime;
    private boolean longPressFired, moved;
    private final Runnable longPress = this::onLongPress;
    private final Runnable leftDown = this::startLeft;

    // ---- overlay ----
    private boolean panelOpen;
    private final RectF panel = new RectF(), panelClose = new RectF(), panelSupport = new RectF();
    private final Runnable fpsTick = new Runnable() {
        @Override
        public void run() {
            invalidate();
            if (sShowFps.value() == 1) handler.postDelayed(this, 500);
        }
    };

    /** Frames per second over the last second (libmain.so, d3d9.cpp). */
    static native float nativeFps();

    public SporeControls(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        prefs = context.getSharedPreferences("controls", Context.MODE_PRIVATE);
        for (Setting s : settings) {
            int v = s.engine ? readIni(s.key, s.value()) : prefs.getInt(s.key, s.value());
            for (int i = 0; i < s.values.length; ++i) if (s.values[i] == v) s.index = i;
        }
        for (int i = 0; i < settingRects.length; ++i) settingRects[i] = new RectF();
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        handleFx = prefs.getFloat("ui_handle_x", 0.5f);
        handleFy = prefs.getFloat("ui_handle_y", 0.0f);
        buttons.add(new Button("Esc", () -> key(KeyEvent.KEYCODE_ESCAPE)));
        buttons.add(new Button("Cam", () -> {
            camLock = !camLock;
            invalidate();
        }));
        buttons.add(new Button("WASD", () -> {
            if (stick == null) return;
            if (stick.getVisibility() == View.VISIBLE) stick.hide();
            else stick.setVisibility(View.VISIBLE);
            invalidate();
        }));
        buttons.add(new Button("Undo", SporeControls::undo));
        buttons.add(new Button("⌨", this::toggleKeyboard));
        buttons.add(new Button("⚙", () -> {
            panelOpen = true;
            engineChanged = false;
            invalidate();
        }));
        if (sShowFps.value() == 1) handler.post(fpsTick);
    }

    // ---- input to SDL ----
    private static void mouse(int buttons, int action, float x, float y) {
        SDLActivity.onNativeMouse(buttons, action, x, y, false);
    }

    private static void key(int keycode) {
        SDLActivity.onNativeKeyDown(keycode);
        SDLActivity.onNativeKeyUp(keycode);
    }

    /** Ctrl+Z: undo in the editors. */
    private static void undo() {
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_CTRL_LEFT);
        key(KeyEvent.KEYCODE_Z);
        SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_CTRL_LEFT);
    }

    private static void wheel(float steps) {
        SDLActivity.onNativeMouse(0, MotionEvent.ACTION_SCROLL, 0, steps, false);
    }

    private void click(float x, float y, int button) {
        mouse(0, MOVE, x, y);
        mouse(button, DOWN, x, y);
        mouse(0, UP, x, y);
    }

    private void buzz(int kind) {
        if (sVibrate.value() == 1) performHapticFeedback(kind);
    }

    private float speed() {
        return sSpeed.value() / 100f;
    }

    private boolean touchpad() {
        return sMode.value() == 1;
    }

    private void toggleKeyboard() {
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm == null) return;
        setFocusableInTouchMode(true);
        requestFocus();
        imm.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0);
    }

    /** The pointer position a one-finger gesture acts at: the finger, or the touchpad cursor. */
    private float px(float x) {
        return touchpad() ? cursorX : x;
    }

    private float py(float y) {
        return touchpad() ? cursorY : y;
    }

    private void startLeft() {
        if (mode != Mode.PENDING) return;
        handler.removeCallbacks(longPress);
        if (camLock) {
            mode = Mode.CAM_DRAG;
            camX = px(downX);
            camY = py(downY);
            mouse(0, MOVE, camX, camY);
            mouse(RIGHT, DOWN, camX, camY);
            return;
        }
        mode = Mode.LEFT_DRAG;
        mouse(0, MOVE, px(downX), py(downY));
        mouse(LEFT, DOWN, px(downX), py(downY));
    }

    private void onLongPress() {
        if (mode != Mode.PENDING || moved) return;
        handler.removeCallbacks(leftDown);
        longPressFired = true;
        mode = Mode.NONE;
        buzz(HapticFeedbackConstants.LONG_PRESS);
        click(px(downX), py(downY), RIGHT);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        float x = e.getX(), y = e.getY();
        if (cursorX < 0) {
            cursorX = getWidth() / 2f;
            cursorY = getHeight() / 2f;
        }
        if (action == MotionEvent.ACTION_DOWN) {
            longPressFired = false;
            moved = false;
            if (panelOpen) {
                mode = Mode.PANEL;
                return true;
            }
            if (handle.contains(x, y)) {
                mode = Mode.HANDLE;
                handleDragged = false;
                downX = x;
                downY = y;
                return true;
            }
            Button b = menuOpen ? hitButton(x, y) : null;
            if (b != null) {
                pressedButton = b;
                mode = Mode.BUTTON;
                return true;
            }
            downX = lastX = x;
            downY = lastY = y;
            long now = System.currentTimeMillis();
            boolean doubleTap = touchpad() && now - lastTapTime < DOUBLE_TAP_MS;
            if (doubleTap) {  // touchpad: double tap and hold = drag with the left button
                mode = Mode.PENDING;
                startLeft();
                return true;
            }
            mode = touchpad() && !camLock ? Mode.CURSOR : Mode.PENDING;
            if (mode == Mode.PENDING && !touchpad()) handler.postDelayed(leftDown, LEFT_DELAY_MS * 3);
            handler.postDelayed(longPress, LONG_PRESS_MS);
            return true;
        }
        if (mode == Mode.PANEL) {
            if (action == MotionEvent.ACTION_UP) panelTap(x, y);
            return true;
        }
        if (mode == Mode.HANDLE) {
            if (action == MotionEvent.ACTION_MOVE && (handleDragged || Math.hypot(x - downX, y - downY) > 12 * density)) {
                handleDragged = true;
                handleFx = clamp(x / getWidth(), 0, 1);
                handleFy = clamp(y / getHeight(), 0, 1);
                invalidate();
            } else if (action == MotionEvent.ACTION_UP) {
                if (handleDragged) {
                    prefs.edit().putFloat("ui_handle_x", handleFx).putFloat("ui_handle_y", handleFy).apply();
                } else {
                    menuOpen = !menuOpen;
                    buzz(HapticFeedbackConstants.VIRTUAL_KEY);
                }
                mode = Mode.NONE;
                invalidate();
            }
            return true;
        }
        if (mode == Mode.BUTTON) {
            if (action == MotionEvent.ACTION_UP) {
                if (pressedButton != null && pressedButton.rect.contains(x, y)) {
                    buzz(HapticFeedbackConstants.VIRTUAL_KEY);
                    pressedButton.action.run();
                    // toggles keep the menu open; one-shot actions close it
                    if (!pressedButton.label.equals("Cam") && !pressedButton.label.equals("WASD")) menuOpen = false;
                    invalidate();
                }
                pressedButton = null;
                mode = Mode.NONE;
            }
            return true;
        }
        switch (action) {
            case MotionEvent.ACTION_POINTER_DOWN:
                if (e.getPointerCount() == 2) {
                    handler.removeCallbacks(leftDown);
                    handler.removeCallbacks(longPress);
                    if (mode == Mode.LEFT_DRAG || mode == Mode.CAM_DRAG) mouse(0, UP, lastMidX, lastMidY);
                    mode = Mode.TWO_FINGER;
                    lastMidX = (e.getX(0) + e.getX(1)) / 2;
                    lastMidY = (e.getY(0) + e.getY(1)) / 2;
                    camX = getWidth() / 2f;
                    camY = getHeight() / 2f;
                    lastSpan = span(e);
                    wheelAccum = 0;
                    mouse(0, MOVE, camX, camY);
                    mouse(RIGHT, DOWN, camX, camY);
                }
                return true;
            case MotionEvent.ACTION_MOVE:
                onMove(e, x, y);
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                if (mode == Mode.TWO_FINGER) {
                    mouse(0, UP, camX, camY);
                    mode = Mode.NONE;  // the remaining finger does nothing until lifted
                }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                handler.removeCallbacks(longPress);
                handler.removeCallbacks(leftDown);
                if (!longPressFired && !moved && (mode == Mode.PENDING || mode == Mode.CURSOR)) {
                    if (camLock) {
                        click(px(downX), py(downY), LEFT);
                    } else {
                        click(px(x), py(y), LEFT);
                    }
                    lastTapTime = System.currentTimeMillis();
                    buzz(HapticFeedbackConstants.KEYBOARD_TAP);
                } else if (mode == Mode.LEFT_DRAG) {
                    mouse(0, UP, px(x), py(y));
                } else if (mode == Mode.CAM_DRAG || mode == Mode.TWO_FINGER) {
                    mouse(0, UP, camX, camY);
                }
                if (!touchpad()) parkIfAtEdge(x, y);
                mode = Mode.NONE;
                invalidate();
                return true;
            default:
                return true;
        }
    }

    /**
     * In the tribal, civilization and space stages the camera scrolls while the mouse rests at a screen edge. With a
     * finger the "mouse" stays where it was lifted, so after a tap near an edge the camera kept sliding: the cursor
     * goes back to the middle of the screen instead.
     */
    private void parkIfAtEdge(float x, float y) {
        float mx = getWidth() * 0.06f, my = getHeight() * 0.08f;
        if (x > mx && x < getWidth() - mx && y > my && y < getHeight() - my) return;
        handler.postDelayed(() -> mouse(0, MOVE, getWidth() / 2f, getHeight() / 2f), 120);
    }

    private void onMove(MotionEvent e, float x, float y) {
        float dx = x - lastX, dy = y - lastY;
        if (!moved && Math.hypot(x - downX, y - downY) > 10 * density) {
            moved = true;
            handler.removeCallbacks(longPress);
        }
        switch (mode) {
            case TWO_FINGER: {
                if (e.getPointerCount() < 2) return;
                float mx = (e.getX(0) + e.getX(1)) / 2, my = (e.getY(0) + e.getY(1)) / 2;
                camX += (mx - lastMidX) * speed();
                camY += (my - lastMidY) * speed();
                lastMidX = mx;
                lastMidY = my;
                mouse(RIGHT, MOVE, camX, camY);
                float s = span(e);
                wheelAccum += (s - lastSpan) / (40 * density);  // one wheel step per 40 dp of pinch
                lastSpan = s;
                while (Math.abs(wheelAccum) >= 1) {
                    float step = Math.signum(wheelAccum);
                    wheel(step);
                    wheelAccum -= step;
                }
                break;
            }
            case CURSOR:
                cursorX = clamp(cursorX + dx * speed(), 0, getWidth() - 1);
                cursorY = clamp(cursorY + dy * speed(), 0, getHeight() - 1);
                mouse(0, MOVE, cursorX, cursorY);
                invalidate();
                break;
            case PENDING:
                if (moved) {
                    handler.removeCallbacks(leftDown);
                    startLeft();
                }
                break;
            case LEFT_DRAG:
                if (touchpad()) {
                    cursorX = clamp(cursorX + dx * speed(), 0, getWidth() - 1);
                    cursorY = clamp(cursorY + dy * speed(), 0, getHeight() - 1);
                    mouse(LEFT, MOVE, cursorX, cursorY);
                    invalidate();
                } else {
                    mouse(LEFT, MOVE, x, y);
                }
                break;
            case CAM_DRAG:
                camX += dx * speed();
                camY += dy * speed();
                mouse(RIGHT, MOVE, camX, camY);
                break;
            default:
                break;
        }
        lastX = x;
        lastY = y;
    }

    private static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }

    private static float span(MotionEvent e) {
        return (float) Math.hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1));
    }

    private Button hitButton(float x, float y) {
        for (Button b : buttons) if (b.rect.contains(x, y)) return b;
        return null;
    }

    // ---- settings panel ----
    private void panelTap(float x, float y) {
        if (panelClose.contains(x, y) || !panel.contains(x, y)) {
            panelOpen = false;
            if (engineChanged) {
                android.widget.Toast.makeText(getContext(), "Restart the game to apply the * settings",
                        android.widget.Toast.LENGTH_LONG).show();
            }
            invalidate();
            return;
        }
        if (panelSupport.contains(x, y)) {
            SetupActivity.openSupport(getContext());
            return;
        }
        for (int i = 0; i < settings.length; ++i) {
            if (!settingRects[i].contains(x, y)) continue;
            Setting s = settings[i];
            s.index = (s.index + 1) % s.values.length;
            buzz(HapticFeedbackConstants.KEYBOARD_TAP);
            if (s.engine) {
                writeIni(s.key, s.value());
                // Quality also picks the texture detail: Pretty loads full textures, the others skip the top mip
                if (s == sQuality) writeIni("texture_skip_mips", s.value() == 2 ? 0 : 1);
                engineChanged = true;
            } else {
                prefs.edit().putInt(s.key, s.value()).apply();
                if (s == sShowFps && s.value() == 1) handler.post(fpsTick);
            }
            invalidate();
            return;
        }
    }

    private File iniFile() {
        File dir = getContext().getExternalFilesDir(null);
        return dir == null ? null : new File(dir, "sporere.ini");
    }

    private int readIni(String key, int def) {
        File ini = iniFile();
        if (ini == null) return def;
        try (BufferedReader r = new BufferedReader(new FileReader(ini))) {
            for (String l; (l = r.readLine()) != null;) {
                String[] kv = l.split("=", 2);
                if (kv.length == 2 && kv[0].trim().equalsIgnoreCase(key)) return Integer.parseInt(kv[1].trim());
            }
        } catch (IOException | NumberFormatException ignored) {
            // no setting yet
        }
        return def;
    }

    private void writeIni(String key, int value) {
        File ini = iniFile();
        if (ini == null) return;
        List<String> lines = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(ini))) {
            for (String l; (l = r.readLine()) != null;) {
                if (!l.split("=", 2)[0].trim().equalsIgnoreCase(key)) lines.add(l);
            }
        } catch (IOException ignored) {
            // a new file
        }
        lines.add(key + " = " + value);
        try (FileWriter w = new FileWriter(ini)) {
            for (String l : lines) w.write(l + "\n");
        } catch (IOException ignored) {
            // the game keeps its defaults
        }
    }

    // ---- drawing ----
    @Override
    protected void onDraw(Canvas c) {
        float size = 44 * density, gap = 8 * density, r = size / 2;
        float hx = clamp(handleFx * getWidth(), r + 4 * density, getWidth() - r - 4 * density);
        float hy = clamp(handleFy * getHeight(), r + 4 * density, getHeight() - r - 4 * density);
        handle.set(hx - r, hy - r, hx + r, hy + r);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(menuOpen ? 0xd040d6c8 : 0x900a1a2a);
        c.drawCircle(hx, hy, r, fill);
        fill.setStyle(Paint.Style.STROKE);
        fill.setStrokeWidth(2 * density);
        fill.setColor(TEAL);
        c.drawCircle(hx, hy, r, fill);
        fill.setStyle(Paint.Style.FILL);
        text.setColor(Color.WHITE);
        text.setTextSize(20 * density);
        c.drawText(menuOpen ? "\u00d7" : "\u2630", hx, hy + 7 * density, text);
        // state dots: camera lock, WASD stick
        boolean wasd = stick != null && stick.getVisibility() == View.VISIBLE;
        if (camLock || wasd) {
            fill.setColor(0xffffd24a);
            if (camLock) c.drawCircle(hx - r * 0.55f, hy - r * 0.75f, 4 * density, fill);
            if (wasd) c.drawCircle(hx + r * 0.55f, hy - r * 0.75f, 4 * density, fill);
        }
        // the menu row opens toward the wider side of the screen, beside the handle
        boolean toLeft = hx > getWidth() / 2f;
        float bx = toLeft ? hx - r - gap - size : hx + r + gap;
        float by = hy - r;
        text.setTextSize(14 * density);
        for (Button b : buttons) {
            if (!menuOpen) {
                b.rect.setEmpty();
                continue;
            }
            b.rect.set(bx, by, bx + size, by + size);
            boolean on = (b.label.equals("Cam") && camLock) || (b.label.equals("WASD") && wasd);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(on ? 0xd040d6c8 : 0xc00a1a2a);
            c.drawRoundRect(b.rect, 12 * density, 12 * density, fill);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(1.5f * density);
            fill.setColor(TEAL);
            c.drawRoundRect(b.rect, 12 * density, 12 * density, fill);
            fill.setStyle(Paint.Style.FILL);
            c.drawText(b.label, b.rect.centerX(), b.rect.centerY() + 5 * density, text);
            bx += toLeft ? -(size + gap) : size + gap;
        }
        float x = 8 * density;
        text.setColor(Color.WHITE);
        if (touchpad() && cursorX >= 0) {  // the touchpad cursor
            fill.setColor(0xccffffff);
            c.drawCircle(cursorX, cursorY, 7 * density, fill);
            fill.setColor(0xff102030);
            c.drawCircle(cursorX, cursorY, 3 * density, fill);
        }
        if (sShowFps.value() == 1) {
            text.setTextSize(13 * density);
            text.setTextAlign(Paint.Align.LEFT);
            c.drawText(String.format(Locale.ROOT, "%.0f fps", nativeFps()), x, getHeight() - 10 * density, text);
            text.setTextAlign(Paint.Align.CENTER);
        }
        if (panelOpen) drawPanel(c);
    }

    private void drawPanel(Canvas c) {
        float w = Math.min(getWidth() - 32 * density, 640 * density);
        float row = 38 * density, pad = 16 * density, header = 64 * density;
        int rows = (settings.length + 1) / 2;
        float h = header + rows * (row + 6 * density) + 70 * density;
        float left = (getWidth() - w) / 2, top = Math.max(8 * density, (getHeight() - h) / 2);
        panel.set(left, top, left + w, top + h);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0xf00b1626);
        c.drawRoundRect(panel, 18 * density, 18 * density, fill);
        fill.setStyle(Paint.Style.STROKE);
        fill.setStrokeWidth(2 * density);
        fill.setColor(TEAL);
        c.drawRoundRect(panel, 18 * density, 18 * density, fill);
        fill.setStyle(Paint.Style.FILL);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTextSize(18 * density);
        c.drawText("⚙  Settings", panel.centerX(), top + 30 * density, text);
        text.setTextSize(12 * density);
        text.setColor(0xffa8c8d8);
        c.drawText(SetupActivity.CREDIT + "   ·   * = after restart", panel.centerX(), top + 50 * density, text);
        text.setColor(Color.WHITE);
        float colW = (w - pad * 3) / 2;
        for (int i = 0; i < settings.length; ++i) {
            int col = i / rows, r = i % rows;
            float rx = left + pad + col * (colW + pad), ry = top + header + r * (row + 6 * density);
            RectF rect = settingRects[i];
            rect.set(rx, ry, rx + colW, ry + row);
            fill.setColor(0xff17304a);
            c.drawRoundRect(rect, 10 * density, 10 * density, fill);
            Setting s = settings[i];
            text.setTextSize(13 * density);
            text.setTextAlign(Paint.Align.LEFT);
            c.drawText(s.label + (s.engine ? " *" : ""), rect.left + 12 * density, rect.centerY() + 5 * density, text);
            text.setTextAlign(Paint.Align.RIGHT);
            text.setColor(TEAL);
            c.drawText(s.names[s.index], rect.right - 12 * density, rect.centerY() + 5 * density, text);
            text.setColor(Color.WHITE);
        }
        text.setTextAlign(Paint.Align.CENTER);
        float by = top + h - 52 * density, bw = (w - pad * 3) / 2;
        panelSupport.set(left + pad, by, left + pad + bw, by + 38 * density);
        panelClose.set(left + pad * 2 + bw, by, left + pad * 2 + bw * 2, by + 38 * density);
        fill.setColor(0xff8a3a7a);
        c.drawRoundRect(panelSupport, 10 * density, 10 * density, fill);
        fill.setColor(0xff1d5a6a);
        c.drawRoundRect(panelClose, 10 * density, 10 * density, fill);
        text.setTextSize(14 * density);
        c.drawText("Support on Patreon", panelSupport.centerX(), panelSupport.centerY() + 5 * density, text);
        c.drawText("Close", panelClose.centerX(), panelClose.centerY() + 5 * density, text);
    }

    /** The system back button opens the game's menu, like Esc. */
    static boolean onBack(Activity a) {
        key(KeyEvent.KEYCODE_ESCAPE);
        return true;
    }
}
