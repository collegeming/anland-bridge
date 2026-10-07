package com.anland.consumer;

import static org.junit.Assert.*;

import java.util.Arrays;
import org.junit.Test;

public class EvdevTouchStateTest {
    private static final int ABS_X = 0, ABS_Y = 1, SLOT = 0x2f;
    private static final int X = 0x35, Y = 0x36, TOOL = 0x37, ID = 0x39;
    private static final int BTN_TOUCH = 0x14a;

    private static void packet(EvdevTouchState state, Integer id, int x, int y) {
        if (id != null)
            state.abs(ID, id);
        state.abs(X, x);
        state.abs(Y, y);
        state.endContact();
    }

    private static int[] contacts(EvdevTouchState state) {
        int[] slots = new int[EvdevTouchState.MAX_CONTACTS];
        return Arrays.copyOf(slots, state.gather(slots));
    }

    @Test
    public void protocolADragEndsWithoutNegativeTrackingIdAndNextTapStartsFresh() {
        EvdevTouchState state = new EvdevTouchState();
        state.key(BTN_TOUCH, true);
        packet(state, 0, 438, 349);
        state.endFrame();
        assertArrayEquals(new int[] {0}, contacts(state));
        long firstFinger = state.generation[0];

        packet(state, 0, 800, 600);
        state.endFrame();
        assertEquals(firstFinger, state.generation[0]);
        assertEquals(800f, state.x[0], 0f);

        // Protocol A ends with BTN_TOUCH=0 and an empty SYN_MT_REPORT;
        // there is no ABS_MT_TRACKING_ID=-1 to close the drag.
        state.key(BTN_TOUCH, false);
        state.endContact();
        state.endFrame();
        assertArrayEquals(new int[0], contacts(state));

        state.key(BTN_TOUCH, true);
        packet(state, 0, 100, 150);
        state.endFrame();
        assertArrayEquals(new int[] {0}, contacts(state));
        assertTrue(state.generation[0] > firstFinger);
        assertEquals(100f, state.x[0], 0f);
        assertEquals(150f, state.y[0], 0f);
    }

    @Test
    public void protocolAKeepsFingerIdentityWhenPacketsReorderOrOneFingerLifts() {
        EvdevTouchState state = new EvdevTouchState();
        state.key(BTN_TOUCH, true);
        packet(state, 42, 100, 200);
        packet(state, 87, 700, 800);
        state.endFrame();
        assertArrayEquals(new int[] {0, 1}, contacts(state));
        long[] generations = state.generation.clone();

        packet(state, 87, 710, 810);
        packet(state, 42, 110, 210);
        state.endFrame();
        assertEquals(110f, state.x[0], 0f);
        assertEquals(710f, state.x[1], 0f);
        assertArrayEquals(generations, state.generation);

        // BTN_TOUCH stays down; absence from the next frame lifts only id 42.
        packet(state, 87, 720, 820);
        state.endFrame();
        assertArrayEquals(new int[] {1}, contacts(state));
        assertEquals(generations[1], state.generation[1]);
    }

    @Test
    public void protocolAEmptyFrameReleasesEvenWithoutTouchButton() {
        EvdevTouchState state = new EvdevTouchState();
        packet(state, 23, 100, 200);
        state.endFrame();
        assertEquals(1, contacts(state).length);
        state.endFrame(); // A final SYN_REPORT may be the entire release frame.
        assertEquals(0, contacts(state).length);
    }

    @Test
    public void protocolAReplacementInSameSlotIsANewFinger() {
        EvdevTouchState state = new EvdevTouchState();
        packet(state, 21, 100, 200);
        state.endFrame();
        long before = state.generation[0];
        packet(state, 22, 500, 600);
        state.endFrame();
        assertArrayEquals(new int[] {0}, contacts(state));
        assertTrue(state.generation[0] > before);
    }

    @Test
    public void protocolAWithoutIdsMatchesRemainingFingerByPosition() {
        EvdevTouchState state = new EvdevTouchState();
        packet(state, null, 100, 200);
        packet(state, null, 700, 800);
        state.endFrame();
        long remaining = state.generation[1];
        packet(state, null, 710, 810);
        state.endFrame();
        assertArrayEquals(new int[] {1}, contacts(state));
        assertEquals(remaining, state.generation[1]);
        assertEquals(710f, state.x[1], 0f);
    }

    @Test
    public void protocolAPenAndPalmAreNotReplayedAsFingers() {
        EvdevTouchState state = new EvdevTouchState();
        packet(state, 0, 100, 200);
        state.abs(TOOL, 1);
        packet(state, 1, 200, 300);
        state.abs(TOOL, 2);
        packet(state, 2, 300, 400);
        state.endFrame();
        assertArrayEquals(new int[] {0}, contacts(state));
    }

