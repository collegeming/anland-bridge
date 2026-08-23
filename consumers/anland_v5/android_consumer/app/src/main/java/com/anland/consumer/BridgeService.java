package com.anland.consumer;

import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.TimeUnit;

public final class BridgeService extends Service {
    static final String PREFS_NAME = "anland_settings";
    static final String KEY_ENABLED = "bridge_service_enabled";
    static final String KEY_TOKEN = "bridge_service_token";
    static final String DEFAULT_ENDPOINT = "tcp://127.0.0.1:33910";

    private static final String TAG = "AnlandBridge";
    private static final String CHANNEL_ID = "anland_bridge";
    private static final int NOTIFICATION_ID = 33910;
    private static final int LISTEN_PORT = 33910;
    private static final int VIDEO_QUEUE_CAPACITY = 8;
    private static final int CONTROL_QUEUE_CAPACITY = 16;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final ArrayBlockingQueue<byte[]> videoQueue =
            new ArrayBlockingQueue<>(VIDEO_QUEUE_CAPACITY);
    private final ArrayBlockingQueue<byte[]> controlQueue =
            new ArrayBlockingQueue<>(CONTROL_QUEUE_CAPACITY);
    private volatile boolean running;
    private volatile boolean clientConnected;
    private volatile boolean dropVideoUntilIdr;
    private volatile Native remoteNative;
    private volatile MediaCodecEncoder encoder;
    private ServerSocket serverSocket;
    private volatile Socket clientSocket;
    private Thread serverThread;
    private Thread writerThread;
    private boolean clipboardListening;
    private volatile String lastRemoteClipboard;
    private final Set<Integer> pressedKeys = new HashSet<>();
    private final Set<Integer> pressedButtons = new HashSet<>();

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

