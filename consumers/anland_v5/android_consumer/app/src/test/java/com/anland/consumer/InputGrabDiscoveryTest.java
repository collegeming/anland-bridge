package com.anland.consumer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

import org.junit.Test;

public class InputGrabDiscoveryTest {
    // node, name, class, bus, flags — the shape the bundled helper prints.
    private static String line(String node, String name, int cls, int bus, String flags) {
        return node + "\t" + name + "\t" + cls + "\t"
                + String.format("%04x", bus) + "\t" + flags + "\n";
    }

    @Test
    public void parsesNodeNameClassBusAndFlags() {
        List<InputGrab.DiscoveredDevice> devices = InputGrab.parseDeviceList(
                line("event6", "OnePlus Pad 2 Pro Keyboard", InputGrab.CLASS_KEYBOARD,
                        InputGrab.BUS_BLUETOOTH, "a")
                        + line("event9", "Atmel maXTouch", InputGrab.CLASS_TOUCHSCREEN,
                                InputGrab.BUS_I2C, "-"));

        assertEquals(2, devices.size());
        assertEquals(new InputGrab.DiscoveredDevice("event6", "OnePlus Pad 2 Pro Keyboard",
                InputGrab.CLASS_KEYBOARD, InputGrab.BUS_BLUETOOTH, true, false),
                devices.get(0));
        assertEquals(InputGrab.BUS_I2C, devices.get(1).bus);
        assertEquals(InputGrab.CLASS_TOUCHSCREEN, devices.get(1).cls);
    }

    @Test
    public void busIsParsedAsHexNotDecimal() {
        // 0019 is BUS_HOST; read as decimal it would be 19 and match nothing.
        List<InputGrab.DiscoveredDevice> devices =
                InputGrab.parseDeviceList(line("event1", "kbd", InputGrab.CLASS_KEYBOARD,
                        InputGrab.BUS_HOST, "a"));

        assertEquals(InputGrab.BUS_HOST, devices.get(0).bus);
        assertEquals(0x19, devices.get(0).bus);
    }

    @Test
    public void watchOnlyFlagIsCarriedThrough() {
        List<InputGrab.DiscoveredDevice> devices =
                InputGrab.parseDeviceList(line("event1", "pmic_pwrkey",
                        InputGrab.CLASS_KEYBOARD, InputGrab.BUS_HOST, "w"));

        assertEquals(1, devices.size());
        assertTrue(devices.get(0).watchOnly);
    }

