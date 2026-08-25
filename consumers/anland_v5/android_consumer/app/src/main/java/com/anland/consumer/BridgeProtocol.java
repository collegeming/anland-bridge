package com.anland.consumer;

import android.os.SystemClock;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.net.SocketTimeoutException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.Arrays;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** Wire constants shared with lamco-anland-bridge. */
final class BridgeProtocol {
    static final int VERSION = 1;

    static final int MSG_KEY = 1;
    static final int MSG_MOUSE_MOTION = 2;
    static final int MSG_MOUSE_BUTTON = 3;
    static final int MSG_MOUSE_AXIS = 4;
    static final int MSG_CLIPBOARD = 5;
    static final int MSG_CLIPBOARD_ACK = 6;
    static final int MSG_INPUT_RESET = 7;
    // CLIPBOARD_IMAGE is 8 on the server (7 is INPUT_RESET here).
    static final int MSG_CLIPBOARD_IMAGE = 8;
    static final int MSG_VIDEO_FRAME = 16;
    static final int MSG_IDR_REQUEST = 17;
    static final int MSG_STREAM_START = 18;
    static final int MSG_STREAM_STOP = 19;
    static final int MSG_FILE_LIST = 23;
    static final int MSG_FILE_CONTENT_REQUEST = 24;
    static final int MSG_FILE_CONTENT_RESPONSE = 25;

    static final int MSG_AUTH_INIT = 32;
    static final int MSG_AUTH_SERVER_PROOF = 33;
    static final int MSG_AUTH_CLIENT_PROOF = 34;
    static final int MSG_AUTH_OK = 35;

    static final int NONCE_SIZE = 32;
    static final int HMAC_SIZE = 32;
    static final int MAX_CLIPBOARD_SIZE = 1024 * 1024;
    static final int MAX_FRAME_SIZE = 16 * 1024 * 1024;
    static final int AUTH_TIMEOUT_MS = 10_000;
    static final int IO_TIMEOUT_MS = 30_000;
    static final int SEND_TIMEOUT_MS = 5_000;

    /* Authentication v1, byte exact:
     *   AUTH_INIT payload         = version:u8 || android_nonce[32]
     *   AUTH_SERVER_PROOF payload = version:u8 || rust_nonce[32] || server_hmac[32]
     *   AUTH_CLIENT_PROOF payload = version:u8 || client_hmac[32]
     *   AUTH_OK payload           = version:u8
     *
     * token_bytes is the decoded 32-hex-character token (exactly 16 bytes).
     * session_key = HMAC-SHA256(token_bytes, KDF_CONTEXT)
     * transcript  = MAGIC || version:u8 || android_nonce || rust_nonce
     * server_hmac = HMAC-SHA256(session_key, SERVER_CONTEXT || transcript)
     * client_hmac = HMAC-SHA256(session_key, CLIENT_CONTEXT || transcript)
     * Rust proves first. Android compares with MessageDigest.isEqual before it emits
     * its proof. The token itself is never put on the wire. */
    static final byte[] MAGIC = ascii("ANLB");
    static final byte[] KDF_CONTEXT = ascii("ANLAND-BRIDGE-HMAC-SHA256-v1");
    static final byte[] SERVER_CONTEXT = ascii("ANLAND-BRIDGE-SERVER-v1");
    static final byte[] CLIENT_CONTEXT = ascii("ANLAND-BRIDGE-ANDROID-v1");

    static final class Message {
        final int type;
        final byte[] payload;

        Message(int type, byte[] payload) {
            this.type = type;
            this.payload = payload;
        }
    }

    private BridgeProtocol() {}

    static Message read(DataInputStream input) throws IOException {
        int length;
        try {
            length = input.readInt();
        } catch (EOFException e) {
            return null;
        }
        if (length < 1 || length > MAX_FRAME_SIZE) {
            throw new IOException("Invalid bridge frame length " + length);
        }

        int type = input.readUnsignedByte();
        byte[] payload = new byte[length - 1];
        input.readFully(payload);
        return new Message(type, payload);
    }

