package com.anland.consumer;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

final class BridgeProtocol {
    static final int MSG_HELLO = 0;
    static final int MSG_KEY = 1;
    static final int MSG_MOUSE_MOTION = 2;
    static final int MSG_MOUSE_BUTTON = 3;
    static final int MSG_MOUSE_AXIS = 4;
    static final int MSG_CLIPBOARD = 5;
    static final int MSG_VIDEO_FRAME = 16;
    static final int MSG_IDR_REQUEST = 17;
    static final int MSG_STREAM_START = 18;
    static final int MSG_STREAM_STOP = 19;

    private static final int MAX_FRAME_SIZE = 16 * 1024 * 1024;

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

    static byte[] frame(int type, byte[] payload) {
        ByteBuffer buffer = ByteBuffer.allocate(5 + payload.length);
        buffer.putInt(1 + payload.length);
        buffer.put((byte) type);
        buffer.put(payload);
        return buffer.array();
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
}
