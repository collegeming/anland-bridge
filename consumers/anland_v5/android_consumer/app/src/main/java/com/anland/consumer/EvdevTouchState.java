package com.anland.consumer;

import java.util.Arrays;

/** Decodes single-touch and both Linux multitouch protocols, without Android APIs. */
final class EvdevTouchState {
    static final int MAX_CONTACTS = 16;
    private static final int ABS_X = 0x00, ABS_Y = 0x01, ABS_MT_SLOT = 0x2f;
    private static final int ABS_MT_POSITION_X = 0x35, ABS_MT_POSITION_Y = 0x36;
    private static final int ABS_MT_TOOL_TYPE = 0x37, ABS_MT_TRACKING_ID = 0x39;
    private static final int BTN_TOUCH = 0x14a, BTN_TOOL_PEN = 0x140, BTN_TOOL_RUBBER = 0x141;

    final float[] x = new float[MAX_CONTACTS];
    final float[] y = new float[MAX_CONTACTS];
    // A slot can be reused between two SYN_REPORTs. Its generation distinguishes
    // the new finger from the one the consumer still needs to release.
    final long[] generation = new long[MAX_CONTACTS];
    private final int[] trackingId = new int[MAX_CONTACTS];
    private final int[] toolType = new int[MAX_CONTACTS];
    private final boolean[] identified = new boolean[MAX_CONTACTS];
    private int slot;
    private boolean sawSlots, sawTrackingId, protocolA;
    private boolean touchKnown, touching, pen, rubber, validX, validY;
    private float singleX, singleY;

    // Protocol A reports a complete contact list each frame, separated by
    // SYN_MT_REPORT. Unlike protocol B, it need not send tracking_id = -1.
    private final int[] packetId = new int[MAX_CONTACTS];
    private final int[] packetTool = new int[MAX_CONTACTS];
    private final boolean[] packetIdentified = new boolean[MAX_CONTACTS];
    private final float[] packetX = new float[MAX_CONTACTS];
    private final float[] packetY = new float[MAX_CONTACTS];
    private int packetCount;
    private int currentId = -1, currentTool;
    private boolean currentIdentified, currentHasX, currentHasY;
    private float currentX, currentY;
    private final int[] assigned = new int[MAX_CONTACTS];
    private final boolean[] used = new boolean[MAX_CONTACTS];

    EvdevTouchState() {
        Arrays.fill(trackingId, -1);
    }

    boolean key(int code, boolean pressed) {
        switch (code) {
            case BTN_TOUCH:
                if (pressed && !touching && !sawTrackingId && !protocolA)
                    generation[0]++;
                touchKnown = true;
                touching = pressed;
                break;
            case BTN_TOOL_PEN: pen = pressed; break;
            case BTN_TOOL_RUBBER: rubber = pressed; break;
            default: return false;
        }
        return true;
    }

    boolean abs(int code, int value) {
        switch (code) {
            case ABS_X: singleX = value; validX = true; return true;
            case ABS_Y: singleY = value; validY = true; return true;
            case ABS_MT_SLOT:
                sawSlots = true;
                slot = value >= 0 && value < MAX_CONTACTS ? value : -1;
                clearPacket();
                return true;
            case ABS_MT_TRACKING_ID:
            case ABS_MT_POSITION_X:
            case ABS_MT_POSITION_Y:
            case ABS_MT_TOOL_TYPE:
                break;
            default:
                return false;
        }

        // Until the first delimiter arrives, keep both interpretations. A
        // protocol B driver need not repeat ABS_MT_SLOT when it stays in slot 0.
        if (!sawSlots) {
            switch (code) {
                case ABS_MT_TRACKING_ID: currentId = value; currentIdentified = true; break;
                case ABS_MT_POSITION_X: currentX = value; currentHasX = true; break;
                case ABS_MT_POSITION_Y: currentY = value; currentHasY = true; break;
                case ABS_MT_TOOL_TYPE: currentTool = value; break;
            }
        }
        if (protocolA || slot < 0)
            return true;
        switch (code) {
            case ABS_MT_TRACKING_ID:
                sawTrackingId = true;
                if (value >= 0 && trackingId[slot] != value)
                    generation[slot]++;
                trackingId[slot] = value;
                identified[slot] = true;
                break;
            case ABS_MT_POSITION_X: x[slot] = value; break;
            case ABS_MT_POSITION_Y: y[slot] = value; break;
            case ABS_MT_TOOL_TYPE: toolType[slot] = value; break;
        }
        return true;
    }

