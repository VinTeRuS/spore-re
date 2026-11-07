package com.sporere.game;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

/**
 * A movement stick for the creature and tribal stages, which walk with W/A/S/D: a ring in the lower left with a knob
 * that holds the keys of its direction (diagonals hold two), and a jump button (Space) to its right. A view of its
 * own, so a finger on the stick and another one tapping the game are separate touch streams. Shown with the WASD
 * button of SporeControls.
 */
public class SporeStick extends View {
    private static final int TEAL = 0xff40d6c8;
    private static final int[] KEYS = {KeyEvent.KEYCODE_W, KeyEvent.KEYCODE_A, KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_D};

    private final float density;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final boolean[] held = new boolean[4];  // W A S D
    private float cx, cy, radius, knobX, knobY, jumpX, jumpY, jumpR;
    private int stickPointer = -1, jumpPointer = -1;

    public SporeStick(Context context) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        setVisibility(GONE);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        radius = 62 * density;
        cx = 70 * density + radius;  // clear of SporeControls' button column
        cy = h - 30 * density - radius;
        knobX = cx;
        knobY = cy;
        jumpR = 34 * density;
        jumpX = cx + radius + 40 * density + jumpR;
        jumpY = cy + radius - jumpR;
    }

    /** Only the stick and jump areas take touches; everything else falls through to the controls below. */
    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN && !inStick(e.getX(), e.getY()) && !inJump(e.getX(), e.getY()))
            return false;
        return super.dispatchTouchEvent(e);
    }

    private boolean inStick(float x, float y) {
        return Math.hypot(x - cx, y - cy) < radius * 1.4f;
    }

    private boolean inJump(float x, float y) {
        return Math.hypot(x - jumpX, y - jumpY) < jumpR * 1.3f;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked(), index = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(index), y = e.getY(index);
                if (stickPointer < 0 && inStick(x, y)) {
                    stickPointer = e.getPointerId(index);
                    moveKnob(x, y);
                } else if (jumpPointer < 0 && inJump(x, y)) {
                    jumpPointer = e.getPointerId(index);
                    SDLActivity.onNativeKeyDown(KeyEvent.KEYCODE_SPACE);
                    performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                    invalidate();
                }
                return true;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); ++i) {
                    if (e.getPointerId(i) == stickPointer) moveKnob(e.getX(i), e.getY(i));
                }
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                int id = action == MotionEvent.ACTION_CANCEL ? -2 : e.getPointerId(index);
                if (id == stickPointer || id == -2) {
                    stickPointer = -1;
                    knobX = cx;
                    knobY = cy;
                    setKeys(false, false, false, false);
                }
                if (id == jumpPointer || id == -2) {
                    if (jumpPointer >= 0) SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_SPACE);
                    jumpPointer = -1;
                }
                invalidate();
                return true;
            }
            default:
                return true;
        }
    }

    private void moveKnob(float x, float y) {
        float dx = x - cx, dy = y - cy, d = (float) Math.hypot(dx, dy);
        if (d > radius) {
            dx *= radius / d;
            dy *= radius / d;
        }
        knobX = cx + dx;
        knobY = cy + dy;
        float dead = radius * 0.3f;
        if (d < dead) {
            setKeys(false, false, false, false);
        } else {
            // eight directions: a key is held when the stick leans more than 22.5 degrees toward it
            double a = Math.atan2(-dy, dx);  // 0 = right, pi/2 = up
            double s = Math.sin(a), c = Math.cos(a), t = Math.sin(Math.toRadians(22.5));
            setKeys(s > t, c < -t, s < -t, c > t);
        }
        invalidate();
    }

    private void setKeys(boolean w, boolean a, boolean s, boolean d) {
        boolean[] want = {w, a, s, d};
        for (int i = 0; i < 4; ++i) {
            if (want[i] == held[i]) continue;
            held[i] = want[i];
            if (want[i]) SDLActivity.onNativeKeyDown(KEYS[i]);
            else SDLActivity.onNativeKeyUp(KEYS[i]);
        }
    }

    /** Hides the stick, releasing whatever it holds. */
    void hide() {
        setKeys(false, false, false, false);
        if (jumpPointer >= 0) SDLActivity.onNativeKeyUp(KeyEvent.KEYCODE_SPACE);
        stickPointer = jumpPointer = -1;
        setVisibility(GONE);
    }

    @Override
    protected void onDraw(Canvas c) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0x400a1a2a);
        c.drawCircle(cx, cy, radius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(2 * density);
        paint.setColor(0x9040d6c8);
        c.drawCircle(cx, cy, radius, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(stickPointer >= 0 ? 0xc040d6c8 : 0x8040d6c8);
        c.drawCircle(knobX, knobY, radius * 0.42f, paint);
        paint.setColor(jumpPointer >= 0 ? 0xc040d6c8 : 0x600a1a2a);
        c.drawCircle(jumpX, jumpY, jumpR, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setColor(TEAL);
        c.drawCircle(jumpX, jumpY, jumpR, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(0xffffffff);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(13 * density);
        c.drawText("Jump", jumpX, jumpY + 5 * density, paint);
    }
}
