package com.anland.consumer;

import static org.junit.Assert.*;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.junit.Test;

public class TouchLedgerTest {
    private final List<String> events = new ArrayList<>();
    private final TouchLedger ledger = new TouchLedger(new TouchLedger.Sender() {
        @Override public void touch(int action, int id, float x, float y) {
            events.add(action + ":" + id + ":" + x + ":" + y);
        }
        @Override public void frame() { events.add("frame"); }
    });

    @Test
    public void ownershipChangeReleasesEveryFingerAtItsLastForwardedPosition() {
        ledger.send(0, 3, 100, 200);
        ledger.send(0, 8, 300, 400);
        ledger.frame();
        ledger.send(2, 3, 110, 220);
        ledger.frame();
        events.clear();

        ledger.releaseAll();
        assertEquals(Arrays.asList("1:3:110.0:220.0", "1:8:300.0:400.0", "frame"), events);
        events.clear();
        ledger.releaseAll();
        ledger.send(2, 3, 120, 240);
        ledger.send(1, 8, 300, 400);
        ledger.frame();
        assertTrue(events.isEmpty());
    }

    @Test
    public void liftingOneFingerKeepsTheOtherAvailableForCleanup() {
        ledger.send(0, 0, 100, 200);
        ledger.send(0, 1, 300, 400);
        ledger.frame();
        ledger.send(1, 0, 110, 210);
        ledger.frame();
        events.clear();
        ledger.releaseAll();
        assertEquals(Arrays.asList("1:1:300.0:400.0", "frame"), events);
    }

    @Test
    public void reusedPointerIdEndsOldContactBeforeNewDown() {
        ledger.send(0, 0, 438, 349);
        ledger.frame();
        events.clear();
        ledger.send(0, 0, 100, 150);
        ledger.frame();
        assertEquals(Arrays.asList("1:0:438.0:349.0", "frame", "0:0:100.0:150.0", "frame"), events);
    }
}