    public static void setEnabled(Context context, boolean enabled) {
        if (enabled) getOrCreateToken(context);
        Intent intent = new Intent(context, BridgeService.class);
        if (enabled) {
            context.startForegroundService(intent);
        } else {
            context.stopService(intent);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
        Intent settingsIntent = new Intent(this, SettingsActivity.class);
        PendingIntent pendingIntent = PendingIntent.getActivity(
                this, 0, settingsIntent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        android.app.Notification notification =
                new android.app.Notification.Builder(this, CHANNEL_ID)
                        .setSmallIcon(R.mipmap.ic_launcher)
                        .setContentTitle(getString(R.string.bridge_notification_title))
                        .setContentText(getString(R.string.bridge_notification_waiting))
                        .setContentIntent(pendingIntent)
                        .setOngoing(true)
                        .build();
        startForeground(NOTIFICATION_ID, notification);

        running = true;
        serverThread = new Thread(this::serverLoop, "anland-bridge-server");
        serverThread.start();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        boolean enabled = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getBoolean(KEY_ENABLED, false);
        if (!enabled) {
            stopSelf();
            return START_NOT_STICKY;
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        closeServerSocket();
        closeClientSocket();
        stopRemoteSession();
        if (serverThread != null) {
            try {
                serverThread.join(1000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
        mainHandler.post(() -> setClipboardListening(false));
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void serverLoop() {
        try {
            serverSocket = new ServerSocket(LISTEN_PORT, 1,
                    InetAddress.getByName("127.0.0.1"));
            Log.i(TAG, "Listening on " + DEFAULT_ENDPOINT);
            while (running) {
                try (Socket socket = serverSocket.accept()) {
                    socket.setTcpNoDelay(true);
                    handleClient(socket);
                } catch (IOException e) {
                    if (running) Log.w(TAG, "Bridge client disconnected", e);
                } finally {
                    stopRemoteSession();
                }
            }
        } catch (IOException e) {
            if (running) Log.e(TAG, "Unable to listen on bridge endpoint", e);
        }
    }

    private void handleClient(Socket socket) throws IOException {
        clientSocket = socket;
        try {
            DataInputStream input = new DataInputStream(
                    new BufferedInputStream(socket.getInputStream()));
            authenticate(input);

            BufferedOutputStream clientOutput = new BufferedOutputStream(socket.getOutputStream());
            videoQueue.clear();
            controlQueue.clear();
            dropVideoUntilIdr = true;
            lastRemoteClipboard = null;
            clientConnected = true;
            mainHandler.post(() -> {
                setClipboardListening(true);
                pushSystemClipboard();
            });
            writerThread = new Thread(() -> writerLoop(socket, clientOutput),
                    "anland-bridge-writer");
            writerThread.start();

            BridgeProtocol.Message message;
            while (running && clientConnected
                    && (message = BridgeProtocol.read(input)) != null) {
                dispatch(message);
            }
        } finally {
            clientConnected = false;
            mainHandler.post(() -> setClipboardListening(false));
            stopRemoteSession();
            videoQueue.clear();
            controlQueue.clear();
            if (clientSocket == socket) clientSocket = null;
            try {
                socket.close();
            } catch (IOException ignored) {
            }
            if (writerThread != null) {
                writerThread.interrupt();
                try {
                    writerThread.join(1000);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
                writerThread = null;
            }
        }
    }

    private void authenticate(DataInputStream input) throws IOException {
        BridgeProtocol.Message hello = BridgeProtocol.read(input);
        if (hello == null || hello.type != BridgeProtocol.MSG_HELLO) {
            throw new IOException("Missing bridge authentication message");
        }
        byte[] expected = getOrCreateToken(this).getBytes(StandardCharsets.US_ASCII);
        if (!MessageDigest.isEqual(expected, hello.payload)) {
            throw new IOException("Bridge authentication failed");
        }
    }

    private void configureNative(Native nativeTransport, SharedPreferences prefs) {
        String socketPath = prefs.getString("socket_path",
                "/data/local/tmp/display_daemon.sock");
        boolean useRoot = prefs.getBoolean("use_root", true);
        String helperPath = getApplicationInfo().nativeLibraryDir + "/libfdhelper.so";
        String bridgePath = getCacheDir().getAbsolutePath() + "/anland_rdp_fdbridge.sock";
        nativeTransport.configure(socketPath, useRoot, helperPath, bridgePath);
    }

    private synchronized void dispatch(BridgeProtocol.Message message) throws IOException {
        ByteBuffer payload = ByteBuffer.wrap(message.payload).order(ByteOrder.LITTLE_ENDIAN);
        switch (message.type) {
            case BridgeProtocol.MSG_STREAM_START:
                requireRemaining(payload, 5);
                startRemoteSession(payload.getShort() & 0xffff,
                        payload.getShort() & 0xffff, payload.get() & 0xff);
                return;
            case BridgeProtocol.MSG_STREAM_STOP:
                requireRemaining(payload, 0);
                stopRemoteSession();
                return;
            case BridgeProtocol.MSG_IDR_REQUEST:
                requireRemaining(payload, 0);
                MediaCodecEncoder activeEncoder = encoder;
                if (activeEncoder != null) activeEncoder.requestIdr();
                return;
            default:
                break;
        }

        if (message.type == BridgeProtocol.MSG_CLIPBOARD) {
            lastRemoteClipboard = new String(message.payload, StandardCharsets.UTF_8);
            Native nativeTransport = remoteNative;
            if (nativeTransport != null) nativeTransport.sendClipboard(message.payload);
            String text = lastRemoteClipboard;
            mainHandler.post(() -> setSystemClipboard(text));
            queueControlFrame(BridgeProtocol.frame(
                    BridgeProtocol.MSG_CLIPBOARD, message.payload));
            return;
        }

        Native nativeTransport = remoteNative;
        if (nativeTransport == null) return;
        switch (message.type) {
            case BridgeProtocol.MSG_KEY:
                requireRemaining(payload, 5);
                int action = payload.get() & 0xff;
                int keycode = payload.getInt();
                if (action == 0) pressedKeys.add(keycode);
                else if (action == 1) pressedKeys.remove(keycode);
                else throw new IOException("Invalid key action " + action);
                nativeTransport.sendKey(action, keycode);
                break;
            case BridgeProtocol.MSG_MOUSE_MOTION:
                requireRemaining(payload, 20);
                payload.getInt();
                nativeTransport.sendMouseMotion(payload.getFloat(), payload.getFloat(),
                        payload.getFloat(), payload.getFloat());
                break;
            case BridgeProtocol.MSG_MOUSE_BUTTON:
                requireRemaining(payload, 5);
                int button = payload.getInt();
                boolean pressed = payload.get() != 0;
                if (pressed) pressedButtons.add(button);
                else pressedButtons.remove(button);
                nativeTransport.sendMouseButton(button, pressed);
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

    private static void requireRemaining(ByteBuffer buffer, int expected) throws IOException {
        if (buffer.remaining() != expected) {
            throw new IOException("Malformed bridge message: expected " + expected
                    + " bytes, received " + buffer.remaining());
        }
    }

    private synchronized void startRemoteSession(int displayWidth, int displayHeight,
                                                  int fps) throws IOException {
        if (remoteNative != null || encoder != null) return;

        if (displayWidth < 64 || displayWidth > 8192
                || displayHeight < 64 || displayHeight > 8192
                || fps < 1 || fps > 60) {
            throw new IOException("Invalid stream geometry or frame rate");
        }
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);

        int encodedWidth = align16(displayWidth);
        int encodedHeight = align16(displayHeight);
        long requestedBitRate = (long) displayWidth * displayHeight * fps / 8L;
        int bitRate = (int) Math.max(4_000_000L,
                Math.min(20_000_000L, requestedBitRate));

        final int encW = encodedWidth;
        final int encH = encodedHeight;
        final int dispW = displayWidth;
        final int dispH = displayHeight;
        MediaCodecEncoder newEncoder = null;
        Native newNative = null;
        try {
            newEncoder = new MediaCodecEncoder(encW, encH, fps, bitRate,
                    (annexB, timestampMs, keyFrame) -> queueVideoFrame(
                            BridgeProtocol.videoFrame(encW, encH, dispW, dispH,
                                    timestampMs, keyFrame, annexB), keyFrame));
            MainActivity.setRemoteBridgeActiveSync(true);
            newNative = new Native();
            configureNative(newNative, prefs);
            newNative.startRemote(newEncoder.getInputSurface(), this,
                    displayWidth, displayHeight, encodedWidth, encodedHeight, fps);
            encoder = newEncoder;
            remoteNative = newNative;
            updateNotification(true);
        } catch (RuntimeException | IOException e) {
            if (newNative != null) {
                newNative.stop();
                newNative.destroy();
            }
            if (newEncoder != null) newEncoder.close();
            MainActivity.setRemoteBridgeActiveSync(false);
            updateNotification(false);
            if (e instanceof IOException) throw (IOException) e;
            throw new IOException("Unable to start remote anland session", e);
        }
    }

    private synchronized void stopRemoteSession() {
        Native nativeTransport = remoteNative;
        remoteNative = null;
        if (nativeTransport != null) {
            for (int keycode : pressedKeys) nativeTransport.sendKey(1, keycode);
            for (int button : pressedButtons) nativeTransport.sendMouseButton(button, false);
            nativeTransport.stop();
            nativeTransport.destroy();
        }
        pressedKeys.clear();
        pressedButtons.clear();
        MediaCodecEncoder activeEncoder = encoder;
        encoder = null;
        if (activeEncoder != null) activeEncoder.close();
        MainActivity.setRemoteBridgeActiveSync(false);
        updateNotification(false);
    }

    private void writerLoop(Socket socket, OutputStream output) {
        try {
            while (running && clientConnected) {
                byte[] frame = controlQueue.poll();
                if (frame == null) frame = videoQueue.poll(100, TimeUnit.MILLISECONDS);
                if (frame == null) continue;
                output.write(frame);
                output.flush();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } catch (IOException e) {
            if (running && clientConnected) Log.w(TAG, "Bridge writer failed", e);
            try {
                socket.close();
            } catch (IOException ignored) {
            }
        } finally {
            clientConnected = false;
        }
    }

    private void queueVideoFrame(byte[] frame, boolean keyFrame) {
        if (!clientConnected) return;
        if (dropVideoUntilIdr && !keyFrame) return;
        if (keyFrame) dropVideoUntilIdr = false;
        if (videoQueue.offer(frame)) return;

        videoQueue.clear();
        dropVideoUntilIdr = true;
        MediaCodecEncoder activeEncoder = encoder;
        if (activeEncoder != null) activeEncoder.requestIdr();
        Log.w(TAG, "Video queue saturated; dropped prediction chain and requested IDR");
    }

    private void queueControlFrame(byte[] frame) throws IOException {
        if (!clientConnected) throw new IOException("Bridge client is closed");
        if (!controlQueue.offer(frame)) {
            controlQueue.poll();
            if (!controlQueue.offer(frame)) {
                throw new IOException("Bridge control queue is saturated");
            }
        }
    }

    public void nativeSetClipboardText(String text) {
        lastRemoteClipboard = text;
        try {
            queueControlFrame(BridgeProtocol.frame(BridgeProtocol.MSG_CLIPBOARD,
                    text.getBytes(StandardCharsets.UTF_8)));
        } catch (IOException e) {
            Log.w(TAG, "Unable to send clipboard to RDP server", e);
        }
        mainHandler.post(() -> setSystemClipboard(text));
    }

    public void nativeClipboardSync() {
        mainHandler.post(() -> pushSystemClipboard(true));
    }

    public void nativeClipListening(boolean enabled) {
        mainHandler.post(() -> setClipboardListening(enabled || clientConnected));
    }

    private final ClipboardManager.OnPrimaryClipChangedListener clipboardListener =
            this::pushSystemClipboard;

    private void setClipboardListening(boolean enabled) {
        ClipboardManager clipboardManager = getSystemService(ClipboardManager.class);
        if (clipboardManager == null || clipboardListening == enabled) return;
        if (enabled) clipboardManager.addPrimaryClipChangedListener(clipboardListener);
        else clipboardManager.removePrimaryClipChangedListener(clipboardListener);
        clipboardListening = enabled;
    }

    private void setSystemClipboard(String text) {
        if (text == null) return;
        ClipboardManager clipboardManager = getSystemService(ClipboardManager.class);
        if (clipboardManager != null) {
            clipboardManager.setPrimaryClip(ClipData.newPlainText("anland RDP", text));
        }
    }

    private void pushSystemClipboard() {
        pushSystemClipboard(false);
    }

    private void pushSystemClipboard(boolean forceNativeSync) {
        ClipboardManager clipboardManager = getSystemService(ClipboardManager.class);
        if (clipboardManager == null) return;

        String text = "";
        ClipData clip = clipboardManager.getPrimaryClip();
        if (clip != null && clip.getItemCount() > 0) {
            CharSequence value = clip.getItemAt(0).coerceToText(this);
            if (value != null) text = value.toString();
        }
        boolean changed = !text.equals(lastRemoteClipboard);
        if (!changed && !forceNativeSync) return;
        if (changed) lastRemoteClipboard = text;

        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        Native nativeTransport = remoteNative;
        if (nativeTransport != null) nativeTransport.sendClipboard(bytes);
        if (!changed) return;
        try {
            queueControlFrame(BridgeProtocol.frame(BridgeProtocol.MSG_CLIPBOARD, bytes));
        } catch (IOException e) {
            Log.w(TAG, "Unable to mirror Android clipboard to RDP server", e);
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

    private void updateNotification(boolean connected) {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager == null) return;
        android.app.Notification notification =
                new android.app.Notification.Builder(this, CHANNEL_ID)
                        .setSmallIcon(R.mipmap.ic_launcher)
                        .setContentTitle(getString(R.string.bridge_notification_title))
                        .setContentText(getString(connected
                                ? R.string.bridge_notification_connected
                                : R.string.bridge_notification_waiting))
                        .setOngoing(true)
                        .build();
        manager.notify(NOTIFICATION_ID, notification);
    }

    private void closeClientSocket() {
        Socket activeSocket = clientSocket;
        clientSocket = null;
        if (activeSocket == null) return;
        try {
            activeSocket.close();
        } catch (IOException ignored) {
        }
    }

    private void closeServerSocket() {
        if (serverSocket == null) return;
        try {
            serverSocket.close();
        } catch (IOException ignored) {
        }
    }

    private static int align16(int value) {
        return (value + 15) & ~15;
    }
}
