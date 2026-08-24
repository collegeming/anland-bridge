package com.anland.consumer;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;
import android.view.Surface;

import java.io.IOException;
import java.lang.ref.WeakReference;

/**
 * Process owner for the launcher/default daemon transport.
 * SecondaryActivity deliberately does not use this object and keeps its own Native.
 */
final class DisplaySession {
    enum Mode {
        LOCAL("local"), REMOTE("remote"), BOTH("both");

        final String value;
        Mode(String value) { this.value = value; }

        static Mode from(String value) {
            if (REMOTE.value.equals(value)) return REMOTE;
            if (BOTH.value.equals(value)) return BOTH;
            return LOCAL;
        }
    }

    private enum Route { STOPPED, LOCAL_DIRECT, REMOTE_DIRECT, FANOUT }

    private static final String TAG = "AnlandSession";
    private static DisplaySession instance;

    static synchronized DisplaySession get(Context context) {
        if (instance == null) instance = new DisplaySession(context.getApplicationContext());
        return instance;
    }

    private final Context context;
    private final Native nativeTransport = new Native();
    private final Clipboard clipboard;
    private Mode mode = Mode.LOCAL;
    private boolean bridgeAttached;
    private Surface localSurface;
    private WeakReference<MainActivity> localActivity = new WeakReference<>(null);
    private int localWidth;
    private int localHeight;
    private MediaCodecEncoder encoder;
    private MediaCodecEncoder.FrameSink frameSink;
    private Runnable streamFailureHandler;
    private Route route = Route.STOPPED;
    private int outputWidth;
    private int outputHeight;
    private int streamWidth;
    private int streamHeight;
    private int encodedWidth;
    private int encodedHeight;
    private int streamFps;
    private boolean fanoutDisabledForStream;
    private long nextFanoutGeneration;
    private long activeFanoutGeneration;

    private DisplaySession(Context context) {
        this.context = context;
        clipboard = new Clipboard(context, nativeTransport);
        mode = BridgeService.getMode(context);
        configureNative();
    }

    Native nativeTransport() { return nativeTransport; }
    Clipboard clipboard() { return clipboard; }

    synchronized int outputWidth() { return outputWidth; }
    synchronized int outputHeight() { return outputHeight; }
    synchronized boolean usesLocalRefreshRate() {
        return route == Route.LOCAL_DIRECT || route == Route.FANOUT;
    }

    synchronized boolean attachLocalSurface(Surface surface, MainActivity activity,
                                            int width, int height) {
        boolean changed = localSurface != surface || localActivity.get() != activity
                || localWidth != width || localHeight != height;
        localSurface = surface;
        localActivity = new WeakReference<>(activity);
        localWidth = width;
        localHeight = height;
        mode = BridgeService.getMode(context);
        return applyRoute(changed && (mode == Mode.BOTH || streamWidth == 0));
    }

    synchronized void detachLocalSurface(Surface surface, MainActivity activity) {
        if (localSurface != surface || localActivity.get() != activity) return;
        localSurface = null;
        localWidth = localHeight = 0;
        localActivity.clear();
        applyRoute();
    }

    synchronized void suspendLocalSurface(MainActivity activity) {
        if (localActivity.get() != activity) return;
        localSurface = null;
        localWidth = localHeight = 0;
        localActivity.clear();
        applyRoute();
    }

    synchronized void setMode(Mode newMode) {
        if (newMode == null) newMode = Mode.LOCAL;
        boolean changed = mode != newMode;
        mode = newMode;
        if (mode == Mode.LOCAL) stopStreamLocked();
        applyRoute(changed);
    }

    synchronized void setBridgeAttached(boolean attached) {
        bridgeAttached = attached;
        mode = BridgeService.getMode(context);
        if (!attached) stopStreamLocked();
        applyRoute();
    }

    synchronized void startStream(int width, int height, int fps,
                                  MediaCodecEncoder.FrameSink sink,
                                  Runnable failureHandler) throws IOException {
        if (mode == Mode.LOCAL || !bridgeAttached) {
            throw new IOException("Remote stream requested while bridge mode is local");
        }
        if (width < 64 || width > 8192 || height < 64 || height > 8192
                || fps < 1 || fps > 60) {
            throw new IOException("Invalid stream geometry or frame rate");
        }
        stopStreamLocked();
        streamWidth = width;
        streamHeight = height;
        encodedWidth = align16(width);
        encodedHeight = align16(height);
        streamFps = fps;
        frameSink = sink;
        streamFailureHandler = failureHandler;
        fanoutDisabledForStream = false;
        activeFanoutGeneration = 0;
        if (!applyRoute(true, true)) {
            throw new IOException("Unable to start the hardware stream route");
        }
    }

