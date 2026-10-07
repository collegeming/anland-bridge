package com.anland.consumer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.security.SecureRandom;
import java.util.Arrays;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.junit.Test;

public class GoldInputStatusClientTest {
    private static final String BOOT = "boot-test-001";
    private static final String INSTANCE = "0123456789abcdef0123456789abcdef";
    private static final String CONTROLLER = "/data/adb/modules/Gold_Keyboardremaps/bin/keyboardctl";
    private static final Pattern ABSENT = Pattern.compile("GOLDCTL_ABSENT_[A-Za-z0-9_-]+");

    // ---- accepted ---------------------------------------------------------

    @Test
    public void acceptsCanonicalOccupiedSnapshot() {
        GoldInputStatusClient.Snapshot snapshot =
                GoldInputStatusClient.parseReady(clean(ready("2", "event12,event3")));

        assertNotNull(snapshot);
        assertEquals(BOOT, snapshot.bootId);
        assertEquals(17L, snapshot.generation);
        assertEquals(INSTANCE, snapshot.instance);
        assertEquals(Arrays.asList("event12", "event3"), snapshot.nodes);
    }

    @Test
    public void acceptsVerifiedEmptySnapshot() {
        GoldInputStatusClient.Snapshot snapshot =
                GoldInputStatusClient.parseReady(clean(ready("0", "-")));

        assertNotNull(snapshot);
        assertTrue(snapshot.nodes.isEmpty());
    }

    // ---- rejected ---------------------------------------------------------

    @Test
    public void rejectsNonCanonicalNodeLists() {
        // Descending order: Gold's grammar is a strict ascending list.
        assertNull(GoldInputStatusClient.parseReady(clean(ready("2", "event3,event12"))));
        // Leading zero is not a node Gold will ever name.
        assertNull(GoldInputStatusClient.parseReady(clean(ready("1", "event01"))));
        // Duplicates cannot survive the ascending requirement either.
        assertNull(GoldInputStatusClient.parseReady(clean(ready("2", "event3,event3"))));
        // event0 is legal on its own, but count=0 must be spelled nodes=-.
        assertNull(GoldInputStatusClient.parseReady(clean(ready("0", "event0"))));
        assertNull(GoldInputStatusClient.parseReady(clean(ready("1", "-"))));
        // Declared count disagrees with the list length.
        assertNull(GoldInputStatusClient.parseReady(clean(ready("3", "event3,event4"))));
        assertNull(GoldInputStatusClient.parseReady(clean(ready("1", "event3,event4"))));
    }

    @Test
    public void rejectsTrailingNoiseAndExtraFields() {
        assertNull(GoldInputStatusClient.parseReady(clean(ready("1", "event0") + "noise\n")));
        assertNull(GoldInputStatusClient.parseReady(clean(ready("1", "event0") + "\nmore\n")));
        assertNull(GoldInputStatusClient.parseReady(clean(
                ready("1", "event0").trim() + "\textra\n")));
        // Missing trailing newline is fine; the line itself is what must be exact.
        assertNotNull(GoldInputStatusClient.parseReady(clean(ready("1", "event0").trim())));
    }

    @Test
    public void rejectsAResultThatWasNotClean() {
        assertNull(GoldInputStatusClient.parseReady(
                new SuCommand.CommandResult(0, ready("1", "event0"), "warning", false, false, false)));
        assertNull(GoldInputStatusClient.parseReady(
                new SuCommand.CommandResult(0, ready("1", "event0"), "", true, false, false)));
        assertNull(GoldInputStatusClient.parseReady(
                new SuCommand.CommandResult(0, ready("1", "event0"), "", false, true, false)));
        assertNull(GoldInputStatusClient.parseReady(
                new SuCommand.CommandResult(0, ready("1", "event0"), "", false, false, true)));
        assertNull(GoldInputStatusClient.parseReady(
                new SuCommand.CommandResult(1, ready("1", "event0"), "", false, false, false)));
        assertNull(GoldInputStatusClient.parseReady(null));
    }

