package com.anland.consumer;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class ImmersiveInputControllerTest {

    @Test
    public void absentGoldDoesNotNeedAnOccupancyWarning() {
        assertFalse(ImmersiveInputController.shouldWarnGoldUnknown(
                GoldInputStatusClient.Result.absent()));
    }

    @Test
    public void missingOrUnreadableGoldStatusNeedsAnOccupancyWarning() {
        assertTrue(ImmersiveInputController.shouldWarnGoldUnknown(null));
        assertTrue(ImmersiveInputController.shouldWarnGoldUnknown(
                GoldInputStatusClient.Result.unknown()));
    }
}