    synchronized void stopStream() {
        boolean wasStreaming = streamWidth > 0 || frameSink != null;
        stopStreamLocked();
        applyRoute(wasStreaming);
    }

    synchronized void requestIdr() {
        if (encoder != null) encoder.requestIdr();
    }

    synchronized void releaseRemoteInput() {
        nativeTransport.releasePressedInput();
    }

    synchronized void closeProcessSession() {
        nativeTransport.releasePressedInput();
        activeFanoutGeneration = 0;
        MediaCodecEncoder closing = beginEncoderClose();
        nativeTransport.stop();
        finishEncoderClose(closing);
        clipboard.setBridgeSink(null);
        nativeTransport.destroy();
        route = Route.STOPPED;
    }

    private void stopStreamLocked() {
        streamWidth = streamHeight = encodedWidth = encodedHeight = streamFps = 0;
        frameSink = null;
        streamFailureHandler = null;
        fanoutDisabledForStream = false;
        activeFanoutGeneration = 0;
    }

    private boolean applyRoute() {
        return applyRoute(false);
    }

    private boolean applyRoute(boolean force) {
        return applyRoute(force, false);
    }

    private boolean applyRoute(boolean force, boolean rebuildEncoder) {
        Route wanted = wantedRoute();
        if (!force && wanted == route
                && (wanted == Route.STOPPED || nativeTransport.isStarted())) return true;

        nativeTransport.releasePressedInput();
        activeFanoutGeneration = 0;
        boolean closeCurrentEncoder = rebuildEncoder || route == Route.FANOUT
                || wanted == Route.LOCAL_DIRECT || wanted == Route.STOPPED;
        MediaCodecEncoder closing = closeCurrentEncoder ? beginEncoderClose() : null;
        nativeTransport.stop();
        finishEncoderClose(closing);
        route = Route.STOPPED;
        configureNative();

        try {
            switch (wanted) {
                case LOCAL_DIRECT:
                    closeEncoder();
                    updateLocalGeometry();
                    if (!nativeTransport.start(localSurface, clipboard, localActivity.get())) {
                        throw new IOException("Native local display route failed to start");
                    }
                    route = Route.LOCAL_DIRECT;
                    break;
                case REMOTE_DIRECT:
                    ensureEncoder();
                    setOutputGeometry(streamWidth, streamHeight);
                    if (!nativeTransport.startRemote(encoder.getInputSurface(), clipboard,
                            streamWidth, streamHeight, encodedWidth, encodedHeight, streamFps,
                            mode == Mode.BOTH)) {
                        throw new IOException("Native remote display route failed to start");
                    }
                    route = Route.REMOTE_DIRECT;
                    break;
                case FANOUT:
                    ensureEncoder();
                    setOutputGeometry(streamWidth, streamHeight);
                    long generation = nextFanoutGeneration();
                    MainActivity activity = localActivity.get();
                    if (nativeTransport.startFanout(localSurface, encoder.getInputSurface(),
                            clipboard, activity, streamWidth, streamHeight,
                            encodedWidth, encodedHeight, streamFps, generation)) {
                        activeFanoutGeneration = generation;
                        route = Route.FANOUT;
                    } else {
                        fanoutDisabledForStream = true;
                        activeFanoutGeneration = 0;
                        Log.e(TAG, "EGL Android-native-buffer fanout unavailable; "
                                + "falling back to remote-direct for this stream");
                        if (!nativeTransport.startRemote(encoder.getInputSurface(), clipboard,
                                streamWidth, streamHeight, encodedWidth, encodedHeight,
                                streamFps, true)) {
                            throw new IOException("Native remote fallback route failed to start");
                        }
                        route = Route.REMOTE_DIRECT;
                    }
                    break;
                case STOPPED:
                default:
                    closeEncoder();
                    route = Route.STOPPED;
                    break;
            }
            return true;
        } catch (IOException | RuntimeException e) {
            Log.e(TAG, "Unable to apply display route " + wanted, e);
            MediaCodecEncoder failed = beginEncoderClose();
            nativeTransport.stop();
            finishEncoderClose(failed);
            route = Route.STOPPED;
            return false;
        }
    }