    @Test
    public void protocolBFiltersToolsAndLetsPenSlotsBecomeFingers() {
        EvdevTouchState state = new EvdevTouchState();
        for (int slot = 0; slot < 3; slot++) {
            state.abs(SLOT, slot);
            state.abs(ID, 100 + slot);
            state.abs(TOOL, slot); // finger, pen, palm
            state.abs(X, 300);
            state.abs(Y, 500);
        }
        state.key(0x140, true); // A hovering pen does not hide an MT finger.
        state.endFrame();
        assertArrayEquals(new int[] {0}, contacts(state));
        state.abs(SLOT, 1);
        state.abs(ID, -1);
        state.abs(ID, 101);
        state.abs(TOOL, 0);
        state.endFrame();
        assertArrayEquals(new int[] {0, 1}, contacts(state));
    }

    @Test
    public void protocolBRetainsUnchangedSlotsAndReleasesOnlyTheLiftedFinger() {
        EvdevTouchState state = new EvdevTouchState();
        state.key(BTN_TOUCH, true);
        state.abs(SLOT, 0);
        state.abs(ID, 20);
        state.abs(X, 100);
        state.abs(Y, 200);
        state.abs(SLOT, 3);
        state.abs(ID, 30);
        state.abs(X, 700);
        state.abs(Y, 800);
        state.endFrame();
        state.abs(X, 710); // only the changed axis of slot 3 is repeated
        state.endFrame();
        state.endFrame();
        assertArrayEquals(new int[] {0, 3}, contacts(state));
        assertEquals(800f, state.y[3], 0f);

        state.abs(SLOT, 0);
        state.abs(ID, -1);
        state.endFrame();
        assertArrayEquals(new int[] {3}, contacts(state));
    }

    @Test
    public void protocolBMayOmitSlotZeroAndStillRetainItsContact() {
        EvdevTouchState state = new EvdevTouchState();
        state.abs(ID, 20);
        state.abs(X, 100);
        state.abs(Y, 200);
        state.endFrame();
        state.abs(X, 110);
        state.endFrame();
        assertFalse(state.isProtocolA());
        assertArrayEquals(new int[] {0}, contacts(state));
        state.abs(ID, -1);
        state.endFrame();
        assertEquals(0, contacts(state).length);
    }

    @Test
    public void globalTouchReleaseClearsAnMtIdWhoseIndividualUpWasMissing() {
        EvdevTouchState state = new EvdevTouchState();
        state.key(BTN_TOUCH, true);
        state.abs(SLOT, 2);
        state.abs(ID, 55);
        state.abs(X, 100);
        state.abs(Y, 200);
        state.endFrame();
        state.key(BTN_TOUCH, false);
        state.endFrame();
        assertEquals(0, contacts(state).length);
    }

    @Test
    public void protocolBSlotReuseWithinAFrameChangesContactIdentity() {
        EvdevTouchState state = new EvdevTouchState();
        state.abs(SLOT, 2);
        state.abs(ID, 55);
        state.abs(X, 100);
        state.abs(Y, 200);
        state.endFrame();
        long before = state.generation[2];
        state.abs(ID, -1);
        state.abs(ID, 56);
        state.abs(X, 500);
        state.endFrame();
        assertArrayEquals(new int[] {2}, contacts(state));
        assertTrue(state.generation[2] > before);
    }

    @Test
    public void outOfRangeSlotDoesNotOverwriteLastSupportedFinger() {
        EvdevTouchState state = new EvdevTouchState();
        state.abs(SLOT, 15);
        state.abs(ID, 20);
        state.abs(X, 100);
        state.abs(Y, 200);
        state.abs(SLOT, 16);
        state.abs(ID, -1);
        state.abs(X, 500);
        state.endFrame();
        assertArrayEquals(new int[] {15}, contacts(state));
        assertEquals(100f, state.x[15], 0f);
    }

    @Test
    public void lostFrameCannotResurrectOldContactsOrAnUnfinishedPacket() {
        for (boolean protocolA : new boolean[] {false, true}) {
            EvdevTouchState state = new EvdevTouchState();
            state.abs(ID, 20);
            state.abs(X, 100);
            state.abs(Y, 200);
            if (protocolA)
                state.endContact();
            state.endFrame();
            state.abs(ID, 21);
            state.abs(X, 110);
            state.abs(Y, 210);
            state.clear();
            state.endFrame();
            assertEquals(0, contacts(state).length);
        }
    }

    @Test
    public void singleTouchNeedsBothAxesAndReleasesOnTouchButtonUp() {
        EvdevTouchState state = new EvdevTouchState();
        state.key(BTN_TOUCH, true);
        assertEquals(0, contacts(state).length);
        state.abs(ABS_X, 100);
        state.endFrame();
        assertEquals(0, contacts(state).length);
        state.abs(ABS_Y, 200);
        state.endFrame();
        assertArrayEquals(new int[] {0}, contacts(state));
        for (int tool : new int[] {0x140, 0x141}) { // pen, eraser
            state.key(tool, true);
            state.endFrame();
            assertEquals(0, contacts(state).length);
            state.key(tool, false);
            state.endFrame();
            assertArrayEquals(new int[] {0}, contacts(state));
        }
        state.key(BTN_TOUCH, false);
        state.endFrame();
        assertEquals(0, contacts(state).length);
    }
}
