package com.anland.consumer;

import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.SecureRandom;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/** Non-exported foreground client for the private root-owned bridge Unix socket. */
public final class BridgeService extends Service {
    static final String PREFS_NAME = "anland_settings";
    static final String KEY_MODE = "bridge_mode";
    static final String KEY_MODE_MIGRATED = "bridge_mode_migrated_v1";
    static final String KEY_LEGACY_ENABLED = "bridge_service_enabled";
    static final String KEY_TOKEN = "bridge_service_token";
    static final String DEFAULT_SOCKET_PATH = "/data/local/tmp/anland-rdp/bridge.sock";

    private static final String TAG = "AnlandBridge";
    private static final String CHANNEL_ID = "anland_bridge";
    private static final int NOTIFICATION_ID = 33910;
    private static final int VIDEO_QUEUE_CAPACITY = 8;
    private static final int CONTROL_QUEUE_CAPACITY = 16;
    private static final long MIN_RECONNECT_MS = 500;
    private static final long MAX_RECONNECT_MS = 30_000;
    private static final AtomicLong NEXT_SERVICE_GENERATION = new AtomicLong();
    private static final AtomicLong ACTIVE_SERVICE_GENERATION = new AtomicLong();
    private static final Object SERVICE_LIFECYCLE_LOCK = new Object();

    private final Object connectionLock = new Object();
    private final AtomicLong clipboardSequence = new AtomicLong(1);
    private volatile boolean running;
    private volatile ConnectionAttempt connectionAttempt;
    private volatile ConnectionState connection;
    private Thread connectionThread;
    private DisplaySession displaySession;
    private long serviceGeneration;
    private long nextAttemptGeneration;
    private long nextConnectionGeneration;

    private static final class ConnectionAttempt {
        final long generation;
        final ParcelFileDescriptor cancelRead;
        final ParcelFileDescriptor cancelWrite;
        final String handoffPath;
        final AtomicBoolean cancelled = new AtomicBoolean();
        ParcelFileDescriptor descriptor;
        ParcelFileDescriptor writerDescriptor;

        ConnectionAttempt(long generation, ParcelFileDescriptor cancelRead,
                          ParcelFileDescriptor cancelWrite, String handoffPath) {
            this.generation = generation;
            this.cancelRead = cancelRead;
            this.cancelWrite = cancelWrite;
            this.handoffPath = handoffPath;
        }
    }

    private static final class VideoFrame {
        final long streamGeneration;
        final long predictionEpoch;
        final byte[] bytes;

        VideoFrame(long streamGeneration, long predictionEpoch, byte[] bytes) {
            this.streamGeneration = streamGeneration;
            this.predictionEpoch = predictionEpoch;
            this.bytes = bytes;
        }
    }

    private static final class ConnectionState {
        final long generation;
        final ParcelFileDescriptor descriptor;
        final ParcelFileDescriptor writerDescriptor;
        final ArrayBlockingQueue<VideoFrame> videoQueue =
                new ArrayBlockingQueue<>(VIDEO_QUEUE_CAPACITY);
        final ArrayBlockingQueue<byte[]> controlQueue =
                new ArrayBlockingQueue<>(CONTROL_QUEUE_CAPACITY);
        final AtomicReference<byte[]> pendingClipboard = new AtomicReference<>();
        final AtomicLong awaitingClipboardAck = new AtomicLong();
        final AtomicBoolean connected = new AtomicBoolean();
        final AtomicBoolean retired = new AtomicBoolean();
        final AtomicBoolean socketShutdown = new AtomicBoolean();
        final AtomicBoolean dropVideoUntilIdr = new AtomicBoolean(true);
        final AtomicLong streamGeneration = new AtomicLong();
        final AtomicLong predictionEpoch = new AtomicLong(1);
        long nextStreamGeneration;
        final Object streamLock = new Object();
        final Object videoWriteLock = new Object();
        Clipboard.BridgeSink clipboardSink;
        Thread writerThread;

