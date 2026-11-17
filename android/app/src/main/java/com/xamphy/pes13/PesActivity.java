package com.xamphy.pes13;

import android.app.AlertDialog;
import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputFilter;
import android.view.InputDevice;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;

import org.libsdl.app.SDLActivity;

// The game. The whole game lives in libmain.so (SDL3 linked in statically). On top of SDL's surface:
// the performance overlay, then the touch controls (hidden while a controller is connected).
// Started by LauncherActivity with the chosen settings as extras.
public class PesActivity extends SDLActivity implements InputManager.InputDeviceListener {
    public static final String EXTRA_TOUCH = "touch_controls", EXTRA_OVERLAY = "perf_overlay";

    static native void nativeSetTouchPad(int buttons, int lt, int rt, int lx, int ly);
    static native float[] nativePerfStats();
    static native void nativeLog(String line);
    static native void nativePromptDone(boolean ok, String text);

    private static PesActivity sInstance;
    private TouchControlsView touch;
    private InputManager inputManager;

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sInstance = this;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (mLayout == null || mBrokenLibraries) return;
        final boolean wantOverlay = getIntent().getBooleanExtra(EXTRA_OVERLAY, true);
        final boolean wantTouch = getIntent().getBooleanExtra(EXTRA_TOUCH, true);
        final ViewGroup.LayoutParams full = new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        if (wantOverlay) mLayout.addView(new PerfOverlayView(this), full); // under the touch controls
        if (wantTouch) {
            touch = new TouchControlsView(this);
            mLayout.addView(touch, new ViewGroup.LayoutParams(full));
            inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
            inputManager.registerInputDeviceListener(this, new Handler(Looper.getMainLooper()));
            updateTouchVisibility();
        }
    }

    // ---------------------------------------------------------------- lifecycle (logged: a destroyed
    // activity ends the game - SDL turns onDestroy into a quit - so the log must say why it happened)
    static boolean isRunning() { return sInstance != null && !sInstance.isFinishing(); }

    private void logLifecycle(String what) {
        if (!mBrokenLibraries) nativeLog("[android] " + what);
    }

    @Override protected void onPause() { logLifecycle("onPause"); super.onPause(); }
    @Override protected void onResume() { super.onResume(); logLifecycle("onResume"); }
    @Override protected void onStop() { logLifecycle("onStop"); super.onStop(); }

    @Override
    public void onTrimMemory(int level) {
        logLifecycle("onTrimMemory level " + level);
        super.onTrimMemory(level);
    }

    @Override
    public void onConfigurationChanged(android.content.res.Configuration c) {
        logLifecycle("configuration changed: " + c.screenWidthDp + "x" + c.screenHeightDp + " dp, orientation " + c.orientation);
        super.onConfigurationChanged(c);
    }

    @Override
    protected void onDestroy() {
        logLifecycle("onDestroy (finishing " + isFinishing() + ", changing configuration " + isChangingConfigurations() + ")");
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        if (sInstance == this) sInstance = null;
        super.onDestroy();
    }

    // ---------------------------------------------------------------- controllers
    private static boolean anyGamepad() {
        for (int id : InputDevice.getDeviceIds()) {
            final InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            final int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                return true;
        }
        return false;
    }

    private void updateTouchVisibility() {
        if (touch == null) return;
        final boolean pad = anyGamepad();
        final int v = pad ? android.view.View.GONE : android.view.View.VISIBLE;
        if (touch.getVisibility() == v) return;
        if (pad) touch.releaseAll();
        touch.setVisibility(v);
        nativeLog(pad ? "[input] touch controls hidden (controller connected)" : "[input] touch controls shown");
    }

    @Override public void onInputDeviceAdded(int deviceId) { updateTouchVisibility(); }
    @Override public void onInputDeviceRemoved(int deviceId) { updateTouchVisibility(); }
    @Override public void onInputDeviceChanged(int deviceId) { updateTouchVisibility(); }

    // ---------------------------------------------------------------- text prompt (player name)
    // Called from native code (android_bridge.cpp) on the SDL thread; the answer comes back through
    // nativePromptDone on the UI thread.
    public static boolean showTextPrompt(final String title, final String message, final String text, final int maxChars) {
        final PesActivity a = sInstance;
        if (a == null) return false;
        a.runOnUiThread(() -> {
            final EditText edit = new EditText(a);
            edit.setSingleLine(true);
            edit.setText(text);
            edit.setSelection(edit.getText().length());
            if (maxChars > 0) edit.setFilters(new InputFilter[] { new InputFilter.LengthFilter(maxChars) });
            final FrameLayout box = new FrameLayout(a);
            final int pad = Math.round(20 * a.getResources().getDisplayMetrics().density);
            box.setPadding(pad, pad / 2, pad, 0);
            box.addView(edit);
            new AlertDialog.Builder(a)
                .setTitle(title)
                .setMessage(message)
                .setView(box)
                .setCancelable(false)
                .setPositiveButton(android.R.string.ok, (d, w) -> nativePromptDone(true, edit.getText().toString()))
                .setNegativeButton(android.R.string.cancel, (d, w) -> nativePromptDone(false, text))
                .show();
            edit.requestFocus();
        });
        return true;
    }
}
