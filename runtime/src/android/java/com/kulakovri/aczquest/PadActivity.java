/* PadActivity.java -- the Quest build's activity: android.app.NativeActivity plus gamepads, joysticks and their rumble
   (ps2_pad_android.c).

   The device run of 2026-11-09 (ACZ_QUEST_PAD_LOG=1, an Xbox Wireless Controller over Bluetooth, immersive flight) saw
   the pad in Android's device list but not one of its events in the NativeActivity's input queue; this activity's
   dispatchKeyEvent / dispatchGenericMotionEvent got them all (step 1, the same night). So every gamepad, joystick (HOTAS)
   and Sony sub-device (the DualSense / DualShock 4 touchpad) event is handed to libmain.so here, through JNI.

   libmain.so registers the native methods itself (RegisterNatives from android_main: hidden visibility does not matter,
   no System.loadLibrary) and then sets sMode; until then, and with ACZ_QUEST_PAD=0, ACZ_QUEST_JOY=0 and
   ACZ_QUEST_PAD_LOG=0, every event goes to NativeActivity unchanged.
     sMode bit 0: forward the events of gamepads, joysticks and Sony sub-devices (consumed when the native side says so)
     sMode bit 1: also forward every other key and motion event (only logged there, never consumed)
   startDevices() (called by libmain.so) reports every such device to the native side and listens for hot-plug.
   rumble() is called by libmain.so's rumble thread (SDL_RumbleGamepad): the pad's vibrators through
   InputDevice.getVibratorManager() (API 31; Android lists a pad's motors only when its kernel driver offers force
   feedback), else InputDevice.getVibrator().
   startDisplays() (called by libmain.so, ACZ_QUEST_CAPTURE_WATCH) reports every display Android lists at the start and
   every display added, removed or changed later (DisplayManager.DisplayListener), with its state, size and refresh rate:
   Meta's recorder does not add a display, it switches one of the "MirrorRoot for <uuid>" displays that are there from the
   start (device run 2026-11-10 00:48), and the GPU hang report names the display that changed or was added last
   (BUGS_RQ R-37, ps2_display_android.c). */
package com.kulakovri.aczquest;

import android.app.NativeActivity;
import android.content.Context;
import android.graphics.Point;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.CombinedVibration;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.util.SparseIntArray;
import android.view.Display;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.util.List;

public class PadActivity extends NativeActivity {
    static volatile int sMode;
    static PadActivity sSelf;
    static final int NAX = 48;                  /* axis ids 0..47: every MotionEvent axis ps2_pad_android.c reads */

    static native boolean nKey(int dev, int source, int action, int code, int scan, int flags, int meta, int repeat, long timeNs);
    static native boolean nMotion(int dev, int source, int action, int buttons, float[] axes, long timeNs);
    static native void nDevice(int dev, String name, int vendor, int product, int sources, float[] ranges, long keys,
                               int vibrators, boolean added);
    static native void nRemoved(int dev);
    static native void nFocus(boolean focus);
    /* what: 0 there at the start, 1 added, 2 removed, 3 changed; state = Display.getState() (-1: no access), the mode's size and rate */
    static native void nDisplay(int what, int id, String name, int flags, int state, int w, int h, float hz);

    /* the keys whose presence nDevice reports (bit i): ps2_pad_android.c KEYBITS has the same order */
    static final int[] KEYS = {
        KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_C, KeyEvent.KEYCODE_BUTTON_X,
        KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_Z, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
        KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR,
        KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.KEYCODE_UNKNOWN,
        KeyEvent.KEYCODE_BUTTON_1, KeyEvent.KEYCODE_BUTTON_2, KeyEvent.KEYCODE_BUTTON_3, KeyEvent.KEYCODE_BUTTON_4,
        KeyEvent.KEYCODE_BUTTON_5, KeyEvent.KEYCODE_BUTTON_6, KeyEvent.KEYCODE_BUTTON_7, KeyEvent.KEYCODE_BUTTON_8,
        KeyEvent.KEYCODE_BUTTON_9, KeyEvent.KEYCODE_BUTTON_10, KeyEvent.KEYCODE_BUTTON_11, KeyEvent.KEYCODE_BUTTON_12,
        KeyEvent.KEYCODE_BUTTON_13, KeyEvent.KEYCODE_BUTTON_14, KeyEvent.KEYCODE_BUTTON_15, KeyEvent.KEYCODE_BUTTON_16 };

