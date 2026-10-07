package com.anland.consumer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Set;

import org.junit.Test;

public class ImmersiveInputSelectionTest {

    // ---- source preference ------------------------------------------------

    @Test
    public void missingOrUnknownSourceFallsBackToDirect() {
        assertEquals(ImmersiveInputSource.DIRECT_EVENT_NODES,
                ImmersiveInputSource.fromPreference(null));
        assertEquals(ImmersiveInputSource.DIRECT_EVENT_NODES,
                ImmersiveInputSource.fromPreference(""));
        assertEquals(ImmersiveInputSource.DIRECT_EVENT_NODES,
                ImmersiveInputSource.fromPreference("future-source"));
        assertEquals(ImmersiveInputSource.EXISTING_UINPUT_BUS,
                ImmersiveInputSource.fromPreference("existing_uinput_bus"));
        assertEquals(ImmersiveInputSource.DIRECT_EVENT_NODES,
                ImmersiveInputSource.fromPreference("direct_event_nodes"));
    }

    // ---- selection --------------------------------------------------------

    @Test
    public void theCombinedSourceAsksForBothHalves() {
        ImmersiveInputSource combined = ImmersiveInputSource.DIRECT_PLUS_GOLD_KEYBOARD;

        assertTrue(combined.takesDevices());
        assertTrue(combined.listensToGoldKeyboard());
    }

    @Test
    public void eachHalfOfTheCombinedSourceIsStillItsOwnSource() {
        // Direct takes devices and leaves the keyboard to ordinary forwarding;
        // Gold-only takes the virtual output and leaves physical nodes alone.
        assertTrue(ImmersiveInputSource.DIRECT_EVENT_NODES.takesDevices());
        assertFalse(ImmersiveInputSource.DIRECT_EVENT_NODES.listensToGoldKeyboard());

        assertFalse(ImmersiveInputSource.EXISTING_UINPUT_BUS.takesDevices());
        assertTrue(ImmersiveInputSource.EXISTING_UINPUT_BUS.listensToGoldKeyboard());
    }

    @Test
    public void aConfigWrittenBeforeTheCombinedSourceExistedStillMeansDirect() {
        // The new value is additive: nothing already stored can resolve to it.
        assertEquals(ImmersiveInputSource.DIRECT_EVENT_NODES,
                ImmersiveInputSource.fromPreference(""));
        assertEquals(ImmersiveInputSource.DIRECT_EVENT_NODES,
                ImmersiveInputSource.fromPreference("direct"));
        assertEquals(ImmersiveInputSource.EXISTING_UINPUT_BUS,
                ImmersiveInputSource.fromPreference("existing_uinput_bus"));
        assertEquals(ImmersiveInputSource.DIRECT_PLUS_GOLD_KEYBOARD,
                ImmersiveInputSource.fromPreference("direct_plus_gold_keyboard"));
    }

    @Test
    public void automaticStaysAutomaticAndOnlyExcludesStrictGoldNodes() {
        ImmersiveDirectSelection.Result result = ImmersiveDirectSelection.resolve(true,
                Collections.<String>emptySet(), linkedSet("event3", "event12", "event01"), 32);

        assertNull(result.selectedNodes);
        // event01 is not a name Gold can produce, so it is not an exclusion.
        assertEquals(linkedSet("event12", "event3"), result.excludedNodes);
        assertFalse(result.invalid);
        assertFalse(result.allFiltered);
    }

    @Test
    public void manualKeepsLegacyNamesAndFiltersGoldOccupancy() {
        Set<String> saved = linkedSet("event01", "event3", "event12");

        ImmersiveDirectSelection.Result result =
                ImmersiveDirectSelection.resolve(false, saved, linkedSet("event3"), 32);

        assertEquals(linkedSet("event01", "event12"), result.selectedNodes);
        assertEquals(linkedSet("event3"), result.excludedNodes);
        assertFalse(result.invalid);
        assertFalse(result.allFiltered);
        // The user's saved intent is not rewritten by resolving a session.
        assertEquals(linkedSet("event01", "event3", "event12"), saved);
    }