    /** SYN_MT_REPORT terminates one protocol A contact, including an empty one. */
    void endContact() {
        if (sawSlots)
            return;
        protocolA = true;
        appendPacket();
    }

    boolean isProtocolA() {
        return protocolA;
    }

    /** Commit only at SYN_REPORT, after all contacts and BTN_TOUCH are known. */
    void endFrame() {
        if (protocolA) {
            appendPacket();
            if (touchKnown && !touching)
                packetCount = 0;
            commitPackets();
        } else if (touchKnown && !touching) {
            // BTN_TOUCH=0 means every contact lifted. Do not retain an old MT
            // id just because this driver omitted its individual release.
            Arrays.fill(trackingId, -1);
        }
        packetCount = 0;
        clearPacket();
    }

    int gather(int[] out) {
        int count = 0;
        if (protocolA || sawTrackingId) {
            for (int i = 0; i < MAX_CONTACTS; i++) {
                if (trackingId[i] >= 0 && toolType[i] == 0)
                    out[count++] = i;
            }
        } else if (touching && validX && validY && !pen && !rubber) {
            x[0] = singleX;
            y[0] = singleY;
            out[count++] = 0;
        }
        return count;
    }

    /** A lost frame cancels contacts; stale tracking ids must not resurrect them. */
    void clear() {
        Arrays.fill(trackingId, -1);
        Arrays.fill(toolType, 0);
        touchKnown = touching = pen = rubber = validX = validY = false;
        packetCount = 0;
        clearPacket();
    }

    private void appendPacket() {
        if (currentHasX && currentHasY && (!currentIdentified || currentId >= 0)
                && packetCount < MAX_CONTACTS) {
            int p = packetCount++;
            packetId[p] = currentId;
            packetIdentified[p] = currentIdentified;
            packetTool[p] = currentTool;
            packetX[p] = currentX;
            packetY[p] = currentY;
        }
        clearPacket();
    }

    private void clearPacket() {
        currentId = -1;
        currentTool = 0;
        currentIdentified = currentHasX = currentHasY = false;
    }

    private void commitPackets() {
        Arrays.fill(assigned, -1);
        Arrays.fill(used, false);
        // Tracking ids identify fingers, not packet order or array indices.
        for (int p = 0; p < packetCount; p++) {
            if (!packetIdentified[p])
                continue;
            for (int s = 0; s < MAX_CONTACTS; s++) {
                if (!used[s] && identified[s] && trackingId[s] == packetId[p]) {
                    assigned[p] = s;
                    used[s] = true;
                    break;
                }
            }
        }

        // Tracking ids are optional in protocol A. Match those contacts by
        // proximity so lifting one finger does not renumber the remaining one.
        for (;;) {
            double best = Double.POSITIVE_INFINITY;
            int bestPacket = -1, bestSlot = -1;
            for (int p = 0; p < packetCount; p++) {
                if (assigned[p] >= 0 || packetIdentified[p])
                    continue;
                for (int s = 0; s < MAX_CONTACTS; s++) {
                    if (used[s] || identified[s] || trackingId[s] < 0
                            || packetTool[p] != toolType[s])
                        continue;
                    double dx = packetX[p] - x[s], dy = packetY[p] - y[s];
                    double distance = dx * dx + dy * dy;
                    if (distance < best) {
                        best = distance;
                        bestPacket = p;
                        bestSlot = s;
                    }
                }
            }
            if (bestSlot < 0)
                break;
            assigned[bestPacket] = bestSlot;
            used[bestSlot] = true;
        }

        for (int s = 0; s < MAX_CONTACTS; s++) {
            if (!used[s])
                trackingId[s] = -1;
        }
        for (int p = 0; p < packetCount; p++) {
            int s = assigned[p];
            if (s < 0) {
                for (s = 0; s < MAX_CONTACTS && used[s]; s++) { }
                used[s] = true;
                generation[s]++;
            }
            trackingId[s] = packetIdentified[p] ? packetId[p] : 0;
            identified[s] = packetIdentified[p];
            toolType[s] = packetTool[p];
            x[s] = packetX[p];
            y[s] = packetY[p];
        }
    }
}