    @Test
    public void rejectsBadIdentityAndGeneration() {
        assertNull(GoldInputStatusClient.parseReady(clean(line("boot id!", "17", INSTANCE, "1", "event0"))));
        assertNull(GoldInputStatusClient.parseReady(clean(line(BOOT, "17", "short", "1", "event0"))));
        assertNull(GoldInputStatusClient.parseReady(clean(line(BOOT, "0", INSTANCE, "1", "event0"))));
        assertNull(GoldInputStatusClient.parseReady(clean(line(BOOT, "-1", INSTANCE, "1", "event0"))));
        assertNull(GoldInputStatusClient.parseReady(clean(line(BOOT, "1.5", INSTANCE, "1", "event0"))));
        assertNull(GoldInputStatusClient.parseReady(clean(line(BOOT, "17", INSTANCE, "33", "event0"))));
    }

    /**
     * These vectors are mirrored by tests/test_keyboardctl_input_status.sh. The
     * two validators must agree: keyboardctl emitting a line this parser rejects
     * would turn a good snapshot into UNKNOWN.
     */
    @Test
    public void rejectsReorderedFields() {
        assertNull(GoldInputStatusClient.parseReady(clean(
                "GOLDCTL1\tinput-status\tok\tcode=ready\tgeneration=17\tboot_id=" + BOOT
                        + "\tinstance=" + INSTANCE + "\tcount=0\tnodes=-\n")));
    }

    @Test
    public void rejectsMoreNodesThanTheHelperCanHold() {
        StringBuilder nodes = new StringBuilder("event0");
        for (int i = 1; i <= 32; i++)
            nodes.append(",event").append(i);
        // 33 strictly ascending nodes, which is one past MAX_SELECTED_NODES.
        assertNull(GoldInputStatusClient.parseReady(clean(ready("33", nodes.toString()))));
    }

    // ---- the claimed set ---------------------------------------------------

    @Test
    public void acceptsAClaimedFieldWiderThanWhatIsHeld() {
        // Gold holds one keyboard but is responsible for both transports of it.
        GoldInputStatusClient.Snapshot snapshot = GoldInputStatusClient.parseReady(clean(
                readyWithoutNewline("1", "event12") + "\tclaimed=event12,event9\n"));

        assertNotNull(snapshot);
        assertEquals(Arrays.asList("event12"), snapshot.nodes);
        assertEquals(Arrays.asList("event12", "event9"), snapshot.claimed);
    }

    @Test
    public void aGoldWithoutTheClaimedFieldFallsBackToWhatItHolds() {
        // Version skew is real: Gold is a separate module and may be older than
        // this app. The narrower answer is still a correct one.
        GoldInputStatusClient.Snapshot snapshot =
                GoldInputStatusClient.parseReady(clean(ready("1", "event12")));

        assertNotNull(snapshot);
        assertEquals(Arrays.asList("event12"), snapshot.claimed);
    }

    @Test
    public void anEmptyClaimedSetIsSpelledAsADash() {
        GoldInputStatusClient.Snapshot snapshot = GoldInputStatusClient.parseReady(clean(
                readyWithoutNewline("1", "event12") + "\tclaimed=-\n"));

        assertNotNull(snapshot);
        assertTrue(snapshot.claimed.isEmpty());
    }

    @Test
    public void rejectsAMalformedClaimedSet() {
        // Not the field we expect in that position.
        assertNull(GoldInputStatusClient.parseReady(clean(
                readyWithoutNewline("1", "event12") + "\tclaimed2=event9\n")));
        // Descending, duplicated, or misspelled.
        assertNull(GoldInputStatusClient.parseReady(clean(
                readyWithoutNewline("1", "event12") + "\tclaimed=event9,event12\n")));
        assertNull(GoldInputStatusClient.parseReady(clean(
                readyWithoutNewline("1", "event12") + "\tclaimed=event9,event9\n")));
        assertNull(GoldInputStatusClient.parseReady(clean(
                readyWithoutNewline("1", "event12") + "\tclaimed=event09\n")));
        assertNull(GoldInputStatusClient.parseReady(clean(
                readyWithoutNewline("1", "event12") + "\tclaimed=\n")));
    }