    @Test
    public void fullyFilteredManualSelectionDoesNotBecomeAutomatic() {
        ImmersiveDirectSelection.Result result =
                ImmersiveDirectSelection.resolve(false, linkedSet("event3"), linkedSet("event3"), 32);

        assertEquals(Collections.<String>emptySet(), result.selectedNodes);
        assertTrue(result.allFiltered);
        // Not invalid: the saved list was fine, Gold simply holds all of it.
        assertFalse(result.invalid);
    }

    @Test
    public void unusableSavedListsAreInvalidRatherThanSilentlyEmpty() {
        assertTrue(ImmersiveDirectSelection.resolve(false, null, null, 32).invalid);
        assertTrue(ImmersiveDirectSelection.resolve(false,
                Collections.<String>emptySet(), null, 32).invalid);
        assertTrue(ImmersiveDirectSelection.resolve(false, linkedSet("eventX"), null, 32).invalid);
        assertTrue(ImmersiveDirectSelection.resolve(false, linkedSet("event3", ""), null, 32).invalid);
        assertTrue(ImmersiveDirectSelection.resolve(false,
                linkedSet("event1", "event2", "event3"), null, 2).invalid);
    }

    @Test
    public void unverifiableOccupancyContributesNoExclusions() {
        ImmersiveDirectSelection.Result result =
                ImmersiveDirectSelection.resolve(false, linkedSet("event3"), null, 32);

        assertEquals(linkedSet("event3"), result.selectedNodes);
        assertEquals(Collections.<String>emptySet(), result.excludedNodes);
        assertFalse(result.allFiltered);
        assertFalse(result.invalid);
    }

    @Test
    public void eachSourceSelectsItsNativeCaptureScope() {
        assertEquals("source=physical", ImmersiveInputSource.DIRECT_EVENT_NODES.helperArgument());
        assertEquals("source=gold", ImmersiveInputSource.EXISTING_UINPUT_BUS.helperArgument());
        assertEquals("source=combined", ImmersiveInputSource.DIRECT_PLUS_GOLD_KEYBOARD.helperArgument());
        assertTrue(ImmersiveInputSource.EXISTING_UINPUT_BUS.allowsUnboundToggle());
        assertFalse(ImmersiveInputSource.DIRECT_EVENT_NODES.allowsUnboundToggle());
        assertFalse(ImmersiveInputSource.DIRECT_PLUS_GOLD_KEYBOARD.allowsUnboundToggle());
    }

    // ---- wire arguments ---------------------------------------------------

    @Test
    public void exclusionSerializationIsCanonicalAndStrict() {
        assertEquals("exclude=event12,event3",
                InputGrab.serializeExcludedNodes(Arrays.asList("event3", "event12")));
        assertEquals("", InputGrab.serializeExcludedNodes(Collections.<String>emptySet()));
        assertEquals("", InputGrab.serializeExcludedNodes(null));

        // Gold's strict grammar: a leading zero is not a node, and a repeated
        // node means we do not understand the list well enough to act on it.
        assertNull(InputGrab.serializeExcludedNodes(Arrays.asList("event01")));
        assertNull(InputGrab.serializeExcludedNodes(Arrays.asList("event3", "event3")));
        assertNull(InputGrab.serializeExcludedNodes(Arrays.asList("event3", "nonsense")));
    }

    @Test
    public void selectedSerializationKeepsLegacyNames() {
        assertEquals("nodes=event01,event3",
                InputGrab.serializeSelectedNodes(Arrays.asList("event3", "event01")));
        assertEquals("", InputGrab.serializeSelectedNodes(Collections.<String>emptySet()));
        // event01 is exactly the value an older build could have saved.
        assertEquals("nodes=event01",
                InputGrab.serializeSelectedNodes(Collections.singletonList("event01")));
        assertNull(InputGrab.serializeSelectedNodes(Arrays.asList("event1", "x")));
    }

    private static Set<String> linkedSet(String... values) {
        return new LinkedHashSet<>(Arrays.asList(values));
    }
}