    private Route wantedRoute() {
        boolean streaming = streamWidth > 0 && frameSink != null;
        if (streaming) {
            if (mode == Mode.BOTH && localSurface != null && !fanoutDisabledForStream) {
                return Route.FANOUT;
            }
            return Route.REMOTE_DIRECT;
        }
        if (localSurface != null) return Route.LOCAL_DIRECT;
        return Route.STOPPED;
    }

    synchronized void onFanoutFailed(MainActivity activity, long generation) {
        if (route != Route.FANOUT || generation == 0
                || generation != activeFanoutGeneration
                || localActivity.get() != activity || streamWidth == 0) return;
        Log.e(TAG, "Runtime GPU fanout failure; rebuilding as remote-direct");
        fanoutDisabledForStream = true;
        activeFanoutGeneration = 0;
        if (!applyRoute(true, true)) notifyStreamFailure();
    }

    private long nextFanoutGeneration() {
        nextFanoutGeneration++;
        if (nextFanoutGeneration <= 0) nextFanoutGeneration = 1;
        return nextFanoutGeneration;
    }

    private void notifyStreamFailure() {
        Runnable handler = streamFailureHandler;
        if (handler != null) handler.run();
    }

    private void ensureEncoder() throws IOException {
        if (encoder != null) return;
        long requestedBitRate = (long) streamWidth * streamHeight * streamFps / 8L;
        int bitRate = (int) Math.max(4_000_000L, Math.min(20_000_000L, requestedBitRate));
        MediaCodecEncoder.FrameSink sink = frameSink;
        Runnable failure = streamFailureHandler;
        encoder = new MediaCodecEncoder(encodedWidth, encodedHeight, streamFps, bitRate,
                sink, failure);
    }

    private MediaCodecEncoder beginEncoderClose() {
        MediaCodecEncoder active = encoder;
        encoder = null;
        if (active != null) active.beginClose();
        return active;
    }

    private static void finishEncoderClose(MediaCodecEncoder closing) {
        if (closing != null) closing.finishClose();
    }

    private void closeEncoder() {
        finishEncoderClose(beginEncoderClose());
    }

    private void configureNative() {
        SharedPreferences prefs = context.getSharedPreferences(
                BridgeService.PREFS_NAME, Context.MODE_PRIVATE);
        String socketPath = prefs.getString("socket_path",
                "/data/local/tmp/display_daemon.sock");
        boolean useRoot = prefs.getBoolean("use_root", true);
        String helperPath = context.getApplicationInfo().nativeLibraryDir + "/libfdhelper.so";
        String bridgePath = context.getCacheDir().getAbsolutePath() + "/anland_fdbridge.sock";
        nativeTransport.configure(socketPath, useRoot, helperPath, bridgePath);
        int customWidth = prefs.getInt("custom_width", 0);
        int customHeight = prefs.getInt("custom_height", 0);
        nativeTransport.setCustomResolution(customWidth, customHeight);
        if (outputWidth <= 0 || outputHeight <= 0) {
            if (customWidth > 0 && customHeight > 0) setOutputGeometry(customWidth, customHeight);
            else setOutputGeometry(1920, 1080);
        }
    }

    private void updateLocalGeometry() {
        SharedPreferences prefs = context.getSharedPreferences(
                BridgeService.PREFS_NAME, Context.MODE_PRIVATE);
        int customWidth = prefs.getInt("custom_width", 0);
        int customHeight = prefs.getInt("custom_height", 0);
        if (customWidth > 0 && customHeight > 0) setOutputGeometry(customWidth, customHeight);
        else if (localWidth > 0 && localHeight > 0) {
            setOutputGeometry(localWidth, localHeight);
        }
    }

    private void setOutputGeometry(int width, int height) {
        if (width > 0 && height > 0) {
            outputWidth = width;
            outputHeight = height;
        }
    }

    private static int align16(int value) {
        return (value + 15) & ~15;
    }
}