        ConnectionState(long generation, ParcelFileDescriptor descriptor,
                        ParcelFileDescriptor writerDescriptor) {
            this.generation = generation;
            this.descriptor = descriptor;
            this.writerDescriptor = writerDescriptor;
        }
    }

    static DisplaySession.Mode getMode(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String value = prefs.getString(KEY_MODE, null);
        if (value == null) {
            // One-time exact legacy migration: false/absent -> local, true -> remote.
            // Once the marker exists, a missing/corrupt new value never consults the
            // legacy boolean again.
            boolean migrated = prefs.getBoolean(KEY_MODE_MIGRATED, false);
            value = !migrated && prefs.getBoolean(KEY_LEGACY_ENABLED, false)
                    ? DisplaySession.Mode.REMOTE.value : DisplaySession.Mode.LOCAL.value;
            prefs.edit().putString(KEY_MODE, value)
                    .putBoolean(KEY_MODE_MIGRATED, true)
                    .remove(KEY_LEGACY_ENABLED).commit();
        } else if (!prefs.getBoolean(KEY_MODE_MIGRATED, false)) {
            prefs.edit().putBoolean(KEY_MODE_MIGRATED, true)
                    .remove(KEY_LEGACY_ENABLED).commit();
        }
        return DisplaySession.Mode.from(value);
    }

    static void setMode(Context context, DisplaySession.Mode mode) {
        if (mode == null) mode = DisplaySession.Mode.LOCAL;
        context.getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                .putString(KEY_MODE, mode.value)
                .putBoolean(KEY_MODE_MIGRATED, true)
                .remove(KEY_LEGACY_ENABLED).commit();
        DisplaySession.get(context).setMode(mode);
        Intent intent = new Intent(context, BridgeService.class);
        if (mode == DisplaySession.Mode.LOCAL) context.stopService(intent);
        else context.startForegroundService(intent);
    }

    static String getOrCreateToken(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String token = prefs.getString(KEY_TOKEN, null);
        if (isValidToken(token)) return token;
        byte[] random = new byte[16];
        new SecureRandom().nextBytes(random);
        char[] hex = "0123456789abcdef".toCharArray();
        char[] value = new char[32];
        for (int i = 0; i < random.length; i++) {
            value[i * 2] = hex[(random[i] >>> 4) & 0x0f];
            value[i * 2 + 1] = hex[random[i] & 0x0f];
        }
        token = new String(value);
        prefs.edit().putString(KEY_TOKEN, token).commit();
        return token;
    }

