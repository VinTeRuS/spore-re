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
 * Each stage gets its own layout (the game mode comes from libmain.so): creature: the WASD stick and ability keys 1-4;
 * tribe and civilization: RMB (taps give orders); space: tool keys 1-4 and RMB; editors: Undo and Redo.
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
    // The game draws its interface in pixels of its resolution: a higher one is sharper, with a smaller interface.
    private final Setting sResolution = new Setting("Resolution", "render_height",
            new String[] {"540p", "600p", "720p", "900p", "1080p"}, new int[] {540, 600, 720, 900, 1080}, 600, true);
    private final Setting sFps = new Setting("FPS limit", "fps_limit",
            new String[] {"Off", "30", "60"}, new int[] {0, 30, 60}, 60, true);
    // Off: the CPU prepares the next frame while the GPU draws (smoother on most phones); On: the game's own wait.
    private final Setting sGpuSync = new Setting("Wait for GPU", "gpu_sync",
            new String[] {"Off", "On"}, new int[] {0, 1}, 0, true);
    private final Setting sMode = new Setting("Touch mode", "ui_touchpad",
            new String[] {"Direct", "Touchpad"}, new int[] {0, 1}, 0, false);
    private final Setting sSpeed = new Setting("Camera speed", "ui_speed",
            new String[] {"Slow", "Normal", "Fast"}, new int[] {60, 100, 160}, 100, false);
    private final Setting sVibrate = new Setting("Vibration", "ui_vibrate",
            new String[] {"Off", "On"}, new int[] {0, 1}, 1, false);
    private final Setting sShowFps = new Setting("Show FPS", "ui_fps",
            new String[] {"Off", "On"}, new int[] {0, 1}, 0, false);
    private final Setting[] settings = {sQuality, sResolution, sFps, sGpuSync, sMode, sSpeed, sVibrate, sShowFps};
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

    // ---- per-stage layout ----
    // The active game mode (libmain.so reads it from the game once a frame) picks a set of action buttons along the
    // right edge, whether the WASD stick shows (remembered per stage once toggled) and whether taps give orders
    // (right clicks) in the strategy stages.
    private enum Stage { OTHER, CELL, CREATURE, TRIBE, CIV, SPACE, EDITOR }
    private Stage stage = Stage.OTHER;
    private final List<Button> actions = new ArrayList<>();
    private boolean rightTaps;  // Order button: one-finger taps are right clicks (orders to the selected units)
    private boolean boxArmed;   // Select button: the next one-finger drag is a left drag (box selection)
    private String hint;        // what the gestures do in this stage, shown for a few seconds after it starts
    private final Runnable hideHint = () -> {
        hint = null;
        invalidate();
    };

    /** In the 3D stages a one-finger drag turns the camera (right drag); elsewhere it drags with the left button. */
    private boolean dragTurnsCamera() {
        if (camLock) return true;
        if (boxArmed) return false;
        return stage == Stage.CREATURE || stage == Stage.TRIBE || stage == Stage.CIV || stage == Stage.SPACE;
    }

    private static boolean russian() {
        return "ru".equals(Locale.getDefault().getLanguage()) || "uk".equals(Locale.getDefault().getLanguage());
    }

    private static String hintFor(Stage s) {
        boolean ru = russian();
        switch (s) {
            case CELL:
                return ru ? "Клетка: тап или веди пальцем — плыть, щипок — зум"
                          : "Cell: tap or drag to swim, pinch to zoom";
            case CREATURE:
                return ru ? "Существо: тап — идти / действовать, палец — камера, стик — ходьба, 1–4 — способности"
                          : "Creature: tap to walk / act, drag to turn the camera, stick to walk, 1-4 abilities";
            case TRIBE:
            case CIV:
                return ru ? "Тап — выбрать, «Приказ» + тап — приказ, «Выбор» + палец — рамка, стик — камера"
                          : "Tap to select, Order + tap to command, Select + drag for a box, stick moves the camera";
            case SPACE:
                return ru ? "Космос: тап — лететь / выбрать, палец — камера, 1–4 — инструменты, «Приказ» — правый клик"
                          : "Space: tap to fly / pick, drag to turn the camera, 1-4 tools, Order = right click";
            case EDITOR:
                return ru ? "Редактор: тяни детали пальцем, два пальца — камера, щипок — зум"
                          : "Editor: drag parts, two fingers turn the camera, pinch to zoom";
            default:
                return null;
        }
    }

    /** The active game mode id (App::cGameModeManager), 0 if none. */
    static native int nativeGameMode();

    private static Stage stageOf(int mode) {
        switch (mode) {
            case 0x1654c00: return Stage.CELL;
            case 0x1654c01: return Stage.CREATURE;
            case 0x1654c02: return Stage.TRIBE;
            case 0x1654c04: return Stage.CIV;
            case 0x1654c05: return Stage.SPACE;
            case 0x0dbdba1: return Stage.EDITOR;
            default: return Stage.OTHER;
        }
    }

    private final Runnable modeTick = new Runnable() {
        @Override
        public void run() {
            Stage s = Stage.OTHER;
            try {
                s = stageOf(nativeGameMode());
            } catch (UnsatisfiedLinkError ignored) {
                // the library is not loaded yet
            }
            if (s != stage) enterStage(s);
            handler.postDelayed(this, 500);
        }
    };

    private void enterStage(Stage s) {
        stage = s;
        rightTaps = false;
        boxArmed = false;
        actions.clear();
        hint = hintFor(s);
        handler.removeCallbacks(hideHint);
        if (hint != null) handler.postDelayed(hideHint, 6000);
        switch (s) {
            case CREATURE:  // abilities on 1-4 (attack or social, the game swaps the bar), WASD + jump on the stick
                for (int k = 1; k <= 4; ++k) actions.add(keyButton(String.valueOf(k), KeyEvent.KEYCODE_0 + k));
                break;
            case TRIBE:
            case CIV:
                actions.add(new Button(russian() ? "Приказ" : "Order", this::toggleRightTaps));
                actions.add(new Button(russian() ? "Выбор" : "Select", this::armBox));
                break;
            case SPACE:  // tools on the number keys
                for (int k = 1; k <= 4; ++k) actions.add(keyButton(String.valueOf(k), KeyEvent.KEYCODE_0 + k));
                actions.add(new Button(russian() ? "Приказ" : "Order", this::toggleRightTaps));
                break;
            case EDITOR:
                actions.add(new Button("Undo", SporeControls::undo));
                actions.add(new Button("Redo", SporeControls::redo));
                break;
            default:
                break;
        }
        if (stick != null) {
            boolean show = prefs.getBoolean("stick_" + s.name(),
                    s == Stage.CREATURE || s == Stage.TRIBE || s == Stage.CIV);
            if (show) stick.setVisibility(View.VISIBLE);
            else if (stick.getVisibility() == View.VISIBLE) stick.hide();
        }
        invalidate();
    }

    private Button keyButton(String label, int keycode) {
        return new Button(label, () -> {
            SDLActivity.onNativeKeyDown(keycode);
            handler.postDelayed(() -> SDLActivity.onNativeKeyUp(keycode), 120);
        });
    }

    private void toggleRightTaps() {
        rightTaps = !rightTaps;
        invalidate();
    }

    private void armBox() {
        boxArmed = !boxArmed;
        invalidate();
    }

    private Button hitAction(float x, float y) {
        for (Button b : actions) if (b.rect.contains(x, y)) return b;
        return null;
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
        buttons.add(new Button("WASD", () -> {
            if (stick == null) return;
            boolean show = stick.getVisibility() != View.VISIBLE;
            if (show) stick.setVisibility(View.VISIBLE);
            else stick.hide();
            prefs.edit().putBoolean("stick_" + stage.name(), show).apply();
            invalidate();
        }));
        buttons.add(new Button("Stages", this::unlockStages));
        buttons.add(new Button("⌨", this::toggleKeyboard));
        buttons.add(new Button("⚙", () -> {
            panelOpen = true;
            engineChanged = false;
            invalidate();
        }));
        if (sShowFps.value() == 1) handler.post(fpsTick);
        handler.postDelayed(modeTick, 2000);
    }

    // ---- input to SDL ----
    private static void mouse(int buttons, int action, float x, float y) {
        SDLActivity.onNativeMouse(buttons, action, x, y, false);
    }

    private static void key(int keycode) {
        SDLActivity.onNativeKeyDown(keycode);
        SDLActivity.onNativeKeyUp(keycode);
    }

    /**
     * Opens every stage for a new game (normally a stage is playable only after reaching it): Spore's own cheat
     * console (Ctrl+Shift+C) with "levels -unlock", typed as text. Meant for the galaxy / main menu.
     */
    private void unlockStages() {
        // Ctrl and Shift stay down while the game handles the C (sent at once, the game saw a plain C: Spore's
        // screenshot key), then the command is typed as text.
        Runnable comboDown = () -> {
            SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_CTRL_LEFT);
            SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_SHIFT_LEFT);
        };
        Runnable comboUp = () -> {
            SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_SHIFT_LEFT);
            SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_CTRL_LEFT);
        };
        handler.post(comboDown);
        handler.postDelayed(() -> SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_C), 150);
        handler.postDelayed(() -> SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_C), 350);
        handler.postDelayed(comboUp, 500);
        handler.postDelayed(() -> org.libsdl.app.SporeText.type("levels -unlock"), 1000);
        handler.postDelayed(() -> SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_ENTER), 1400);
        handler.postDelayed(() -> SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_ENTER), 1550);
        // close the console the same way
        handler.postDelayed(comboDown, 2200);
        handler.postDelayed(() -> SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_C), 2350);
        handler.postDelayed(() -> SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_C), 2550);
        handler.postDelayed(comboUp, 2700);
        android.widget.Toast.makeText(getContext(), "All stages unlocked (cheat console: levels -unlock)",
                android.widget.Toast.LENGTH_LONG).show();
    }

    /** Ctrl+Z: undo in the editors. */
    private static void undo() {
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_CTRL_LEFT);
        key(KeyEvent.KEYCODE_Z);
        SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_CTRL_LEFT);
    }

    /** Ctrl+Y: redo in the editors. */
    private static void redo() {
        SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_CTRL_LEFT);
        key(KeyEvent.KEYCODE_Y);
        SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_CTRL_LEFT);
    }

    private static void wheel(float steps) {
        SDLActivity.onNativeMouse(0, MotionEvent.ACTION_SCROLL, 0, steps, false);
    }

    /**
     * A click the game sees whichever way it reads the mouse. Menus use the window messages, but picking things in
     * the 3D world (stars in the galaxy, creatures, buildings) polls the button state once per frame: a press and
     * release within one frame never shows there. So the cursor moves first (hover), the button goes down a little
     * later and stays down for a few frames (150 ms: several frames even at a low frame rate).
     */
    private void click(float x, float y, int button) {
        mouse(0, MOVE, x, y);
        handler.postDelayed(() -> mouse(button, DOWN, x, y), 60);
        handler.postDelayed(() -> mouse(0, UP, x, y), 210);
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
        if (dragTurnsCamera()) {
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
            if (b == null) b = hitAction(x, y);
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
            mode = touchpad() && !dragTurnsCamera() ? Mode.CURSOR : Mode.PENDING;
            // direct mode: the cursor goes under the finger at once, so the game sees it hover over what will be
            // tapped for as long as the finger rests (3D picking needs a few frames of hover before the press)
            if (!touchpad()) mouse(0, MOVE, x, y);
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
                    // the menu stays open (Esc is often pressed twice in a row); the handle closes it, the gear
                    // opens the settings over it
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
                    int button = rightTaps ? RIGHT : LEFT;
                    click(px(x), py(y), button);
                    lastTapTime = System.currentTimeMillis();
                    buzz(HapticFeedbackConstants.KEYBOARD_TAP);
                } else if (mode == Mode.LEFT_DRAG) {
                    mouse(0, UP, px(x), py(y));
                    boxArmed = false;  // the Select button arms one box drag
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
        drawActions(c, size, gap);
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
        if (hint != null && !panelOpen) {
            text.setTextSize(15 * density);
            float w = Math.min(getWidth() - 32 * density, text.measureText(hint) + 40 * density);
            float top = 12 * density, h = 40 * density;
            RectF r = new RectF((getWidth() - w) / 2, top, (getWidth() + w) / 2, top + h);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(0xd00a1a2a);
            c.drawRoundRect(r, 14 * density, 14 * density, fill);
            text.setColor(Color.WHITE);
            c.drawText(hint, r.centerX(), r.centerY() + 5 * density, text);
            text.setTextSize(14 * density);
        }
        if (panelOpen) drawPanel(c);
    }

    /** The stage's action buttons: a column at the right edge, centred vertically. */
    private void drawActions(Canvas c, float size, float gap) {
        if (actions.isEmpty()) return;
        float big = size * 1.15f;
        float total = actions.size() * big + (actions.size() - 1) * gap;
        float bx = getWidth() - big - 14 * density, by = (getHeight() - total) / 2;
        text.setTextSize(16 * density);
        for (Button b : actions) {
            b.rect.set(bx, by, bx + big, by + big);
            boolean on = (b.action == null) ? false : ((b.label.equals("Order") || b.label.equals("Приказ")) && rightTaps)
                    || ((b.label.equals("Select") || b.label.equals("Выбор")) && boxArmed);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(on ? 0xd0ffb03a : 0x900a1a2a);
            c.drawOval(b.rect, fill);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(2 * density);
            fill.setColor(on ? 0xffffb03a : TEAL);
            c.drawOval(b.rect, fill);
            fill.setStyle(Paint.Style.FILL);
            text.setColor(Color.WHITE);
            c.drawText(b.label, b.rect.centerX(), b.rect.centerY() + 6 * density, text);
            by += big + gap;
        }
        text.setTextSize(14 * density);
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