    /**
     * The list has to come out the way a person reads it. The helper reports
     * nodes in readdir order, which on a real device is neither sorted nor
     * stable, and Gold's own virtual device keeps shifting the numbering.
     */
    @Test
    public void nodesAreSortedByNumberNotByReadOrder() {
        String stdout = line("event11", "eleven", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a")
                + line("event15", "fifteen", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a")
                + line("event8", "eight", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a")
                + line("event0", "zero", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a")
                + line("event2", "two", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a");

        assertEquals(Arrays.asList("event0", "event2", "event8", "event11", "event15"),
                nodes(InputGrab.parseDeviceList(stdout)));
    }

    @Test
    public void sortingIsNumericNotLexical() {
        // Lexical order would put event10 before event2 and event9 last.
        List<InputGrab.DiscoveredDevice> devices = InputGrab.parseDeviceList(
                line("event10", "ten", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a")
                        + line("event9", "nine", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a")
                        + line("event2", "two", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a"));

        assertEquals(Arrays.asList("event2", "event9", "event10"), nodes(devices));
    }

    // ---- what may be offered ----------------------------------------------

    @Test
    public void powerAndVolumeButtonNodesAreNotOffered() {
        // These are the nodes the user must never be able to hand to the desktop:
        // Android's own wake and volume paths run through them.
        assertFalse(InputGrab.isSelectable(buttonNode("event0", "gpio-keys")));
        assertFalse(InputGrab.isSelectable(buttonNode("event1", "pmic_pwrkey")));
        assertFalse(InputGrab.isSelectable(buttonNode("event2", "pmic_resin")));
        assertFalse(InputGrab.isSelectable(buttonNode("event3", "pogo_wakeup")));
        assertFalse(InputGrab.isVisible(buttonNode("event0", "gpio-keys")));
    }

    @Test
    public void protectedTouchscreenIsVisibleWithItsRealClass() {
        List<InputGrab.DiscoveredDevice> devices = InputGrab.parseDeviceList(
                line("event4", "touchpanel", InputGrab.CLASS_TOUCHSCREEN,
                        InputGrab.BUS_I2C, "w"));
        assertEquals(1, devices.size());
        InputGrab.DiscoveredDevice panel = devices.get(0);
        assertEquals(InputGrab.CLASS_TOUCHSCREEN, panel.cls);
        assertTrue(InputGrab.isVisible(panel));
        assertFalse(InputGrab.isSelectable(panel));
    }

    @Test
    public void gesturePanelWithIndependentPowerButtonIsSelectable() {
        InputGrab.DiscoveredDevice panel = InputGrab.parseDeviceList(
                line("event4", "touchpanel", InputGrab.CLASS_TOUCHSCREEN,
                        InputGrab.BUS_I2C, "-")).get(0);
        assertTrue(InputGrab.isVisible(panel));
        assertTrue(InputGrab.isSelectable(panel));
    }

    @Test
    public void aNodeWithKeysButNoLettersIsNotOffered() {
        // Classified as a keyboard, but it is a button cluster: Consumer Control,
        // a headset button jack, a stylus button.
        InputGrab.DiscoveredDevice consumerControl = new InputGrab.DiscoveredDevice(
                "event13", "OnePlus Pad 2 Pro Keyboard Consumer Control",
                InputGrab.CLASS_KEYBOARD, InputGrab.BUS_HOST, false, false);

        assertFalse(InputGrab.isSelectable(consumerControl));
    }

    @Test
    public void realKeyboardsPointersAndTouchAreOffered() {
        assertTrue(InputGrab.isSelectable(new InputGrab.DiscoveredDevice(
                "event12", "OnePlus Pad 2 Pro Keyboard", InputGrab.CLASS_KEYBOARD,
                InputGrab.BUS_BLUETOOTH, true, false)));
        assertTrue(InputGrab.isSelectable(new InputGrab.DiscoveredDevice(
                "event4", "touchpanel", InputGrab.CLASS_TOUCHSCREEN,
                InputGrab.BUS_I2C, false, false)));
        assertTrue(InputGrab.isSelectable(new InputGrab.DiscoveredDevice(
                "event10", "pogo_touchpad", InputGrab.CLASS_TOUCHPAD,
                InputGrab.BUS_HOST, false, false)));
        assertTrue(InputGrab.isSelectable(new InputGrab.DiscoveredDevice(
                "event5", "Some Mouse", InputGrab.CLASS_MOUSE,
                InputGrab.BUS_USB, false, false)));
    }

    // ---- malformed input ---------------------------------------------------

    @Test
    public void keepsNamesThatContainSpaces() {
        List<InputGrab.DiscoveredDevice> devices = InputGrab.parseDeviceList(
                line("event1", "My  Keyboard  With Spaces", InputGrab.CLASS_KEYBOARD,
                        InputGrab.BUS_USB, "a"));

        assertEquals(1, devices.size());
        assertEquals("My  Keyboard  With Spaces", devices.get(0).name);
    }

    @Test
    public void missingNameIsAllowed() {
        List<InputGrab.DiscoveredDevice> devices = InputGrab.parseDeviceList(
                line("event2", "", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "-"));

        assertEquals(1, devices.size());
        assertEquals("", devices.get(0).name);
    }

    @Test
    public void dropsLinesItCannotTrust() {
        String stdout = line("event3", "ok", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a")
                + "event\tno number\t4\t0003\ta\n"
                + line("event01", "leading zero is fine here", InputGrab.CLASS_KEYBOARD,
                        InputGrab.BUS_USB, "a")
                + "gpio-keys\tnot an event node\t4\t0019\tw\n"
                + "event5\tnan class\tkeyboard\t0003\t-\n"
                + "event6\tnan bus\t4\tzzzz\t-\n"
                + "event7\tout of range\t0\t0003\t-\n"
                + "event8\tout of range\t5\t0003\t-\n"
                + "event9\ttoo few fields\t4\t0003\n"
                + "event10\ttoo many fields\t4\t0003\ta\textra\n"
                + "\n"
                + "   \n";

        // event01 survives: discovery feeds the saved-preference grammar, which
        // is the permissive one. Everything genuinely malformed is dropped.
        assertEquals(Arrays.asList("event01", "event3"),
                nodes(InputGrab.parseDeviceList(stdout)));
    }

    @Test
    public void nullAndEmptyInputProduceNoDevices() {
        assertTrue(InputGrab.parseDeviceList(null).isEmpty());
        assertTrue(InputGrab.parseDeviceList("").isEmpty());
        assertTrue(InputGrab.parseDeviceList("\n\n").isEmpty());
    }

    @Test
    public void chatterOnStdoutIsIgnored() {
        List<InputGrab.DiscoveredDevice> devices = InputGrab.parseDeviceList(
                "WARNING: linker: unused\n"
                        + line("event4", "Real", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a")
                        + "trailing noise\n");

        assertEquals(Collections.singletonList("event4"), nodes(devices));
    }

    @Test
    public void duplicateNodesAreNotMergedHere() {
        // De-duplication belongs to the selection resolver, which validates the
        // saved list; discovery reports what the kernel actually has.
        List<InputGrab.DiscoveredDevice> devices = InputGrab.parseDeviceList(
                line("event4", "A", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a")
                        + line("event4", "A", InputGrab.CLASS_KEYBOARD, InputGrab.BUS_USB, "a"));

        assertEquals(Arrays.asList("event4", "event4"), nodes(devices));
    }

    private static InputGrab.DiscoveredDevice buttonNode(String node, String name) {
        return new InputGrab.DiscoveredDevice(node, name, InputGrab.CLASS_KEYBOARD,
                InputGrab.BUS_HOST, false, true);
    }

    private static List<String> nodes(List<InputGrab.DiscoveredDevice> devices) {
        List<String> nodes = new ArrayList<>(devices.size());
        for (InputGrab.DiscoveredDevice device : devices)
            nodes.add(device.node);
        return nodes;
    }
}
