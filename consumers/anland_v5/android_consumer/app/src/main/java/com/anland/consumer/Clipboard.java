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
        String text = "";
        ClipData clip = cm.getPrimaryClip();
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