    @Test
    public void rejectsWrongMagicOrVerb() {
        assertNull(GoldInputStatusClient.parseReady(clean(
                "GOLDCTL2\tinput-status\tok\tcode=ready\tboot_id=" + BOOT + "\tgeneration=17"
                        + "\tinstance=" + INSTANCE + "\tcount=0\tnodes=-\n")));
        assertNull(GoldInputStatusClient.parseReady(clean(
                "GOLDCTL1\tinput-probe\tok\tcode=ready\tboot_id=" + BOOT + "\tgeneration=17"
                        + "\tinstance=" + INSTANCE + "\tcount=0\tnodes=-\n")));
        assertNull(GoldInputStatusClient.parseReady(clean(
                "GOLDCTL1\tinput-status\tok\tcode=busy\tboot_id=" + BOOT + "\tgeneration=17"
                        + "\tinstance=" + INSTANCE + "\tcount=0\tnodes=-\n")));
    }

    // ---- query ------------------------------------------------------------

    @Test
    public void queryRunsOnlyInputStatus() {
        SuCommand.CommandRunner runner = (command, timeout) -> {
            assertTrue(command.contains(" input-status"));
            assertTrue(!command.contains("probe"));
            assertTrue(!command.contains("anland-hold"));
            assertTrue(!command.contains("anland-renew"));
            assertTrue(!command.contains("anland-release"));
            assertTrue(!command.contains("restart"));
            return clean(ready("1", "event0"));
        };

        GoldInputStatusClient.Result query = client(runner, CONTROLLER).query();

        assertEquals(GoldInputStatusClient.State.VERIFIED, query.state);
        assertNotNull(query.snapshot);
        assertEquals(Arrays.asList("event0"), query.snapshot.nodes);
    }

    @Test
    public void onlyTheExactMarkerMeansAbsent() {
        SuCommand.CommandRunner exact = (command, timeout) -> {
            Matcher matcher = ABSENT.matcher(command);
            assertTrue(matcher.find());
            return new SuCommand.CommandResult(0, matcher.group() + "\n", "", false, false, false);
        };
        assertEquals(GoldInputStatusClient.State.ABSENT, client(exact, "/missing/bin/keyboardctl").query().state);

        // The same marker, but the call was not clean: cannot be trusted.
        SuCommand.CommandRunner noisy = (command, timeout) -> {
            Matcher matcher = ABSENT.matcher(command);
            assertTrue(matcher.find());
            return new SuCommand.CommandResult(0, matcher.group() + "\n", "warning", false, false, false);
        };
        assertEquals(GoldInputStatusClient.State.UNKNOWN, client(noisy, "/missing/bin/keyboardctl").query().state);

        // A marker that never matched this call's own token proves nothing.
        SuCommand.CommandRunner stale = (command, timeout) ->
                new SuCommand.CommandResult(0, "GOLDCTL_ABSENT_stale_token_value\n", "", false, false, false);
        assertEquals(GoldInputStatusClient.State.UNKNOWN, client(stale, "/missing/bin/keyboardctl").query().state);
    }

    @Test
    public void anUnreadableGoldIsUnknownNotIdle() {
        SuCommand.CommandRunner failing = (command, timeout) ->
                new SuCommand.CommandResult(1, "", "su: not found", false, false, false);
        GoldInputStatusClient.Result query = client(failing, CONTROLLER).query();

        assertEquals(GoldInputStatusClient.State.UNKNOWN, query.state);
        assertNull(query.snapshot);
    }

    // ---- helpers ----------------------------------------------------------

    private static GoldInputStatusClient client(SuCommand.CommandRunner runner, String path) {
        return new GoldInputStatusClient(new GoldController(runner, path, new SecureRandom()));
    }

    private static SuCommand.CommandResult clean(String stdout) {
        return new SuCommand.CommandResult(0, stdout, "", false, false, false);
    }

    private static String ready(String count, String nodes) {
        return line(BOOT, "17", INSTANCE, count, nodes);
    }

    /** A canonical line with no trailing newline, for appending extra fields. */
    private static String readyWithoutNewline(String count, String nodes) {
        String line = line(BOOT, "17", INSTANCE, count, nodes);
        return line.substring(0, line.length() - 1);
    }

    private static String line(String bootId, String generation, String instance, String count,
                               String nodes) {
        return "GOLDCTL1\tinput-status\tok\tcode=ready\tboot_id=" + bootId
                + "\tgeneration=" + generation + "\tinstance=" + instance + "\tcount=" + count
                + "\tnodes=" + nodes + "\n";
    }
}