    private static boolean isValidToken(String token) {
        if (token == null || token.length() != 32) return false;
        for (int i = 0; i < token.length(); i++) {
            char value = token.charAt(i);
            if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        startForeground(NOTIFICATION_ID, buildNotification(false));
        synchronized (SERVICE_LIFECYCLE_LOCK) {
            serviceGeneration = NEXT_SERVICE_GENERATION.incrementAndGet();
            ACTIVE_SERVICE_GENERATION.set(serviceGeneration);
            running = true;
            displaySession = DisplaySession.get(this);
            displaySession.releaseRemoteInput();
            displaySession.stopStream();
            displaySession.setBridgeAttached(true);
            displaySession.clipboard().setBridgeSink(null);
        }
        connectionThread = new Thread(this::connectionLoop, "anland-bridge-client");
        connectionThread.start();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        DisplaySession.Mode mode = getMode(this);
        synchronized (SERVICE_LIFECYCLE_LOCK) {
            if (isActiveServiceLocked()) displaySession.setMode(mode);
        }
        if (mode == DisplaySession.Mode.LOCAL) {
            stopSelf();
            return START_NOT_STICKY;
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        ConnectionAttempt attempt;
        ConnectionState active;
        synchronized (SERVICE_LIFECYCLE_LOCK) {
            running = false;
            attempt = connectionAttempt;
            connectionAttempt = null;
            active = takeConnection();
            if (ACTIVE_SERVICE_GENERATION.compareAndSet(serviceGeneration, 0)) {
                displaySession.clipboard().setBridgeSink(null);
                displaySession.releaseRemoteInput();
                displaySession.stopStream();
                displaySession.setBridgeAttached(false);
            }
        }
        cancelAttempt(attempt);
        retireConnection(active);
        if (connectionThread != null && Thread.currentThread() != connectionThread) {
            connectionThread.interrupt();
            join(connectionThread);
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    private void connectionLoop() {
        long delay = MIN_RECONNECT_MS;
        while (isActiveService()) {
            try {
                connectAndRun();
                delay = MIN_RECONNECT_MS;
            } catch (IOException e) {
                if (isActiveService()) Log.w(TAG, "Private bridge disconnected: " + e.getMessage());
            } finally {
                ConnectionState retired;
                boolean resetSession;
                synchronized (SERVICE_LIFECYCLE_LOCK) {
                    retired = takeConnection();
                    resetSession = isActiveServiceLocked();
                }
                retireConnection(retired);
                if (resetSession && isActiveService()) {
                    displaySession.releaseRemoteInput();
                    displaySession.stopStream();
                    updateNotification(false);
                }
            }
            if (!isActiveService() || !sleepBeforeReconnect(delay)) break;
            delay = Math.min(MAX_RECONNECT_MS, delay * 2);
        }
    }

    private boolean sleepBeforeReconnect(long delayMs) {
        try {
            Thread.sleep(delayMs);
            return isActiveService();
        } catch (InterruptedException e) {
            if (isActiveService()) Thread.currentThread().interrupt();
            return false;
        }
    }

    private void connectAndRun() throws IOException {
        ConnectionAttempt attempt = beginConnectionAttempt();
        ConnectionState state = null;
        try {
            String helperPath = getApplicationInfo().nativeLibraryDir + "/libfdhelper.so";
            int fd = Native.openBridgeSocket(helperPath, DEFAULT_SOCKET_PATH,
                    attempt.handoffPath, attempt.cancelRead.getFd(),
                    BridgeProtocol.AUTH_TIMEOUT_MS);
            if (fd < 0) throw new IOException("Unable to connect to " + DEFAULT_SOCKET_PATH);

            ParcelFileDescriptor descriptor = ParcelFileDescriptor.adoptFd(fd);
            ParcelFileDescriptor writerDescriptor = null;
            try {
                writerDescriptor = ParcelFileDescriptor.dup(descriptor.getFileDescriptor());
                synchronized (attempt) {
                    attempt.descriptor = descriptor;
                    attempt.writerDescriptor = writerDescriptor;
                    if (attempt.cancelled.get()) shutdownAttemptSocketsLocked(attempt);
                }
                if (attempt.cancelled.get()) {
                    throw new IOException("Bridge service stopped during socket connection");
                }

                Native.setBridgeSocketTimeout(fd, BridgeProtocol.AUTH_TIMEOUT_MS);
                DataInputStream input = new DataInputStream(new BufferedInputStream(
                        new FileInputStream(descriptor.getFileDescriptor())));
                BufferedOutputStream output = new BufferedOutputStream(
                        new FileOutputStream(writerDescriptor.getFileDescriptor()));
                authenticate(input, output, fd);

                synchronized (SERVICE_LIFECYCLE_LOCK) {
                    if (!isActiveServiceLocked() || connectionAttempt != attempt
                            || attempt.cancelled.get()) {
                        throw new IOException("Bridge service stopped during authentication");
                    }
                    nextConnectionGeneration++;
                    if (nextConnectionGeneration <= 0) nextConnectionGeneration = 1;
                    state = new ConnectionState(nextConnectionGeneration, descriptor,
                            writerDescriptor);
                    ConnectionState active = state;
                    active.clipboardSink = utf8 -> queueClipboard(active, utf8);
                    active.connected.set(true);
                    active.writerThread = new Thread(
                            () -> writerLoop(active, output), "anland-bridge-writer");
                    synchronized (connectionLock) {
                        connection = active;
                    }
                    synchronized (attempt) {
                        attempt.descriptor = null;
                        attempt.writerDescriptor = null;
                    }
                    connectionAttempt = null;
                    Native.setBridgeSocketReceiveTimeout(fd, 0);
                    Native.setBridgeSocketSendTimeout(fd, BridgeProtocol.SEND_TIMEOUT_MS);
                    active.writerThread.start();
                    displaySession.clipboard().setBridgeSink(active.clipboardSink);
                    displaySession.clipboard().syncClipboard();
                    updateNotification(true);
                }

                while (isActiveConnection(state)) {
                    BridgeProtocol.Message message = BridgeProtocol.readActive(input, fd);
                    if (message == null) break;
                    synchronized (SERVICE_LIFECYCLE_LOCK) {
                        if (!isActiveConnection(state)) break;
                        dispatch(state, message);
                    }
                }
            } finally {
                if (state == null) {
                    synchronized (attempt) {
                        attempt.descriptor = null;
                        attempt.writerDescriptor = null;
                    }
                    closeQuietly(writerDescriptor);
                    closeQuietly(descriptor);
                }
            }
        } finally {
            synchronized (SERVICE_LIFECYCLE_LOCK) {
                if (connectionAttempt == attempt) connectionAttempt = null;
            }
            closeQuietly(attempt.cancelWrite);
            closeQuietly(attempt.cancelRead);
        }
    }

    private ConnectionAttempt beginConnectionAttempt() throws IOException {
        ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
        synchronized (SERVICE_LIFECYCLE_LOCK) {
            if (!isActiveServiceLocked()) {
                closeQuietly(pipe[1]);
                closeQuietly(pipe[0]);
                throw new IOException("Bridge service is stopped");
            }
            nextAttemptGeneration++;
            if (nextAttemptGeneration <= 0) nextAttemptGeneration = 1;
            String handoffPath = getCacheDir().getAbsolutePath() + "/rdp."
                    + Long.toUnsignedString(serviceGeneration, 36) + "."
                    + Long.toUnsignedString(nextAttemptGeneration, 36) + ".sock";
            ConnectionAttempt attempt = new ConnectionAttempt(nextAttemptGeneration,
                    pipe[0], pipe[1], handoffPath);
            connectionAttempt = attempt;
            return attempt;
        }
    }

    private void authenticate(DataInputStream input, OutputStream output, int fd)
            throws IOException {
        byte[] androidNonce = new byte[BridgeProtocol.NONCE_SIZE];
        new SecureRandom().nextBytes(androidNonce);
        output.write(BridgeProtocol.authInit(androidNonce));
        output.flush();
        BridgeProtocol.Message serverProof = BridgeProtocol.readTimed(
                input, fd, BridgeProtocol.AUTH_TIMEOUT_MS);
        output.write(BridgeProtocol.verifyServerAndBuildClientProof(
                getOrCreateToken(this), androidNonce, serverProof));
        output.flush();
        BridgeProtocol.verifyAuthOk(BridgeProtocol.readTimed(
                input, fd, BridgeProtocol.AUTH_TIMEOUT_MS));
    }

    private void dispatch(ConnectionState state, BridgeProtocol.Message message)
            throws IOException {
        ByteBuffer payload = ByteBuffer.wrap(message.payload).order(ByteOrder.LITTLE_ENDIAN);
        switch (message.type) {
            case BridgeProtocol.MSG_STREAM_START:
                requireRemaining(payload, 5);
                startRemoteStream(state, payload.getShort() & 0xffff,
                        payload.getShort() & 0xffff, payload.get() & 0xff);
                return;
            case BridgeProtocol.MSG_STREAM_STOP:
                requireRemaining(payload, 0);
                stopRemoteStream(state);
                return;
            case BridgeProtocol.MSG_IDR_REQUEST:
                requireRemaining(payload, 0);
                displaySession.requestIdr();
                return;
            case BridgeProtocol.MSG_INPUT_RESET:
                requireRemaining(payload, 0);
                displaySession.releaseRemoteInput();
                return;
            case BridgeProtocol.MSG_CLIPBOARD:
                dispatchClipboard(state, message.payload);
                return;
            case BridgeProtocol.MSG_CLIPBOARD_ACK:
                long ack = BridgeProtocol.parseClipboardAck(message.payload);
                long awaiting = state.awaitingClipboardAck.get();
                if (ack == awaiting) state.awaitingClipboardAck.compareAndSet(awaiting, 0);
                return;
            default:
                break;
        }

        Native nativeTransport = displaySession.nativeTransport();
        switch (message.type) {
            case BridgeProtocol.MSG_KEY:
                requireRemaining(payload, 5);
                int action = payload.get() & 0xff;
                int keycode = payload.getInt();
                if (action != 0 && action != 1) throw new IOException("Invalid key action");
                nativeTransport.sendKey(action, keycode);
                break;
            case BridgeProtocol.MSG_MOUSE_MOTION:
                requireRemaining(payload, 20);
                payload.getInt(); // Rust monotonic timestamp, currently informational.
                nativeTransport.sendMouseMotion(payload.getFloat(), payload.getFloat(),
                        payload.getFloat(), payload.getFloat());
                break;
            case BridgeProtocol.MSG_MOUSE_BUTTON:
                requireRemaining(payload, 5);
                nativeTransport.sendMouseButton(payload.getInt(), payload.get() != 0);
                break;
            case BridgeProtocol.MSG_MOUSE_AXIS:
                requireRemaining(payload, 8);
                float dx = payload.getFloat();
                float dy = payload.getFloat();
                if (dy != 0f) nativeTransport.sendMouseScroll(0, dy);
                if (dx != 0f) nativeTransport.sendMouseScroll(1, dx);
                break;
            default:
                Log.w(TAG, "Ignoring bridge message type " + message.type);
        }
    }

    private void dispatchClipboard(ConnectionState state, byte[] payload) throws IOException {
        long sequence = BridgeProtocol.clipboardSequence(payload);
        byte[] utf8 = BridgeProtocol.clipboardTextBytes(payload);
        String text = BridgeProtocol.decodeClipboard(utf8);
        displaySession.clipboard().applyBridgeClipboard(text, utf8, state.clipboardSink);
        queueControlFrame(state, BridgeProtocol.clipboardAck(sequence));
    }

    private void startRemoteStream(ConnectionState state, int width, int height, int fps)
            throws IOException {
        final long streamGeneration;
        synchronized (state.videoWriteLock) {
            synchronized (state.streamLock) {
                streamGeneration = nextStreamGeneration(state);
                state.videoQueue.clear();
                state.dropVideoUntilIdr.set(true);
            }
        }
        final int encodedWidth = align16(width);
        final int encodedHeight = align16(height);
        try {
            displaySession.startStream(width, height, fps,
                    (annexB, timestampMs, keyFrame) -> queueVideoFrame(state,
                            streamGeneration,
                            BridgeProtocol.videoFrame(encodedWidth, encodedHeight, width, height,
                                    timestampMs, keyFrame, annexB), keyFrame),
                    () -> failActiveStream(state, streamGeneration));
        } catch (IOException | RuntimeException e) {
            invalidateStream(state, streamGeneration);
            throw e;
        }
        if (isActiveStream(state, streamGeneration)) updateNotification(true, true);
    }

    private void stopRemoteStream(ConnectionState state) {
        synchronized (state.videoWriteLock) {
            invalidateStream(state, state.streamGeneration.get());
        }
        displaySession.releaseRemoteInput();
        displaySession.stopStream();
        updateNotification(true);
    }

    private void writerLoop(ConnectionState state, OutputStream output) {
        try {
            while (state.connected.get()) {
                byte[] control = state.controlQueue.poll();
                if (control == null) control = state.pendingClipboard.getAndSet(null);
                if (control != null) {
                    output.write(control);
                    output.flush();
                    continue;
                }
                VideoFrame video = state.videoQueue.poll(100, TimeUnit.MILLISECONDS);
                if (video == null) continue;
                synchronized (state.videoWriteLock) {
                    if (!isActiveStream(state, video.streamGeneration)
                            || state.predictionEpoch.get() != video.predictionEpoch) continue;
                    output.write(video.bytes);
                    output.flush();
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (IOException e) {
            if (state.connected.get()) Log.w(TAG, "Bridge writer failed", e);
        } finally {
            shutdownConnectionSocket(state);
        }
    }

    private void queueClipboard(ConnectionState state, byte[] utf8) {
        if (!isActiveConnection(state) || utf8 == null
                || utf8.length > BridgeProtocol.MAX_CLIPBOARD_SIZE) return;
        long sequence = nextClipboardSequence();
        state.awaitingClipboardAck.set(sequence);
        state.pendingClipboard.set(BridgeProtocol.clipboard(sequence, utf8));
    }

    private long nextClipboardSequence() {
        while (true) {
            long current = clipboardSequence.get();
            long sequence = current <= 0 ? 1 : current;
            long next = sequence == Long.MAX_VALUE ? 1 : sequence + 1;
            if (clipboardSequence.compareAndSet(current, next)) return sequence;
        }
    }

    private boolean queueVideoFrame(ConnectionState state, long streamGeneration,
                                    byte[] frame, boolean keyFrame) {
        synchronized (state.streamLock) {
            if (!isActiveStream(state, streamGeneration)) return false;
            if (state.dropVideoUntilIdr.get() && !keyFrame) return false;
            if (keyFrame) state.dropVideoUntilIdr.set(false);
            long predictionEpoch = state.predictionEpoch.get();
            if (state.videoQueue.offer(new VideoFrame(
                    streamGeneration, predictionEpoch, frame))) return false;
        }
        synchronized (state.videoWriteLock) {
            synchronized (state.streamLock) {
                if (!isActiveStream(state, streamGeneration)) return false;
                state.videoQueue.clear();
                advancePredictionEpoch(state);
                state.dropVideoUntilIdr.set(true);
            }
        }
        Log.w(TAG, "Video queue saturated; dropped prediction chain and requested IDR");
        return true;
    }

    private void queueControlFrame(ConnectionState state, byte[] frame) throws IOException {
        if (!isActiveConnection(state)) throw new IOException("Bridge client is closed");
        if (!state.controlQueue.offer(frame)) {
            throw new IOException("Bridge control queue is saturated");
        }
    }

    private void failActiveStream(ConnectionState state, long streamGeneration) {
        synchronized (state.streamLock) {
            if (!isActiveStream(state, streamGeneration)) return;
            state.streamGeneration.set(0);
            state.videoQueue.clear();
            state.dropVideoUntilIdr.set(true);
        }
        Log.e(TAG, "Hardware video route failed; reconnecting private bridge");
        shutdownConnectionSocket(state);
    }

    private long nextStreamGeneration(ConnectionState state) {
        state.nextStreamGeneration++;
        if (state.nextStreamGeneration <= 0) state.nextStreamGeneration = 1;
        state.streamGeneration.set(state.nextStreamGeneration);
        return state.nextStreamGeneration;
    }

    private void advancePredictionEpoch(ConnectionState state) {
        long next = state.predictionEpoch.get() + 1;
        if (next <= 0) next = 1;
        state.predictionEpoch.set(next);
    }

    private boolean isActiveStream(ConnectionState state, long streamGeneration) {
        return streamGeneration != 0 && state.streamGeneration.get() == streamGeneration
                && isActiveConnection(state);
    }

    private void invalidateStream(ConnectionState state, long streamGeneration) {
        synchronized (state.streamLock) {
            if (streamGeneration != 0 && state.streamGeneration.get() != streamGeneration) return;
            state.streamGeneration.set(0);
            state.videoQueue.clear();
            state.dropVideoUntilIdr.set(true);
        }
    }

    private static void requireRemaining(ByteBuffer buffer, int expected) throws IOException {
        if (buffer.remaining() != expected) {
            throw new IOException("Malformed bridge message: expected " + expected
                    + " bytes, received " + buffer.remaining());
        }
    }

    private void createNotificationChannel() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) {
            manager.createNotificationChannel(new NotificationChannel(CHANNEL_ID,
                    getString(R.string.bridge_notification_channel),
                    NotificationManager.IMPORTANCE_LOW));
        }
    }

    private android.app.Notification buildNotification(boolean connected) {
        return buildNotification(connected, false);
    }

    private android.app.Notification buildNotification(boolean connected, boolean streaming) {
        Intent settingsIntent = new Intent(this, SettingsActivity.class);
        PendingIntent pendingIntent = PendingIntent.getActivity(this, 0, settingsIntent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        int text = streaming ? R.string.bridge_notification_streaming
                : connected ? R.string.bridge_notification_connected
                : R.string.bridge_notification_waiting;
        return new android.app.Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.mipmap.ic_launcher)
                .setContentTitle(getString(R.string.bridge_notification_title))
                .setContentText(getString(text))
                .setContentIntent(pendingIntent)
                .setOngoing(true)
                .build();
    }

    private void updateNotification(boolean connected) {
        updateNotification(connected, false);
    }

    private void updateNotification(boolean connected, boolean streaming) {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) manager.notify(NOTIFICATION_ID,
                buildNotification(connected, streaming));
    }

    private boolean isActiveService() {
        return running && ACTIVE_SERVICE_GENERATION.get() == serviceGeneration;
    }

    private boolean isActiveServiceLocked() {
        return running && ACTIVE_SERVICE_GENERATION.get() == serviceGeneration;
    }

    private boolean isActiveConnection(ConnectionState state) {
        if (state == null || !isActiveService() || !state.connected.get()) return false;
        synchronized (connectionLock) {
            return connection == state;
        }
    }

    private ConnectionState takeConnection() {
        synchronized (connectionLock) {
            ConnectionState active = connection;
            connection = null;
            return active;
        }
    }

    private void cancelAttempt(ConnectionAttempt attempt) {
        if (attempt == null || !attempt.cancelled.compareAndSet(false, true)) return;
        closeQuietly(attempt.cancelWrite);
        synchronized (attempt) {
            shutdownAttemptSocketsLocked(attempt);
        }
    }

    private static void shutdownAttemptSocketsLocked(ConnectionAttempt attempt) {
        if (attempt.descriptor != null) {
            Native.shutdownBridgeSocket(attempt.descriptor.getFd());
        }
        if (attempt.writerDescriptor != null) {
            Native.shutdownBridgeSocket(attempt.writerDescriptor.getFd());
        }
    }

    private void shutdownConnectionSocket(ConnectionState state) {
        if (state == null || !state.socketShutdown.compareAndSet(false, true)) return;
        state.connected.set(false);
        Native.shutdownBridgeSocket(state.descriptor.getFd());
        Native.shutdownBridgeSocket(state.writerDescriptor.getFd());
    }

    private void retireConnection(ConnectionState state) {
        if (state == null || !state.retired.compareAndSet(false, true)) return;
        invalidateStream(state, state.streamGeneration.get());
        shutdownConnectionSocket(state);
        Thread writer = state.writerThread;
        if (writer != null && writer != Thread.currentThread()) {
            writer.interrupt();
            join(writer);
        }
        displaySession.clipboard().clearBridgeSink(state.clipboardSink);
        state.videoQueue.clear();
        state.controlQueue.clear();
        state.pendingClipboard.set(null);
        closeQuietly(state.writerDescriptor);
        closeQuietly(state.descriptor);
    }

    private static void closeQuietly(ParcelFileDescriptor descriptor) {
        if (descriptor == null) return;
        try { descriptor.close(); }
        catch (IOException ignored) {}
    }

    private static void join(Thread thread) {
        boolean interrupted = false;
        while (thread.isAlive()) {
            try { thread.join(); }
            catch (InterruptedException e) { interrupted = true; }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }

    private static void join(Thread thread, long timeoutMs) {
        try { thread.join(timeoutMs); }
        catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }

    private static int align16(int value) { return (value + 15) & ~15; }
}
