package com.sporere.game;

import android.app.Activity;
import android.content.Context;
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
 * input:
 *   one finger: left button (tap = click, drag = drag); a long press without moving = right click;
 *   two fingers: the midpoint drags with the right button held (camera rotation in Spore), pinching turns the mouse
 *   wheel (zoom).
 * A column of small buttons on the left edge: Esc (the game's menu and "back"), the keyboard (names of creatures,
 * planets...), an FPS counter and the settings panel (quality presets: render resolution and frame limit; credit).
 */
public class SporeControls extends View {
    // MotionEvent button states and actions as SDL's onNativeMouse expects them.
    private static final int LEFT = MotionEvent.BUTTON_PRIMARY, RIGHT = MotionEvent.BUTTON_SECONDARY;
    private static final int DOWN = MotionEvent.ACTION_DOWN, UP = MotionEvent.ACTION_UP, MOVE = MotionEvent.ACTION_MOVE;
    private static final long LONG_PRESS_MS = 550, LEFT_DELAY_MS = 70;

    private final float density;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Handler handler = new Handler(Looper.getMainLooper());

    /** An on-screen button: label, action, and where it was last drawn. */
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

    // gesture state
    private enum Mode { NONE, PENDING, LEFT_DRAG, TWO_FINGER, BUTTON }
    private Mode mode = Mode.NONE;
    private float downX, downY, lastMidX, lastMidY, lastSpan;
    private float wheelAccum;
    private boolean longPressFired;
    private final Runnable longPress = this::onLongPress;
    private final Runnable leftDown = this::startLeft;

    // overlay state
    private boolean showFps;
    private boolean panelOpen;
    private final RectF panel = new RectF();
    private final List<Button> panelButtons = new ArrayList<>();
    private final Runnable fpsTick = new Runnable() {
        @Override
        public void run() {
            if (showFps) {
                invalidate();
                handler.postDelayed(this, 500);
            }
        }
    };

    /** Frames per second over the last second (libmain.so, d3d9.cpp). */
    static native float nativeFps();

    public SporeControls(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        buttons.add(new Button("Esc", () -> key(KeyEvent.KEYCODE_ESCAPE)));
        buttons.add(new Button("⌨", this::toggleKeyboard));
        buttons.add(new Button("FPS", () -> {
            showFps = !showFps;
            if (showFps) handler.post(fpsTick);
            invalidate();
        }));
        buttons.add(new Button("⚙", () -> {
            panelOpen = !panelOpen;
            invalidate();
        }));
        String[][] presets = {
            {"Performance", "540", "30"}, {"Balanced", "720", "60"}, {"Quality", "900", "60"},
        };
        for (String[] p : presets) {
            panelButtons.add(new Button(p[0] + "  (" + p[1] + "p, " + p[2] + " fps)", () -> applyPreset(p[1], p[2])));
        }
        panelButtons.add(new Button("Support on Patreon", () -> SetupActivity.openSupport(getContext())));
        panelButtons.add(new Button("Close", () -> {
            panelOpen = false;
            invalidate();
        }));
    }

    // ---- input to SDL ----
    private static void mouse(int buttons, int action, float x, float y) {
        SDLActivity.onNativeMouse(buttons, action, x, y, false);
    }

    private static void key(int keycode) {
        SDLActivity.onNativeKeyDown(keycode);
        SDLActivity.onNativeKeyUp(keycode);
    }

    private void wheel(float steps) {
        SDLActivity.onNativeMouse(0, MotionEvent.ACTION_SCROLL, 0, steps, false);
    }