    static Message readTimed(DataInputStream input, int fd, int timeoutMs) throws IOException {
        long deadline = SystemClock.elapsedRealtime() + timeoutMs;
        byte[] lengthBytes = new byte[4];
        readFullyUntil(input, lengthBytes, 0, lengthBytes.length, fd, deadline);
        return readFrameBody(input, fd, deadline, lengthBytes);
    }

    static Message readActive(DataInputStream input, int fd) throws IOException {
        Native.setBridgeSocketReceiveTimeout(fd, 0);
        int firstLengthByte = input.read();
        if (firstLengthByte < 0) return null;

        long deadline = SystemClock.elapsedRealtime() + IO_TIMEOUT_MS;
        byte[] lengthBytes = new byte[4];
        lengthBytes[0] = (byte) firstLengthByte;
        readFullyUntil(input, lengthBytes, 1, 3, fd, deadline);
        Message message = readFrameBody(input, fd, deadline, lengthBytes);
        Native.setBridgeSocketReceiveTimeout(fd, 0);
        return message;
    }

    private static Message readFrameBody(DataInputStream input, int fd, long deadline,
                                         byte[] lengthBytes) throws IOException {
        int length = ByteBuffer.wrap(lengthBytes).getInt();
        if (length < 1 || length > MAX_FRAME_SIZE) {
            throw new IOException("Invalid bridge frame length " + length);
        }
        byte[] frame = new byte[length];
        readFullyUntil(input, frame, 0, frame.length, fd, deadline);
        return new Message(frame[0] & 0xff, Arrays.copyOfRange(frame, 1, frame.length));
    }

    private static void readFullyUntil(DataInputStream input, byte[] buffer, int offset,
                                       int length, int fd, long deadline) throws IOException {
        int end = offset + length;
        while (offset < end) {
            long remaining = deadline - SystemClock.elapsedRealtime();
            if (remaining <= 0) {
                throw new SocketTimeoutException("Bridge frame progress timed out");
            }
            Native.setBridgeSocketReceiveTimeout(fd,
                    (int) Math.min(Integer.MAX_VALUE, remaining));
            int read = input.read(buffer, offset, end - offset);
            if (read < 0) throw new EOFException("Bridge frame ended before its declared length");
            if (read == 0) continue;
            offset += read;
        }
    }

    static byte[] frame(int type, byte[] payload) {
        if (payload == null) payload = new byte[0];
        if (payload.length + 1 > MAX_FRAME_SIZE) {
            throw new IllegalArgumentException("Bridge frame is too large");
        }
        ByteBuffer buffer = ByteBuffer.allocate(5 + payload.length);
        buffer.putInt(1 + payload.length);
        buffer.put((byte) type);
        buffer.put(payload);
        return buffer.array();
    }

    static byte[] authInit(byte[] androidNonce) {
        requireLength(androidNonce, NONCE_SIZE, "Android nonce");
        ByteBuffer payload = ByteBuffer.allocate(1 + NONCE_SIZE);
        payload.put((byte) VERSION).put(androidNonce);
        return frame(MSG_AUTH_INIT, payload.array());
    }

    static byte[] verifyServerAndBuildClientProof(String tokenHex, byte[] androidNonce,
                                                   Message serverProof) throws IOException {
        if (serverProof == null || serverProof.type != MSG_AUTH_SERVER_PROOF
                || serverProof.payload.length != 1 + NONCE_SIZE + HMAC_SIZE
                || (serverProof.payload[0] & 0xff) != VERSION) {
            throw new IOException("Invalid bridge server proof");
        }
        requireLength(androidNonce, NONCE_SIZE, "Android nonce");
        byte[] rustNonce = Arrays.copyOfRange(serverProof.payload, 1, 1 + NONCE_SIZE);
        byte[] supplied = Arrays.copyOfRange(serverProof.payload,
                1 + NONCE_SIZE, serverProof.payload.length);
        byte[] key = deriveSessionKey(decodeToken(tokenHex));
        byte[] transcript = transcript(androidNonce, rustNonce);
        byte[] expected = hmac(key, SERVER_CONTEXT, transcript);
        if (!MessageDigest.isEqual(expected, supplied)) {
            throw new IOException("Bridge server authentication failed");
        }
        byte[] proof = hmac(key, CLIENT_CONTEXT, transcript);
        ByteBuffer payload = ByteBuffer.allocate(1 + HMAC_SIZE);
        payload.put((byte) VERSION).put(proof);
        return frame(MSG_AUTH_CLIENT_PROOF, payload.array());
    }

