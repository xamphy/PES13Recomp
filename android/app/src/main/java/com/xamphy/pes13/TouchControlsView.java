package com.xamphy.pes13;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.MotionEvent;
import android.view.View;

// On-screen controls, the Android port of runtime/video/touch_controls_ios.mm (same layout):
// left: a floating analog stick (touch anywhere in the lower-left area) and a D-pad above it;
// right: A/B/X/Y in the Xbox diamond; top corners: LT/LB and RB/RT; top centre: Back / Start;
// stick clicks LS/RS at the bottom. Fingers can slide between buttons. The state goes to the game
// as player 1 (merged with the keyboard) through PesActivity.nativeSetTouchPad.
public class TouchControlsView extends View {
    private static final int STICK = -2, NONE = -1;

    private static final class Button {
        final String label; final int bit; final int trigger; final int color;
        float x, y, r;
        Button(String label, int bit, int trigger, int color) { this.label = label; this.bit = bit; this.trigger = trigger; this.color = color; }
    }

    private final Button[] buttons;
    private final int[] pointerControl = new int[16]; // pointer id -> control (STICK, NONE or button index)
    private float stickOx, stickOy, stickRestX, stickRestY, stickKx, stickKy, stickR;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private boolean[] lastDown = new boolean[0];

    public TouchControlsView(Context context) {
        super(context);
        final int grey = Color.rgb(217, 217, 217);
        buttons = new Button[] {
            new Button("A", 0x1000, 0, Color.rgb(77, 204, 77)),
            new Button("B", 0x2000, 0, Color.rgb(230, 64, 64)),
            new Button("X", 0x4000, 0, Color.rgb(64, 128, 242)),
            new Button("Y", 0x8000, 0, Color.rgb(242, 204, 51)),
            new Button("LB", 0x0100, 0, grey), new Button("RB", 0x0200, 0, grey),
            new Button("LT", 0, 1, grey), new Button("RT", 0, 2, grey),
            new Button("BACK", 0x0020, 0, grey), new Button("START", 0x0010, 0, grey),
            new Button("▲", 0x0001, 0, grey), new Button("▼", 0x0002, 0, grey),
            new Button("◀", 0x0004, 0, grey), new Button("▶", 0x0008, 0, grey),
            new Button("LS", 0x0040, 0, grey), new Button("RS", 0x0080, 0, grey),
        };
        java.util.Arrays.fill(pointerControl, NONE);
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(Color.argb(217, 255, 255, 255));
        text.setFakeBoldText(true);
    }

    private void place(int i, float x, float y, float r) { buttons[i].x = x; buttons[i].y = y; buttons[i].r = r; }

    @Override
    protected void onSizeChanged(int W, int H, int oldW, int oldH) {
        final float u = Math.min(W, H) / 820f; // same design units as the iPad (1180 x 820 pt landscape)
        final float left = 20 * u, right = W - 20 * u, top = 20 * u, bottom = H - 20 * u;
        final float fx = right - 170 * u, fy = bottom - 190 * u, fo = 88 * u, fr = 46 * u;
        place(0, fx, fy + fo, fr);
        place(1, fx + fo, fy, fr);
        place(2, fx - fo, fy, fr);
        place(3, fx, fy - fo, fr);
        place(6, left + 70 * u, top + 55 * u, 48 * u);
        place(4, left + 190 * u, top + 55 * u, 48 * u);
        place(7, right - 70 * u, top + 55 * u, 48 * u);
        place(5, right - 190 * u, top + 55 * u, 48 * u);
        place(8, W / 2f - 80 * u, top + 35 * u, 34 * u);
        place(9, W / 2f + 80 * u, top + 35 * u, 34 * u);
        final float dx = left + 150 * u, dy = bottom - 430 * u, dpo = 58 * u, dpr = 34 * u;
        place(10, dx, dy - dpo, dpr);
        place(11, dx, dy + dpo, dpr);
        place(12, dx - dpo, dy, dpr);
        place(13, dx + dpo, dy, dpr);
        place(14, left + 330 * u, bottom - 40 * u, 28 * u);
        place(15, right - 330 * u, bottom - 40 * u, 28 * u);
        stickR = 100 * u;
        stickRestX = left + 150 * u;
        stickRestY = bottom - 150 * u;
        if (!stickActive()) { stickOx = stickKx = stickRestX; stickOy = stickKy = stickRestY; }
        stroke.setStrokeWidth(2 * Math.max(1f, u));
        invalidate();
    }

