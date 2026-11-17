package com.xamphy.pes13;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Build;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;

// Launch menu (the Android counterpart of runtime/video/launcher_ios.mm and launcher_win32.cpp):
// Play Game / Settings / About over the generated stadium background. Settings are the shared
// userdata/launcher.ini (launcher_core.cpp), so they mean the same on every platform. Works with
// touch and with a controller (D-pad / stick moves the focus, A selects, B goes back).
public class LauncherActivity extends Activity {
    static native void nativeSetWorkDir(String dir);
    static native int[] nativeRenderBackground(int w, int h);
    static native int[] nativeLoadSettings();      // upscaler, upscaleHeight, perfOverlay, touch, blur
    static native void nativeSaveSettings(int[] v);
    static native String[] nativeAbout(String device);
    static native boolean nativeGameFilesPresent();

    private static final String[] UPSCALERS = { "Off", "FSR 1", "SGSR 1" };
    private static final String[] HEIGHTS = { "Screen", "1440p", "1080p", "900p" };
    private static final int ACCENT = Color.rgb(64, 156, 255);

    private int[] settings;
    private FrameLayout root;
    private View page;
    private boolean onMain = true;
    private File filesDir;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Opened from the home screen while a game is running (the game has its own task): go back
        // to it instead of showing the menu again.
        if (PesActivity.isRunning()) {
            startActivity(new Intent(this, PesActivity.class).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
            finish();
            return;
        }
        System.loadLibrary("main");
        filesDir = getExternalFilesDir(null);
        if (filesDir == null) filesDir = getFilesDir();
        new File(filesDir, "game/xex").mkdirs();   // show testers where the files go
        new File(filesDir, "game/files").mkdirs();
        nativeSetWorkDir(filesDir.getAbsolutePath());
        settings = nativeLoadSettings();

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(3, 5, 12));
        final ImageView bg = new ImageView(this);
        bg.setScaleType(ImageView.ScaleType.CENTER_CROP);
        root.addView(bg, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(root);
        hideSystemBars();
        // Background at half resolution (it is soft anyway), drawn by the shared native code.
        root.post(() -> {
            final int w = Math.max(320, root.getWidth() / 2), h = Math.max(180, root.getHeight() / 2);
            new Thread(() -> {
                final int[] px = nativeRenderBackground(w, h);
                final Bitmap bmp = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
                runOnUiThread(() -> bg.setImageBitmap(bmp));
            }).start();
        });
        showMain();
    }

    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            final WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    // ---------------------------------------------------------------- building blocks
    private float u() { return Math.min(root.getResources().getDisplayMetrics().widthPixels, root.getResources().getDisplayMetrics().heightPixels) / 820f; }

    private TextView label(String s, float sizeU, boolean bold, int color) {
        final TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(color);
        t.setTextSize(TypedValue.COMPLEX_UNIT_PX, sizeU * u());
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return t;
    }

    private static GradientDrawable rounded(int color, float radius, int strokeColor, int strokeW) {
        final GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(radius);
        if (strokeW > 0) g.setStroke(strokeW, strokeColor);
        return g;
    }

    // Card background that lights up when focused (controller) or pressed (touch).
    private StateListDrawable cardBackground(boolean primary) {
        final float r = 18 * u();
        final StateListDrawable s = new StateListDrawable();
        final GradientDrawable focused = rounded(primary ? ACCENT : Color.argb(110, 64, 156, 255), r, Color.WHITE, Math.round(3 * u()));
        s.addState(new int[] { android.R.attr.state_focused }, focused);
        s.addState(new int[] { android.R.attr.state_pressed }, focused);
        s.addState(new int[] {}, rounded(primary ? Color.argb(230, 40, 120, 230) : Color.argb(120, 20, 26, 40), r, Color.argb(60, 255, 255, 255), Math.round(1.5f * u())));
        return s;
    }

    private View menuButton(String text, boolean primary, View.OnClickListener click) {
        final TextView b = label(text, 30, true, Color.WHITE);
        b.setGravity(Gravity.CENTER);
        b.setFocusable(true);
        b.setClickable(true);
        b.setBackground(cardBackground(primary));
        b.setOnClickListener(click);
        final int pad = Math.round(18 * u());
        b.setPadding(pad, pad, pad, pad);
        return b;
    }

