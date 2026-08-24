package com.anland.consumer;

import android.view.Surface;

import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.atomic.AtomicLong;

/** JNI transport for one anland consumer. */
public final class Native {
    static {
        System.loadLibrary("anland_consumer");
    }

    private static final AtomicLong NEXT_HANDOFF_ID = new AtomicLong();

    private long handle;
    private boolean started;
    private final long handoffId = NEXT_HANDOFF_ID.incrementAndGet();
    private final Set<Integer> pressedKeys = new HashSet<>();
    private final Set<Integer> pressedButtons = new HashSet<>();

    public Native() {
        handle = nativeCreate();
        if (handle == 0) throw new IllegalStateException("Unable to allocate native transport");
    }

    public synchronized void destroy() {
        if (handle == 0) return;
        releasePressedInput();
        nativeDestroy(handle);
        handle = 0;
        started = false;
    }

    public synchronized boolean isStarted() { return started && handle != 0; }

    public synchronized void configure(String socketPath, boolean useRoot,
                                       String helperPath, String bridgePath) {
        if (handle != 0) {
            nativeConfigure(handle, socketPath, useRoot, helperPath,
                    bridgePath + "." + Long.toUnsignedString(handoffId));
        }
    }

    public synchronized boolean start(Surface surface, Object clipboardTarget,
                                      Object activityTarget) {
        requireHandle();
        boolean ok = nativeStart(handle, surface, clipboardTarget, activityTarget);
        started = ok;
        return ok;
    }

    public synchronized boolean startRemote(Surface surface, Object clipboardTarget,
                                            int displayWidth, int displayHeight,
                                            int encodedWidth, int encodedHeight, int fps,
                                            boolean preserveLocalAudio) {
        requireHandle();
        boolean ok = nativeStartRemote(handle, surface, clipboardTarget,
                displayWidth, displayHeight, encodedWidth, encodedHeight, fps,
                preserveLocalAudio);
        started = ok;
        return ok;
    }

    public synchronized boolean startFanout(Surface localSurface, Surface encoderSurface,
                                            Object clipboardTarget, Object activityTarget,
                                            int displayWidth, int displayHeight,
                                            int encodedWidth, int encodedHeight, int fps,
                                            long generation) {
        requireHandle();
        boolean ok = nativeStartFanout(handle, localSurface, encoderSurface,
                clipboardTarget, activityTarget, displayWidth, displayHeight,
                encodedWidth, encodedHeight, fps, generation);
        started = ok;
        return ok;
    }

    public synchronized void stop() {
        if (handle == 0) return;
        releasePressedInput();
        nativeStop(handle);
        started = false;
    }

    public synchronized void releasePressedInput() {
        if (handle != 0) {
            for (int keycode : pressedKeys) nativeSendKey(handle, 1, keycode);
            for (int button : pressedButtons) nativeSendMouseButton(handle, button, false);
        }
        pressedKeys.clear();
        pressedButtons.clear();
    }

    public synchronized void setFocused(boolean focused) {
        if (handle != 0) nativeSetFocused(handle, focused);
    }
    public synchronized void setCustomResolution(int width, int height) {
        if (handle != 0) nativeSetCustomResolution(handle, width, height);
    }
    public synchronized void sendTouch(int action, float x, float y, int pointerId) {
        if (handle != 0) nativeSendTouch(handle, action, x, y, pointerId);
    }
    public synchronized void sendTouchFrame() {
        if (handle != 0) nativeSendTouchFrame(handle);
    }
    public synchronized void sendKey(int action, int keycode) {
        if (action == 0) pressedKeys.add(keycode);
        else if (action == 1) pressedKeys.remove(keycode);
        if (handle != 0) nativeSendKey(handle, action, keycode);
    }
    public synchronized void sendMouseMotion(float x, float y, float dx, float dy) {
        if (handle != 0) nativeSendMouseMotion(handle, x, y, dx, dy);
    }
    public synchronized void sendMouseButton(int button, boolean pressed) {
        if (pressed) pressedButtons.add(button);
        else pressedButtons.remove(button);
        if (handle != 0) nativeSendMouseButton(handle, button, pressed);
    }
    public synchronized void sendMouseScroll(int axis, float value) {
        if (handle != 0) nativeSendMouseScroll(handle, axis, value);
    }
    public synchronized void setRefreshRate(float hz) {
        if (handle != 0) nativeSetRefreshRate(handle, hz);
    }
    public synchronized void sendClipboard(byte[] data) {
        if (handle != 0 && data != null && data.length <= BridgeProtocol.MAX_CLIPBOARD_SIZE) {
            nativeSendClipboard(handle, data);
        }
    }
    public synchronized void sendTextInput(byte[] data) {
        if (handle != 0) nativeSendTextInput(handle, data);
    }
    public synchronized void setMicEnabled(boolean enabled) {
        if (handle != 0) nativeSetMicEnabled(handle, enabled);
    }
    public synchronized void setAudioLatency(int speakerMs, int micMs) {
        if (handle != 0) nativeSetAudioLatency(handle, speakerMs, micMs);
    }
    public synchronized void setAudioKeepalive(boolean enabled) {
        if (handle != 0) nativeSetAudioKeepalive(handle, enabled);
    }

