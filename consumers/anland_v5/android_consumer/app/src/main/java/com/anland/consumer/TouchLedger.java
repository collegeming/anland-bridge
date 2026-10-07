package com.anland.consumer;

import java.util.LinkedHashMap;
import java.util.Map;

/** Contacts actually forwarded to the desktop, including their last coordinates. */
final class TouchLedger {
    interface Sender {
        void touch(int action, int id, float x, float y);
        void frame();
    }

    private final Sender sender;
    private final Map<Integer, float[]> held = new LinkedHashMap<>();
    private boolean pending;

    TouchLedger(Sender sender) {
        this.sender = sender;
    }

    void send(int action, int id, float x, float y) {
        float[] previous = held.get(id);
        if (action == 0) {
            if (previous != null) {
                sender.touch(1, id, previous[0], previous[1]);
                pending = true;
                frame();
            }
            held.put(id, new float[] {x, y});
        } else {
            // A late Android MOVE/UP after a grab or focus change has no DOWN
            // in this stream and must not recreate or release another contact.
            if (previous == null)
                return;
            if (action == 1)
                held.remove(id);
            else {
                previous[0] = x;
                previous[1] = y;
            }
        }
        sender.touch(action, id, x, y);
        pending = true;
    }

    void frame() {
        if (pending) {
            sender.frame();
            pending = false;
        }
    }

    void releaseAll() {
        for (Map.Entry<Integer, float[]> entry : held.entrySet()) {
            float[] position = entry.getValue();
            sender.touch(1, entry.getKey(), position[0], position[1]);
            pending = true;
        }
        held.clear();
        frame();
    }
}
