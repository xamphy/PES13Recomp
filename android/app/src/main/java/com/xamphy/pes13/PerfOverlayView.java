package com.xamphy.pes13;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.view.View;

// Performance overlay, the Android port of runtime/video/perf_overlay_ios.mm. Two small lines in the
// letterbox bar below the 16:9 picture, refreshed once per second:
//   58 FPS  ·  CP 9.1 / 14.0 ms  ·  3 slow  ·  1234 draws
//   RAM 1.21 GB  ·  thermal none  ·  battery 80% (charging)  ·  1280x720
// Without a bar big enough (a 16:9 screen) it shrinks to a pill in the top-left corner.
// Never takes touches. Thermal changes are written to the session log ([perf] lines).
public class PerfOverlayView extends View {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Paint p1 = new Paint(Paint.ANTI_ALIAS_FLAG), p2 = new Paint(Paint.ANTI_ALIAS_FLAG), bg = new Paint(Paint.ANTI_ALIAS_FLAG);
    private String fpsText = "", line1 = "", line2 = "";
    private int fpsColor = Color.WHITE;
    private float frameW = 16, frameH = 9;
    private int lastThermal = -1;

    private final Runnable tick = new Runnable() {
        @Override public void run() { refresh(); handler.postDelayed(this, 1000); }
    };

    public PerfOverlayView(Context context) {
        super(context);
        setClickable(false);
        setFocusable(false);
        p1.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
        p2.setTypeface(Typeface.MONOSPACE);
        p1.setColor(Color.argb(191, 255, 255, 255));
        p2.setColor(Color.argb(191, 255, 255, 255));
        bg.setColor(Color.argb(115, 0, 0, 0));
    }

    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); handler.post(tick); }
    @Override protected void onDetachedFromWindow() { handler.removeCallbacks(tick); super.onDetachedFromWindow(); }

    private static String thermalName(int t) {
        switch (t) {
            case 0: return "none"; case 1: return "light"; case 2: return "moderate"; case 3: return "severe";
            case 4: return "critical"; case 5: return "emergency"; case 6: return "shutdown";
        }
        return "?";
    }

    private void refresh() {
        final float[] s = PesActivity.nativePerfStats(); // fps, cpAvg, cpMax, slow, draws, footprint MB, w, h
        final int fps = Math.round(s[0]);
        fpsColor = fps >= 55 ? Color.argb(230, 115, 242, 115) : fps >= 28 ? Color.argb(230, 255, 217, 77) : Color.argb(230, 255, 102, 102);
        fpsText = fps + " FPS";
        line1 = String.format(java.util.Locale.US, "  ·  CP %.1f / %.1f ms  ·  %d slow  ·  %d draws", s[1], s[2], Math.round(s[3]), Math.round(s[4]));
        if (s[6] > 0 && s[7] > 0) { frameW = s[6]; frameH = s[7]; }

        int thermal = -1;
        final PowerManager pm = (PowerManager) getContext().getSystemService(Context.POWER_SERVICE);
        if (pm != null && Build.VERSION.SDK_INT >= 29) thermal = pm.getCurrentThermalStatus();
        final boolean lowPower = pm != null && pm.isPowerSaveMode();
        if (thermal != lastThermal) {
            PesActivity.nativeLog(String.format(java.util.Locale.US, "[perf] thermal %s, power saving %s (%d fps, CP %.1f ms)",
                thermalName(thermal), lowPower ? "ON" : "off", fps, s[1]));
            lastThermal = thermal;
        }
        String battery = "";
        final Intent b = getContext().registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (b != null) {
            final int level = b.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = b.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            final int status = b.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
            final boolean charging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL;
            if (level >= 0 && scale > 0) battery = "  ·  battery " + (level * 100 / scale) + "%" + (charging ? " (charging)" : "");
        }
        line2 = String.format(java.util.Locale.US, "RAM %.2f GB  ·  thermal %s%s%s  ·  %dx%d", s[5] / 1024f,
            thermalName(thermal), lowPower ? "  ·  POWER SAVING" : "", battery, Math.round(s[6]), Math.round(s[7]));
        invalidate();
    }

    @Override
    protected void onDraw(Canvas c) {
        final float W = getWidth(), H = getHeight();
        final float bar = W * frameH > H * frameW ? 0 : (H - W * frameH / frameW) / 2; // same fit as the presenter
        final float u = Math.min(W, H) / 820f;
        if (bar >= 40 * u) {
            final float s1 = 15 * u * 1.3f, s2 = 12 * u * 1.3f;
            p1.setTextSize(s1);
            p2.setTextSize(s2);
            final float y = H - bar + (bar - (s1 + s2 + 4 * u)) / 2 + s1;
            final float w1 = p1.measureText(fpsText) + p1.measureText(line1);
            float x = (W - w1) / 2;
            p1.setColor(fpsColor);
            c.drawText(fpsText, x, y, p1);
            p1.setColor(Color.argb(191, 255, 255, 255));
            c.drawText(line1, x + p1.measureText(fpsText), y, p1);
            c.drawText(line2, (W - p2.measureText(line2)) / 2, y + s2 + 4 * u, p2);
        } else {
            final float s1 = 15 * u * 1.3f;
            p1.setTextSize(s1);
            final float w = p1.measureText(fpsText) + 16 * u;
            c.drawRoundRect(new RectF(8 * u, 8 * u, 8 * u + w, 8 * u + s1 * 1.5f), 6 * u, 6 * u, bg);
            p1.setColor(fpsColor);
            c.drawText(fpsText, 16 * u, 8 * u + s1 * 1.1f, p1);
        }
    }
}
