package com.anland.consumer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

import java.security.SecureRandom;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.junit.Test;

public class GoldControllerTest {
    private static final String CONTROLLER =
            "/data/adb/modules/Gold_Keyboardremaps/bin/keyboardctl";
    private static final Pattern ABSENT = Pattern.compile("GOLDCTL_ABSENT_[A-Za-z0-9_-]+");

    @Test
    public void commandProvesAbsenceFromTheShellItself() {
        String command = controller().buildCommand("input-status", "GOLDCTL_ABSENT_probe_token");

        // Absence is decided by the directory test, not inferred from empty output.
        assertTrue(command.contains("[ ! -d '/data/adb/modules/Gold_Keyboardremaps' ]"));
        assertTrue(command.contains("printf '%s\\n' 'GOLDCTL_ABSENT_probe_token'"));
        // The controller is run through sh: bin/ can lose its exec bit on upgrade.
        assertTrue(command.contains("exec /system/bin/sh "));
        assertTrue(command.endsWith("input-status"));
    }

    @Test
    public void everyCallGetsItsOwnMarker() {
        GoldController controller = controller();
        String first = controller.buildCommand("input-status", "GOLDCTL_ABSENT_a");
        String second = controller.buildCommand("input-status", "GOLDCTL_ABSENT_b");

        assertNotEquals(markerOf(first), markerOf(second));

        GoldController.GuardedResult guarded = controller.runGuarded("input-status");
        assertTrue(markerOf(guarded.marker).equals(guarded.marker));
        assertTrue(guarded.result.unavailable);
    }

    @Test
    public void moduleDirectoryIsTheControllerGrandparent() {
        assertEquals("/data/adb/modules/Gold_Keyboardremaps", controller().moduleDirectory());
    }

    @Test
    public void shellQuoteSurvivesAQuoteInThePath() {
        assertEquals("'a'\\''b'", SuCommand.shellQuote("a'b"));
        assertEquals("'/plain/path'", SuCommand.shellQuote("/plain/path"));
    }

    private static GoldController controller() {
        return new GoldController((command, timeout) ->
                new SuCommand.CommandResult(-1, "", "", false, false, true),
                CONTROLLER, new SecureRandom());
    }

    private static String markerOf(String text) {
        Matcher matcher = ABSENT.matcher(text);
        assertTrue(matcher.find());
        return matcher.group();
    }
}