    private boolean stickActive() {
        for (int c : pointerControl) if (c == STICK) return true;
        return false;
    }

    private int hitTest(float x, float y) {
        int best = NONE;
        float bestD = Float.MAX_VALUE;
        for (int i = 0; i < buttons.length; i++) {
            final float d = (float) Math.hypot(x - buttons[i].x, y - buttons[i].y);
            if (d < buttons[i].r * 1.25f && d < bestD) { best = i; bestD = d; } // a little slack for thumbs
        }
        return best;
    }

    private boolean inStickZone(float x, float y) { return x < getWidth() * 0.42f && y > getHeight() * 0.45f; }

    private void moveStick(float x, float y) {
        float dx = x - stickOx, dy = y - stickOy;
        final float d = (float) Math.hypot(dx, dy);
        if (d > stickR) { dx *= stickR / d; dy *= stickR / d; }
        stickKx = stickOx + dx;
        stickKy = stickOy + dy;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        final int action = e.getActionMasked();
        final int idx = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                final int id = e.getPointerId(idx);
                if (id >= pointerControl.length) break;
                final float x = e.getX(idx), y = e.getY(idx);
                int c = hitTest(x, y);
                if (c == NONE && !stickActive() && inStickZone(x, y)) {
                    c = STICK;
                    stickOx = stickKx = x; // floating stick: centred where the thumb lands
                    stickOy = stickKy = y;
                }
                pointerControl[id] = c;
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    final int id = e.getPointerId(i);
                    if (id >= pointerControl.length) continue;
                    final int c = pointerControl[id];
                    if (c == STICK) moveStick(e.getX(i), e.getY(i));
                    else if (c != NONE) {
                        final int h = hitTest(e.getX(i), e.getY(i)); // slide between buttons
                        if (h != NONE) pointerControl[id] = h;
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (action == MotionEvent.ACTION_CANCEL) java.util.Arrays.fill(pointerControl, NONE);
                else {
                    final int id = e.getPointerId(idx);
                    if (id < pointerControl.length) pointerControl[id] = NONE;
                }
                if (!stickActive()) { stickOx = stickKx = stickRestX; stickOy = stickKy = stickRestY; }
                break;
            }
        }
        publish();
        invalidate();
        return true;
    }

    public void releaseAll() {
        java.util.Arrays.fill(pointerControl, NONE);
        stickOx = stickKx = stickRestX;
        stickOy = stickKy = stickRestY;
        publish();
        invalidate();
    }

    private void publish() {
        int bits = 0, lt = 0, rt = 0, lx = 0, ly = 0;
        final boolean[] down = new boolean[buttons.length];
        for (int c : pointerControl) if (c >= 0) down[c] = true;
        for (int i = 0; i < buttons.length; i++) {
            if (!down[i]) continue;
            bits |= buttons[i].bit;
            if (buttons[i].trigger == 1) lt = 255;
            if (buttons[i].trigger == 2) rt = 255;
        }
        if (stickActive()) {
            final float dx = Math.max(-1f, Math.min(1f, (stickKx - stickOx) / stickR));
            final float dy = Math.max(-1f, Math.min(1f, (stickKy - stickOy) / stickR));
            lx = Math.round(dx * 32767);
            ly = Math.round(-dy * 32767); // Xbox: +y is up
        }
        lastDown = down;
        PesActivity.nativeSetTouchPad(bits, lt, rt, lx, ly);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        for (int i = 0; i < buttons.length; i++) {
            final Button b = buttons[i];
            final boolean d = i < lastDown.length && lastDown[i];
            fill.setColor(b.color);
            fill.setAlpha(d ? 166 : 64);
            canvas.drawCircle(b.x, b.y, b.r, fill);
            stroke.setColor(b.color);
            stroke.setAlpha(179);
            canvas.drawCircle(b.x, b.y, b.r, stroke);
            final float fs = (b.label.length() > 2 ? 13 : 20) * Math.min(getWidth(), getHeight()) / 820f;
            text.setTextSize(fs);
            canvas.drawText(b.label, b.x, b.y + fs * 0.35f, text);
        }
        fill.setColor(Color.argb(26, 255, 255, 255));
        canvas.drawCircle(stickOx, stickOy, stickR, fill);
        stroke.setColor(Color.argb(128, 255, 255, 255));
        canvas.drawCircle(stickOx, stickOy, stickR, stroke);
        fill.setColor(Color.argb(89, 255, 255, 255));
        canvas.drawCircle(stickKx, stickKy, stickR * 0.45f, fill);
    }
}