    static void verifyAuthOk(Message message) throws IOException {
        if (message == null || message.type != MSG_AUTH_OK || message.payload.length != 1
                || (message.payload[0] & 0xff) != VERSION) {
            throw new IOException("Bridge did not confirm authentication");
        }
    }

    static byte[] clipboard(long sequence, byte[] utf8) {
        if (utf8 == null || utf8.length > MAX_CLIPBOARD_SIZE) {
            throw new IllegalArgumentException("Clipboard payload is too large");
        }
        ByteBuffer payload = ByteBuffer.allocate(8 + utf8.length).order(ByteOrder.LITTLE_ENDIAN);
        payload.putLong(sequence).put(utf8);
        return frame(MSG_CLIPBOARD, payload.array());
    }

    static long clipboardSequence(byte[] payload) throws IOException {
        if (payload.length < 8 || payload.length - 8 > MAX_CLIPBOARD_SIZE) {
            throw new IOException("Invalid clipboard payload length");
        }
        long sequence = littleEndian(payload).getLong();
        if (sequence == 0) throw new IOException("Invalid zero clipboard sequence");
        return sequence;
    }

    static byte[] clipboardTextBytes(byte[] payload) throws IOException {
        clipboardSequence(payload);
        return Arrays.copyOfRange(payload, 8, payload.length);
    }