    private void setPage(View v, boolean main) {
        if (page != null) root.removeView(page);
        page = v;
        onMain = main;
        root.addView(v, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    // ---------------------------------------------------------------- pages
    private void showMain() {
        root.post(() -> {
            final float u = u();
            final LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            col.setGravity(Gravity.CENTER_VERTICAL);
            final int side = Math.round(80 * u);
            col.setPadding(side, 0, side, 0);
            col.addView(label("PES 2013", 84, true, Color.WHITE));
            col.addView(label("Xbox 360 static recompilation", 22, false, Color.argb(190, 255, 255, 255)));
            final View bar = new View(this);
            bar.setBackgroundColor(ACCENT);
            final LinearLayout.LayoutParams barLp = new LinearLayout.LayoutParams(Math.round(120 * u), Math.round(5 * u));
            barLp.setMargins(0, Math.round(14 * u), 0, Math.round(30 * u));
            col.addView(bar, barLp);
            final boolean files = nativeGameFilesPresent();
            final View play = menuButton("Play Game", true, v -> play());
            final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(Math.round(380 * u), ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, 0, 0, Math.round(14 * u));
            col.addView(play, lp);
            col.addView(menuButton("Settings", false, v -> showSettings()), new LinearLayout.LayoutParams(lp));
            col.addView(menuButton("About", false, v -> showAbout()), new LinearLayout.LayoutParams(lp));
            if (!files) {
                final TextView warn = label("Game files not found. Copy them to:\n" + new File(filesDir, "game").getAbsolutePath()
                    + "\n(xex/default.xex and the files folder)", 18, false, Color.rgb(255, 200, 90));
                warn.setPadding(0, Math.round(10 * u), 0, 0);
                col.addView(warn);
            }
            setPage(col, true);
            play.requestFocus();
        });
    }

    // A settings row: title, description and the current value; selecting it cycles the value.
    private View row(String title, String detail, String value, View.OnClickListener click) {
        final float u = u();
        final LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setFocusable(true);
        r.setClickable(true);
        r.setBackground(cardBackground(false));
        final int pad = Math.round(18 * u);
        r.setPadding(pad, pad, pad, pad);
        final LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        text.addView(label(title, 22, true, Color.WHITE));
        text.addView(label(detail, 16, false, Color.argb(170, 255, 255, 255)));
        r.addView(text, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        final TextView v = label(value, 22, true, ACCENT);
        v.setGravity(Gravity.END);
        v.setPadding(pad, 0, 0, 0);
        r.addView(v);
        r.setOnClickListener(click);
        return r;
    }

    private View subPage(String title, View content) {
        final float u = u();
        final ScrollView scroll = new ScrollView(this);
        final LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        final int side = Math.round(80 * u);
        col.setPadding(side, Math.round(40 * u), side, Math.round(40 * u));
        col.addView(label(title, 52, true, Color.WHITE));
        final TextView hint = label("B / Back: return", 16, false, Color.argb(150, 255, 255, 255));
        hint.setPadding(0, 0, 0, Math.round(20 * u));
        col.addView(hint);
        col.addView(content);
        scroll.addView(col);
        return scroll;
    }

    private void showSettings() {
        final float u = u();
        final LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        final View[] rows = {
            row("Upscaling", "Sharper picture on the screen. FSR 1 is the sharpest, SGSR 1 is lighter on the GPU.",
                UPSCALERS[settings[0]], v -> { settings[0] = (settings[0] + 1) % UPSCALERS.length; saveAndRefresh(0); }),
            row("Upscaling resolution", "Size the upscaler renders at before the picture is fitted to the screen. Lower sizes cost less GPU time.",
                HEIGHTS[settings[1]], v -> { settings[1] = (settings[1] + 1) % HEIGHTS.length; saveAndRefresh(1); }),
            row("Touch controls", "On-screen buttons. Hidden while a controller is connected.",
                settings[3] != 0 ? "On" : "Off", v -> { settings[3] ^= 1; saveAndRefresh(2); }),
            row("Performance overlay", "FPS, memory, temperature and battery below the picture.",
                settings[2] != 0 ? "On" : "Off", v -> { settings[2] ^= 1; saveAndRefresh(3); }),
            row("Blur effects", "Glow around stadium lights, blurred background behind the pause menu, soft full-screen filter. Off is faster.",
                settings[4] != 0 ? "On" : "Off", v -> { settings[4] ^= 1; saveAndRefresh(4); }),
        };
        for (View r : rows) {
            final LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, 0, 0, Math.round(12 * u));
            list.addView(r, lp);
        }
        setPage(subPage("Settings", list), false);
        rows[focusRow].requestFocus();
    }

    private int focusRow = 0;
    private void saveAndRefresh(int row) {
        nativeSaveSettings(settings);
        focusRow = row;
        showSettings();
        focusRow = 0;
    }

    private void showAbout() {
        final float u = u();
        final LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setBackground(rounded(Color.argb(150, 10, 14, 24), 18 * u, Color.argb(60, 255, 255, 255), Math.round(1.5f * u)));
        final int pad = Math.round(24 * u);
        list.setPadding(pad, pad, pad, pad);
        list.setFocusable(true); // so a controller can land here and scroll
        for (String line : nativeAbout(Build.MANUFACTURER + " " + Build.MODEL + " (Android " + Build.VERSION.RELEASE + ")")) {
            if (line.startsWith("#")) {
                final TextView h = label(line.substring(1), 22, true, ACCENT);
                h.setPadding(0, Math.round(10 * u), 0, Math.round(4 * u));
                list.addView(h);
            } else if (line.contains("\t")) {
                final String[] kv = line.split("\t", 2);
                final LinearLayout r = new LinearLayout(this);
                final TextView k = label(kv[0], 17, true, Color.argb(200, 255, 255, 255));
                r.addView(k, new LinearLayout.LayoutParams(Math.round(260 * u), ViewGroup.LayoutParams.WRAP_CONTENT));
                r.addView(label(kv[1], 17, false, Color.WHITE), new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
                list.addView(r);
            } else {
                final TextView p = label(line, 15, false, Color.argb(170, 255, 255, 255));
                p.setPadding(0, Math.round(14 * u), 0, 0);
                list.addView(p);
            }
        }
        setPage(subPage("About", list), false);
        list.requestFocus();
    }

    // ---------------------------------------------------------------- play
    private void play() {
        if (!nativeGameFilesPresent()) {
            new AlertDialog.Builder(this)
                .setTitle("Game files not found")
                .setMessage("Copy your PES 2013 files (Xbox 360) to\n\n" + new File(filesDir, "game").getAbsolutePath()
                    + "\n\n  xex/default.xex\n  files/  (dt00_e.img ... and the rest)\n\nover USB (file transfer mode), then press Play Game again.")
                .setPositiveButton(android.R.string.ok, null)
                .show();
            return;
        }
        nativeSaveSettings(settings);
        final Intent i = new Intent(this, PesActivity.class);
        i.putExtra(PesActivity.EXTRA_TOUCH, settings[3] != 0);
        i.putExtra(PesActivity.EXTRA_OVERLAY, settings[2] != 0);
        startActivity(i);
        finish();
    }

    // ---------------------------------------------------------------- controller
    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        final int k = e.getKeyCode();
        if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) {
            if (k == KeyEvent.KEYCODE_BUTTON_A || k == KeyEvent.KEYCODE_BUTTON_START) {
                final View f = getCurrentFocus();
                if (f != null && f.isClickable()) { f.performClick(); return true; }
            }
            if (k == KeyEvent.KEYCODE_BUTTON_B || k == KeyEvent.KEYCODE_BACK) {
                if (!onMain) { showMain(); return true; }
                if (k == KeyEvent.KEYCODE_BUTTON_B) return true; // B on the main page does nothing
            }
        }
        if ((k == KeyEvent.KEYCODE_BUTTON_A || k == KeyEvent.KEYCODE_BUTTON_B || k == KeyEvent.KEYCODE_BUTTON_START)
            && e.getAction() == KeyEvent.ACTION_UP) return true;
        return super.dispatchKeyEvent(e);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }
}