    private final float[] ax = new float[NAX];
    private static final SparseIntArray sFwd = new SparseIntArray();   /* device id -> 1 forward, 2 not (UI thread) */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        sSelf = this;
    }

    static boolean has(int source, int what) { return (source & what) == what; }
    /* a device whose events go to libmain.so: a joystick, a gamepad, or a Sony pad's other parts (the touchpad) */
    static boolean interesting(InputDevice d) {
        int s = d.getSources();
        return has(s, InputDevice.SOURCE_JOYSTICK) || (s & 0x400) != 0 || d.getVendorId() == 0x054c;
    }
    private static boolean fwd(InputEvent e) {
        int s = e.getSource();
        if (has(s, InputDevice.SOURCE_JOYSTICK) || (s & 0x400) != 0) return true;
        int id = e.getDeviceId(), v = sFwd.get(id, 0);
        if (v == 0) {
            InputDevice d = e.getDevice();
            v = d != null && !d.isVirtual() && interesting(d) ? 1 : 2;
            sFwd.put(id, v);
        }
        return v == 1;
    }

    /* ---- the devices: reported once at the start and on every hot-plug (UI thread) ---- */
    static void report(int id, boolean added) {
        InputDevice d = InputDevice.getDevice(id);
        if (d == null || d.isVirtual() || !interesting(d)) return;
        List<InputDevice.MotionRange> rs = d.getMotionRanges();
        float[] r = new float[rs.size() * 5];
        for (int i = 0; i < rs.size(); i++) {
            InputDevice.MotionRange m = rs.get(i);
            r[i * 5] = m.getAxis(); r[i * 5 + 1] = m.getMin(); r[i * 5 + 2] = m.getMax(); r[i * 5 + 3] = m.getFlat();
            r[i * 5 + 4] = m.getSource();
        }
        long keys = 0;
        boolean[] hk = d.hasKeys(KEYS);
        for (int i = 0; i < KEYS.length && i < hk.length; i++) if (hk[i] && KEYS[i] != KeyEvent.KEYCODE_UNKNOWN) keys |= 1L << i;
        int vib = 0;
        try {
            if (Build.VERSION.SDK_INT >= 31) vib = d.getVibratorManager().getVibratorIds().length;
            if (vib == 0 && d.getVibrator().hasVibrator()) vib = 100;
        } catch (Throwable t) { vib = -1; }
        sFwd.delete(id);
        nDevice(id, d.getName(), d.getVendorId(), d.getProductId(), d.getSources(), r, keys, vib, added);
    }
    static final InputManager.InputDeviceListener sListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { if (sMode != 0) report(id, true); }
        @Override public void onInputDeviceRemoved(int id) { sFwd.delete(id); if (sMode != 0) nRemoved(id); }
        @Override public void onInputDeviceChanged(int id) { if (sMode != 0) report(id, true); }
    };
    static void startDevices() {
        final Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() {
            @Override public void run() {
                try {
                    if (sSelf != null) {
                        InputManager im = (InputManager) sSelf.getSystemService(Context.INPUT_SERVICE);
                        if (im != null) im.registerInputDeviceListener(sListener, h);
                    }
                    for (int id : InputDevice.getDeviceIds()) report(id, false);
                } catch (Throwable t) {
                    Log.w("aczquest", "pad: device list: " + t);
                }
            }
        });
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int m = sMode;
        if (m != 0) {
            boolean pad = (m & 1) != 0 && (fwd(e) || KeyEvent.isGamepadButton(e.getKeyCode()));
            if (pad || (m & 2) != 0) {
                if (nKey(e.getDeviceId(), e.getSource(), e.getAction(), e.getKeyCode(), e.getScanCode(), e.getFlags(),
                         e.getMetaState(), e.getRepeatCount(), e.getEventTime() * 1000000L) && pad)
                    return true;
            }
        }
        return super.dispatchKeyEvent(e);
    }

    private boolean motion(MotionEvent e) {
        int m = sMode;
        if (m == 0) return false;
        boolean pad = (m & 1) != 0 && fwd(e);
        if (!pad && (m & 2) == 0) return false;
        for (int i = 0; i < NAX; i++) ax[i] = e.getAxisValue(i);
        return nMotion(e.getDeviceId(), e.getSource(), e.getActionMasked(), e.getButtonState(), ax, e.getEventTime() * 1000000L) && pad;
    }
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (motion(e)) return true;
        return super.dispatchGenericMotionEvent(e);
    }
    @Override
    public boolean dispatchTouchEvent(MotionEvent e) {     /* (a Sony touchpad may report as a touch device) */
        if ((sMode & 1) != 0 && !has(e.getSource(), InputDevice.SOURCE_TOUCHSCREEN) && fwd(e) && motion(e)) return true;
        return super.dispatchTouchEvent(e);
    }

    /* lo = the large (low-frequency) motor, hi = the small (high-frequency) one, 0..255 each; 0 and 0 stop.
       Returns the number of motors driven through VibratorManager (1, 2, ...), 100 for the single Vibrator,
       -1 no such device, -2 Android offers no vibrator for it, -3 an exception (logcat tag aczquest). */
    static int rumble(int dev, int lo, int hi, int ms) {
        try {
            InputDevice d = InputDevice.getDevice(dev);
            if (d == null) return -1;
            int both = Math.min(Math.max(lo, hi), 255), dur = Math.max(ms, 1);
            if (Build.VERSION.SDK_INT >= 31) {
                VibratorManager vm = d.getVibratorManager();
                int[] ids = vm.getVibratorIds();
                if (ids.length > 0) {
                    if (lo <= 0 && hi <= 0) { vm.cancel(); return ids.length; }
                    CombinedVibration.ParallelCombination c = CombinedVibration.startParallel();
                    if (ids.length >= 2) {      /* (Android's input vibrators: id 0 the strong motor, id 1 the weak one) */
                        if (lo > 0) c.addVibrator(ids[0], VibrationEffect.createOneShot(dur, Math.min(lo, 255)));
                        if (hi > 0) c.addVibrator(ids[1], VibrationEffect.createOneShot(dur, Math.min(hi, 255)));
                    } else c.addVibrator(ids[0], VibrationEffect.createOneShot(dur, both));
                    vm.vibrate(c.combine());
                    return ids.length;
                }
            }
            Vibrator v = d.getVibrator();
            if (v == null || !v.hasVibrator()) return -2;
            if (lo <= 0 && hi <= 0) v.cancel();
            else v.vibrate(VibrationEffect.createOneShot(dur, both));
            return 100;
        } catch (Throwable t) {
            Log.w("aczquest", "pad: rumble on device " + dev + ": " + t);
            return -3;
        }
    }

    /* ---- the displays (R-37: a capture's mirror is a virtual display); UI thread ---- */
    static void send(int what, int id, Display d) {
        String name = null;
        int flags = 0, state = -1, w = 0, h = 0;
        float hz = 0;
        if (d != null) {
            try {
                name = d.getName(); flags = d.getFlags(); state = d.getState();
                Display.Mode m = d.getMode();
                if (m != null) { w = m.getPhysicalWidth(); h = m.getPhysicalHeight(); hz = m.getRefreshRate(); }
                if (w == 0 || h == 0) { Point p = new Point(); d.getRealSize(p); w = p.x; h = p.y; }
            } catch (Throwable t) { if (name == null) state = -1; }
        }
        nDisplay(what, id, name, flags, state, w, h, hz);
    }
    static void display(int what, int id) {
        Display d = null;
        try {
            DisplayManager dm = sSelf != null ? (DisplayManager) sSelf.getSystemService(Context.DISPLAY_SERVICE) : null;
            d = dm != null ? dm.getDisplay(id) : null;                    /* (null: another app's private display) */
        } catch (Throwable t) { d = null; }
        send(what, id, d);
    }
    static final DisplayManager.DisplayListener sDisplays = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int id) { display(1, id); }
        @Override public void onDisplayRemoved(int id) { send(2, id, null); }
        @Override public void onDisplayChanged(int id) { display(3, id); }
    };
    static void startDisplays() {
        final Handler h = new Handler(Looper.getMainLooper());
        h.post(new Runnable() {
            @Override public void run() {
                try {
                    DisplayManager dm = sSelf != null ? (DisplayManager) sSelf.getSystemService(Context.DISPLAY_SERVICE) : null;
                    if (dm == null) { Log.w("aczquest", "capture: no DisplayManager"); return; }
                    dm.registerDisplayListener(sDisplays, h);
                    for (Display d : dm.getDisplays()) send(0, d.getDisplayId(), d);
                } catch (Throwable t) {
                    Log.w("aczquest", "capture: display list: " + t);
                }
            }
        });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (sMode != 0) nFocus(hasFocus);
    }
}
