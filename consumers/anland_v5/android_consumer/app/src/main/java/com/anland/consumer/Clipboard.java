package com.anland.consumer;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** One clipboard listener shared by the default display transport and bridge service. */
public final class Clipboard {
    interface BridgeSink {
        void onClipboard(byte[] utf8);
        /** Android copied files (SAF URIs): offer them to the bridge. */
        void onClipboardFiles(String[] names, long[] sizes, android.net.Uri[] uris);
    }

    private static final String TAG = "AnlandClipboard";
    private final Context context;
    private final Native mNative;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private String mLastSentClip;
    private boolean mNativeListening;
    private boolean mListenerRegistered;
    private BridgeSink bridgeSink;

    Clipboard(Context context, Native n) {
        this.context = context.getApplicationContext();
        this.mNative = n;
    }

    private final ClipboardManager.OnPrimaryClipChangedListener clipListener =
            () -> pushClipboard(false, bridgeSinkSnapshot());

    public void nativeSetClipboardBytes(byte[] utf8) {
        if (utf8 == null) return;
        final String text;
        try {
            text = BridgeProtocol.decodeClipboard(utf8);
        } catch (java.io.IOException e) {
            Log.w(TAG, "Rejected invalid producer clipboard", e);
            return;
        }
        BridgeSink sink = bridgeSinkSnapshot();
        runOnMain(() -> applyProducerClipboard(text, utf8, sink));
    }

    public void nativeClipboardSync() {
        BridgeSink sink = bridgeSinkSnapshot();
        runOnMain(() -> pushClipboard(true, sink));
    }

    public void nativeClipListening(boolean enable) {
        runOnMain(() -> {
            mNativeListening = enable;
            updateListenerRegistration();
        });
    }

    void setBridgeSink(BridgeSink sink) {
        synchronized (this) {
            bridgeSink = sink;
        }
        runOnMain(() -> {
            updateListenerRegistration();
            if (sink != null && bridgeSinkSnapshot() == sink) pushClipboard(true, sink);
        });
    }

    void clearBridgeSink(BridgeSink expected) {
        synchronized (this) {
            if (bridgeSink != expected) return;
            bridgeSink = null;
        }
        runOnMain(this::updateListenerRegistration);
    }

    void applyBridgeClipboard(String text, byte[] utf8, BridgeSink expected) {
        runOnMain(() -> {
            if (bridgeSinkSnapshot() != expected || !isAllowedText(text, utf8)) return;
            mLastSentClip = text;
            mNative.sendClipboard(utf8);
            setSystemClipboard(text);
        });
    }

    public void pushClipboard() {
        BridgeSink sink = bridgeSinkSnapshot();
        runOnMain(() -> pushClipboard(false, sink));
    }

    void syncClipboard() {
        BridgeSink sink = bridgeSinkSnapshot();
        runOnMain(() -> pushClipboard(true, sink));
    }

    private void applyProducerClipboard(String text, byte[] utf8, BridgeSink sink) {
        mLastSentClip = text;
        setSystemClipboard(text);
        if (sink != null) sink.onClipboard(Arrays.copyOf(utf8, utf8.length));
    }

    private void pushClipboard(boolean force, BridgeSink sink) {
        ClipboardManager cm = context.getSystemService(ClipboardManager.class);
        if (cm == null) return;
        ClipData clip = cm.getPrimaryClip();
        // Files first: a content-URI item that is not plain text. SAF picks
        // (content URIs) carry the file; a plain-text clip has no URI.
        if (clip != null && clip.getItemCount() > 0) {
            android.net.Uri[] uris = collectFileUris(clip);
            if (uris.length > 0) {
                String[] names = resolveNames(uris);
                long[] sizes = resolveSizes(uris);
                if (sink != null) sink.onClipboardFiles(names, sizes, uris);
                return;
            }
        }
        String text = "";
        if (clip != null && clip.getItemCount() > 0) {
            CharSequence value = clip.getItemAt(0).coerceToText(context);
            if (value != null) text = value.toString();
        }
        if (!force && text.equals(mLastSentClip)) return;
        byte[] utf8 = encodeStrict(text);
        if (utf8 == null) {
            Log.w(TAG, "Rejected invalid or oversized Android clipboard");
            return;
        }
        mLastSentClip = text;
        mNative.sendClipboard(utf8);
        if (sink != null) sink.onClipboard(Arrays.copyOf(utf8, utf8.length));
    }