    static String decodeClipboard(byte[] utf8) throws IOException {
        if (utf8.length > MAX_CLIPBOARD_SIZE) throw new IOException("Clipboard is too large");
        for (byte value : utf8) {
            if (value == 0) throw new IOException("Clipboard contains an embedded NUL");
        }
        try {
            return StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(utf8)).toString();
        } catch (CharacterCodingException e) {
            throw new IOException("Clipboard is not strict UTF-8", e);
        }
    }

    static byte[] clipboardAck(long sequence) {
        return frame(MSG_CLIPBOARD_ACK, ByteBuffer.allocate(8)
                .order(ByteOrder.LITTLE_ENDIAN).putLong(sequence).array());
    }

    static long parseClipboardAck(byte[] payload) throws IOException {
        if (payload.length != 8) throw new IOException("Invalid clipboard ACK");
        long sequence = littleEndian(payload).getLong();
        if (sequence == 0) throw new IOException("Invalid zero clipboard ACK sequence");
        return sequence;
    }

    /* Clipboard image: MSG_CLIPBOARD_IMAGE = sequence:u64 || png[]. */
    static byte[] clipboardImage(long sequence, byte[] png) {
        if (png == null || png.length == 0) {
            throw new IllegalArgumentException("Clipboard image is empty");
        }
        ByteBuffer payload = ByteBuffer.allocate(8 + png.length).order(ByteOrder.LITTLE_ENDIAN);
        payload.putLong(sequence).put(png);
        return frame(MSG_CLIPBOARD_IMAGE, payload.array());
    }

    static long clipboardImageSequence(byte[] payload) throws IOException {
        if (payload.length < 9) throw new IOException("Invalid clipboard image");
        long sequence = littleEndian(payload).getLong();
        if (sequence == 0) throw new IOException("Invalid zero clipboard image sequence");
        return sequence;
    }

    static byte[] clipboardImageBytes(byte[] payload) throws IOException {
        clipboardImageSequence(payload);
        return Arrays.copyOfRange(payload, 8, payload.length);
    }

    /* Files: MSG_FILE_LIST = sequence:u64 || count:u32 ||
     *         entries[] (name_len:u16 || name || size:u64).
     * MSG_FILE_CONTENT_REQUEST = request_id:u32 || index:u32 || offset:u64 || length:u32.
     * MSG_FILE_CONTENT_RESPONSE = request_id:u32 || data[]. */
    static byte[] fileList(long sequence, String[] names, long[] sizes) {
        int count = Math.min(names.length, sizes.length);
        int capacity = 12;
        for (int i = 0; i < count; i++) {
            capacity += 2 + names[i].getBytes(StandardCharsets.UTF_8).length + 8;
        }
        ByteBuffer payload = ByteBuffer.allocate(capacity).order(ByteOrder.LITTLE_ENDIAN);
        payload.putLong(sequence).putInt(count);
        for (int i = 0; i < count; i++) {
            byte[] name = names[i].getBytes(StandardCharsets.UTF_8);
            payload.putShort((short) name.length).put(name).putLong(sizes[i]);
        }
        return frame(MSG_FILE_LIST, payload.array());
    }

    /** Decoded MSG_FILE_CONTENT_REQUEST payload. */
    static final class FileContentRequest {
        final int requestId;
        final int index;
        final long offset;
        final int length;

        FileContentRequest(int requestId, int index, long offset, int length) {
            this.requestId = requestId;
            this.index = index;
            this.offset = offset;
            this.length = length;
        }
    }

    static FileContentRequest parseFileContentRequest(byte[] payload) throws IOException {
        if (payload.length != 20) throw new IOException("Invalid file content request");
        ByteBuffer b = littleEndian(payload);
        return new FileContentRequest(b.getInt(), b.getInt(), b.getLong(), b.getInt());
    }

    static byte[] fileContentResponse(int requestId, byte[] data) {
        if (data == null) data = new byte[0];
        ByteBuffer payload = ByteBuffer.allocate(4 + data.length).order(ByteOrder.LITTLE_ENDIAN);
        payload.putInt(requestId).put(data);
        return frame(MSG_FILE_CONTENT_RESPONSE, payload.array());
    }

    static byte[] videoFrame(int encodedWidth, int encodedHeight,
                             int displayWidth, int displayHeight,
                             long timestampMs, boolean keyFrame, byte[] annexB) {
        ByteBuffer payload = ByteBuffer.allocate(13 + annexB.length)
                .order(ByteOrder.LITTLE_ENDIAN);
        payload.putShort((short) encodedWidth);
        payload.putShort((short) encodedHeight);
        payload.putShort((short) displayWidth);
        payload.putShort((short) displayHeight);
        payload.putInt((int) timestampMs);
        payload.put((byte) (keyFrame ? 1 : 0));
        payload.put(annexB);
        return frame(MSG_VIDEO_FRAME, payload.array());
    }

    static ByteBuffer littleEndian(byte[] payload) {
        return ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
    }

    static byte[] decodeToken(String token) throws IOException {
        if (token == null || token.length() != 32) throw new IOException("Invalid bridge token");
        byte[] decoded = new byte[16];
        for (int i = 0; i < decoded.length; i++) {
            char highChar = token.charAt(i * 2);
            char lowChar = token.charAt(i * 2 + 1);
            if (!isLowerHex(highChar) || !isLowerHex(lowChar)) {
                throw new IOException("Invalid bridge token");
            }
            int high = Character.digit(highChar, 16);
            int low = Character.digit(lowChar, 16);
            decoded[i] = (byte) ((high << 4) | low);
        }
        return decoded;
    }

    private static boolean isLowerHex(char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    }

    private static byte[] deriveSessionKey(byte[] tokenBytes) throws IOException {
        return hmac(tokenBytes, KDF_CONTEXT);
    }

    private static byte[] transcript(byte[] androidNonce, byte[] rustNonce) {
        ByteBuffer transcript = ByteBuffer.allocate(MAGIC.length + 1 + 2 * NONCE_SIZE);
        transcript.put(MAGIC).put((byte) VERSION).put(androidNonce).put(rustNonce);
        return transcript.array();
    }

    private static byte[] hmac(byte[] key, byte[]... chunks) throws IOException {
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(key, "HmacSHA256"));
            for (byte[] chunk : chunks) mac.update(chunk);
            return mac.doFinal();
        } catch (GeneralSecurityException e) {
            throw new IOException("HMAC-SHA256 is unavailable", e);
        }
    }

    private static void requireLength(byte[] value, int expected, String name) {
        if (value == null || value.length != expected) {
            throw new IllegalArgumentException(name + " must be " + expected + " bytes");
        }
    }

    private static byte[] ascii(String value) {
        return value.getBytes(StandardCharsets.US_ASCII);
    }
}