    static int openBridgeSocket(String helperPath, String socketPath,
                                String handoffPath, int cancelFd, int timeoutMs) {
        return nativeOpenBridgeSocket(helperPath, socketPath, handoffPath, cancelFd, timeoutMs);
    }

    static void setBridgeSocketTimeout(int fd, int timeoutMs) {
        nativeSetBridgeSocketTimeout(fd, timeoutMs);
    }

    static void setBridgeSocketReceiveTimeout(int fd, int timeoutMs) {
        nativeSetBridgeSocketReceiveTimeout(fd, timeoutMs);
    }

    static void setBridgeSocketSendTimeout(int fd, int timeoutMs) {
        nativeSetBridgeSocketSendTimeout(fd, timeoutMs);
    }

    static void shutdownBridgeSocket(int fd) {
        nativeShutdownBridgeSocket(fd);
    }

    private void requireHandle() {
        if (handle == 0) throw new IllegalStateException("Native transport was destroyed");
    }

    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native void nativeSetFocused(long handle, boolean focused);
    private static native void nativeConfigure(long handle, String socketPath, boolean useRoot,
                                               String helperPath, String bridgePath);
    private static native boolean nativeStart(long handle, Surface surface,
                                              Object clipboardTarget, Object activityTarget);
    private static native boolean nativeStartRemote(long handle, Surface surface,
                                                    Object clipboardTarget,
                                                    int displayWidth, int displayHeight,
                                                    int encodedWidth, int encodedHeight, int fps,
                                                    boolean preserveLocalAudio);
    private static native boolean nativeStartFanout(long handle, Surface localSurface,
                                                    Surface encoderSurface,
                                                    Object clipboardTarget,
                                                    Object activityTarget,
                                                    int displayWidth, int displayHeight,
                                                    int encodedWidth, int encodedHeight, int fps,
                                                    long generation);
    private static native void nativeStop(long handle);
    private static native void nativeSetCustomResolution(long handle, int width, int height);
    private static native void nativeSendTouch(long handle, int action, float x, float y, int pointerId);
    private static native void nativeSendTouchFrame(long handle);
    private static native void nativeSendKey(long handle, int action, int keycode);
    private static native void nativeSendMouseMotion(long handle, float x, float y, float dx, float dy);
    private static native void nativeSendMouseButton(long handle, int button, boolean pressed);
    private static native void nativeSendMouseScroll(long handle, int axis, float value);
    private static native void nativeSetRefreshRate(long handle, float hz);
    private static native void nativeSendClipboard(long handle, byte[] data);
    private static native void nativeSendTextInput(long handle, byte[] data);
    private static native void nativeSetMicEnabled(long handle, boolean enabled);
    private static native void nativeSetAudioLatency(long handle, int speakerMs, int micMs);
    private static native void nativeSetAudioKeepalive(long handle, boolean enabled);
    private static native int nativeOpenBridgeSocket(String helperPath, String socketPath,
                                                     String handoffPath, int cancelFd,
                                                     int timeoutMs);
    private static native void nativeSetBridgeSocketTimeout(int fd, int timeoutMs);
    private static native void nativeSetBridgeSocketReceiveTimeout(int fd, int timeoutMs);
    private static native void nativeSetBridgeSocketSendTimeout(int fd, int timeoutMs);
    private static native void nativeShutdownBridgeSocket(int fd);
}