    private void toggleKeyboard() {
        InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) imm.toggleSoftInput(InputMethodManager.SHOW_FORCED, 0);
    }

    private void startLeft() {
        if (mode != Mode.PENDING) return;
        mode = Mode.LEFT_DRAG;
        mouse(0, MOVE, downX, downY);
        mouse(LEFT, DOWN, downX, downY);
    }

    private void onLongPress() {
        if (mode != Mode.PENDING && mode != Mode.LEFT_DRAG) return;
        if (mode == Mode.LEFT_DRAG) mouse(0, UP, downX, downY);
        longPressFired = true;
        mode = Mode.NONE;
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
        mouse(RIGHT, DOWN, downX, downY);
        mouse(0, UP, downX, downY);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        float x = e.getX(), y = e.getY();
        if (action == MotionEvent.ACTION_DOWN) {
            longPressFired = false;
            Button b = hit(x, y);
            if (b != null) {
                pressedButton = b;
                mode = Mode.BUTTON;
                return true;
            }
            downX = x;
            downY = y;
            mode = Mode.PENDING;
            handler.postDelayed(leftDown, LEFT_DELAY_MS);
            handler.postDelayed(longPress, LONG_PRESS_MS);
            return true;
        }
        if (mode == Mode.BUTTON) {
            if (action == MotionEvent.ACTION_UP) {
                if (pressedButton != null && pressedButton.rect.contains(x, y)) {
                    performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                    pressedButton.action.run();
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
                    if (mode == Mode.LEFT_DRAG) mouse(0, UP, x, y);
                    mode = Mode.TWO_FINGER;
                    lastMidX = (e.getX(0) + e.getX(1)) / 2;
                    lastMidY = (e.getY(0) + e.getY(1)) / 2;
                    lastSpan = span(e);
                    wheelAccum = 0;
                    mouse(0, MOVE, lastMidX, lastMidY);
                    mouse(RIGHT, DOWN, lastMidX, lastMidY);
                }
                return true;
            case MotionEvent.ACTION_MOVE:
                if (mode == Mode.TWO_FINGER && e.getPointerCount() >= 2) {
                    float mx = (e.getX(0) + e.getX(1)) / 2, my = (e.getY(0) + e.getY(1)) / 2;
                    mouse(RIGHT, MOVE, mx, my);
                    lastMidX = mx;
                    lastMidY = my;
                    float s = span(e);
                    wheelAccum += (s - lastSpan) / (40 * density);  // one wheel step per 40 dp of pinch
                    lastSpan = s;
                    while (Math.abs(wheelAccum) >= 1) {
                        float step = Math.signum(wheelAccum);
                        wheel(step);
                        wheelAccum -= step;
                    }
                } else if (mode == Mode.PENDING || mode == Mode.LEFT_DRAG) {
                    if (mode == Mode.PENDING && Math.hypot(x - downX, y - downY) > 12 * density) {
                        handler.removeCallbacks(leftDown);
                        handler.removeCallbacks(longPress);
                        startLeft();
                    }
                    if (mode == Mode.LEFT_DRAG) {
                        handler.removeCallbacks(longPress);
                        mouse(LEFT, MOVE, x, y);
                    }
                }
                return true;
            case MotionEvent.ACTION_POINTER_UP:
                if (mode == Mode.TWO_FINGER) {
                    mouse(0, UP, lastMidX, lastMidY);
                    mode = Mode.NONE;  // the remaining finger does nothing until lifted
                }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                handler.removeCallbacks(longPress);
                if (mode == Mode.PENDING && !longPressFired) {  // a quick tap: click
                    handler.removeCallbacks(leftDown);
                    mouse(0, MOVE, downX, downY);
                    mouse(LEFT, DOWN, downX, downY);
                    mouse(0, UP, downX, downY);
                } else if (mode == Mode.LEFT_DRAG) {
                    mouse(0, UP, x, y);
                } else if (mode == Mode.TWO_FINGER) {
                    mouse(0, UP, lastMidX, lastMidY);
                }
                mode = Mode.NONE;
                return true;
            default:
                return true;
        }
    }

    private static float span(MotionEvent e) {
        return (float) Math.hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1));
    }

    private Button hit(float x, float y) {
        if (panelOpen) {
            for (Button b : panelButtons) if (b.rect.contains(x, y)) return b;
            if (panel.contains(x, y)) return new Button("", () -> {});  // swallow taps on the panel
        }
        for (Button b : buttons) if (b.rect.contains(x, y)) return b;
        return null;
    }

    // ---- settings (sporere.ini next to the game data: read by the native side at start) ----
    private void applyPreset(String renderHeight, String fpsLimit) {
        File dir = getContext().getExternalFilesDir(null);
        if (dir == null) return;
        File ini = new File(dir, "sporere.ini");
        List<String> lines = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(ini))) {
            for (String l; (l = r.readLine()) != null;) {
                String k = l.split("=", 2)[0].trim().toLowerCase(Locale.ROOT);
                if (!k.equals("render_height") && !k.equals("fps_limit")) lines.add(l);
            }
        } catch (IOException ignored) {
            // no settings yet
        }
        lines.add("render_height = " + renderHeight);
        lines.add("fps_limit = " + fpsLimit);
        try (FileWriter w = new FileWriter(ini)) {
            for (String l : lines) w.write(l + "\n");
        } catch (IOException ignored) {
            return;
        }
        android.widget.Toast.makeText(getContext(), "Saved. Restart the game to apply.", android.widget.Toast.LENGTH_LONG)
                .show();
        panelOpen = false;
        invalidate();
    }

    // ---- drawing ----
    @Override
    protected void onDraw(Canvas c) {
        float size = 40 * density, gap = 10 * density, x = 8 * density;
        float y = getHeight() / 2f - (buttons.size() * (size + gap) - gap) / 2;
        text.setTextSize(15 * density);
        for (Button b : buttons) {
            b.rect.set(x, y, x + size, y + size);
            fill.setColor(0x6a0a1a2a);
            c.drawRoundRect(b.rect, 10 * density, 10 * density, fill);
            fill.setColor(0x8064d6c8);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(1.5f * density);
            c.drawRoundRect(b.rect, 10 * density, 10 * density, fill);
            fill.setStyle(Paint.Style.FILL);
            c.drawText(b.label, b.rect.centerX(), b.rect.centerY() + 5 * density, text);
            y += size + gap;
        }
        if (showFps) {
            text.setTextSize(13 * density);
            text.setTextAlign(Paint.Align.LEFT);
            c.drawText(String.format(Locale.ROOT, "%.0f fps", nativeFps()), x + size + gap, 22 * density, text);
            text.setTextAlign(Paint.Align.CENTER);
        }
        if (panelOpen) drawPanel(c);
    }

    private void drawPanel(Canvas c) {
        float w = 360 * density, row = 44 * density, pad = 14 * density;
        float h = pad * 2 + 56 * density + panelButtons.size() * (row + 6 * density);
        float left = (getWidth() - w) / 2, top = (getHeight() - h) / 2;
        panel.set(left, top, left + w, top + h);
        fill.setColor(0xee0b1626);
        c.drawRoundRect(panel, 16 * density, 16 * density, fill);
        text.setTextSize(16 * density);
        c.drawText("Spore — settings", panel.centerX(), top + pad + 16 * density, text);
        text.setTextSize(12 * density);
        text.setColor(0xffa8c8d8);
        c.drawText(SetupActivity.CREDIT, panel.centerX(), top + pad + 38 * density, text);
        text.setColor(Color.WHITE);
        text.setTextSize(14 * density);
        float y = top + pad + 56 * density;
        for (Button b : panelButtons) {
            b.rect.set(left + pad, y, left + w - pad, y + row);
            fill.setColor(0xff1d3a52);
            c.drawRoundRect(b.rect, 10 * density, 10 * density, fill);
            c.drawText(b.label, b.rect.centerX(), b.rect.centerY() + 5 * density, text);
            y += row + 6 * density;
        }
    }

    /** The system back button opens the game's menu, like Esc. */
    static boolean onBack(Activity a) {
        key(KeyEvent.KEYCODE_ESCAPE);
        return true;
    }
}