    /** Collect content URIs from the primary clip (files), skipping text-only items. */
    private android.net.Uri[] collectFileUris(ClipData clip) {
        int count = clip.getItemCount();
        int n = 0;
        for (int i = 0; i < count; i++) {
            if (clip.getItemAt(i).getUri() != null) n++;
        }
        if (n == 0) return new android.net.Uri[0];
        android.net.Uri[] uris = new android.net.Uri[n];
        int k = 0;
        for (int i = 0; i < count; i++) {
            android.net.Uri uri = clip.getItemAt(i).getUri();
            if (uri != null) uris[k++] = uri;
        }
        return uris;
    }

    /** Resolve display names via ContentResolver (OpenableColumns.DISPLAY_NAME). */
    private String[] resolveNames(android.net.Uri[] uris) {
        String[] names = new String[uris.length];
        for (int i = 0; i < uris.length; i++) {
            String name = null;
            try (android.database.Cursor c = context.getContentResolver()
                    .query(uris[i], new String[] { android.provider.OpenableColumns.DISPLAY_NAME },
                            null, null, null)) {
                if (c != null && c.moveToFirst()) {
                    name = c.getString(0);
                }
            } catch (Exception e) {
                Log.w(TAG, "resolve name failed for " + uris[i], e);
            }
            names[i] = (name != null && !name.isEmpty()) ? name : "file_" + i;
        }
        return names;
    }

    /** Resolve sizes via ContentResolver (OpenableColumns.SIZE; unknown → 0). */
    private long[] resolveSizes(android.net.Uri[] uris) {
        long[] sizes = new long[uris.length];
        for (int i = 0; i < uris.length; i++) {
            long size = 0;
            try (android.database.Cursor c = context.getContentResolver()
                    .query(uris[i], new String[] { android.provider.OpenableColumns.SIZE },
                            null, null, null)) {
                if (c != null && c.moveToFirst() && !c.isNull(0)) {
                    size = c.getLong(0);
                }
            } catch (Exception e) {
                Log.w(TAG, "resolve size failed for " + uris[i], e);
            }
            sizes[i] = size;
        }
        return sizes;
    }

    private synchronized BridgeSink bridgeSinkSnapshot() {
        return bridgeSink;
    }

    private void updateListenerRegistration() {
        ClipboardManager cm = context.getSystemService(ClipboardManager.class);
        if (cm == null) return;
        boolean wanted = mNativeListening || bridgeSinkSnapshot() != null;
        if (wanted == mListenerRegistered) return;
        if (wanted) cm.addPrimaryClipChangedListener(clipListener);
        else cm.removePrimaryClipChangedListener(clipListener);
        mListenerRegistered = wanted;
    }

    private void setSystemClipboard(String text) {
        ClipboardManager cm = context.getSystemService(ClipboardManager.class);
        if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("anland", text));
    }

    static byte[] encodeStrict(String text) {
        if (text == null || text.indexOf('\0') >= 0) return null;
        try {
            ByteBuffer encoded = StandardCharsets.UTF_8.newEncoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .encode(CharBuffer.wrap(text));
            if (encoded.remaining() > BridgeProtocol.MAX_CLIPBOARD_SIZE) return null;
            byte[] result = new byte[encoded.remaining()];
            encoded.get(result);
            return result;
        } catch (CharacterCodingException e) {
            return null;
        }
    }

    private static boolean isAllowedText(String text, byte[] utf8) {
        if (text == null || utf8 == null || utf8.length > BridgeProtocol.MAX_CLIPBOARD_SIZE
                || text.indexOf('\0') >= 0) return false;
        byte[] strict = encodeStrict(text);
        return strict != null && Arrays.equals(strict, utf8);
    }

    private void runOnMain(Runnable runnable) {
        if (Looper.myLooper() == Looper.getMainLooper()) runnable.run();
        else mainHandler.post(runnable);
    }
}
