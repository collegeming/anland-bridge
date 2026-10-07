package com.anland.consumer;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * Read-only client for Gold's authoritative record of which physical input nodes
 * it currently holds under {@code EVIOCGRAB}.
 *
 * <p>The only command this class ever runs is {@code <controller> input-status}.
 * It never probes, never takes a lease, and never pauses, resumes or restarts
 * Gold. A node Gold grabbed stays grabbed whether or not Anland is looking.
 *
 * <p>Three outcomes, and the gap between the last two is the whole point:
 * {@code ABSENT} means the controller is genuinely not installed, {@code VERIFIED}
 * means Gold answered with a canonical snapshot, and {@code UNKNOWN} means
 * anything else — a failed command, a malformed line, a timeout, noise on stderr.
 * UNKNOWN is never downgraded to "nothing is occupied", because an unreadable
 * Gold is not an idle Gold, and treating it as idle is how Anland would end up
 * grabbing nodes out from under it.
 */
final class GoldInputStatusClient {
    /** Mirrors IGRAB_MAX_DEVICES in input_grab.h; Gold's own cap is smaller. */
    static final int MAX_NODES = 32;

    private static final String VERB = "input-status";
    private static final String MAGIC = "GOLDCTL1";
    private static final String READY = "ready";

    private static final Pattern BOOT_ID = Pattern.compile("[A-Za-z0-9_-]{1,128}");
    private static final Pattern INSTANCE = Pattern.compile("[A-Za-z0-9_-]{16,128}");
    /** Gold's strict grammar. {@code event01} is not a node Gold will ever name. */
    private static final Pattern EVENT_NODE = Pattern.compile("event(?:0|[1-9][0-9]*)");

    /** Exact field order of a canonical ready line, after the three fixed tokens. */
    private static final String[] FIELD_KEYS = {
            "code", "boot_id", "generation", "instance", "count", "nodes"
    };

    /** Optional trailing field: the keyboards Gold is responsible for. */
    private static final String CLAIMED_PREFIX = "claimed=";

    enum State { ABSENT, VERIFIED, UNKNOWN }

    static final class Snapshot {
        final String bootId;
        final long generation;
        final String instance;
        /** What Gold holds under EVIOCGRAB right now. */
        final List<String> nodes;
        /**
         * Every keyboard Gold is responsible for, whether or not it holds it at
         * this instant.
         *
         * <p>A keyboard offered over two transports can be held over either one,
         * and which is held can change across a replug. Something that means to
         * stay out of Gold's way has to avoid the whole family — "Gold is not
         * holding it this second" is not the same as "it is free".
         */
        final List<String> claimed;

        Snapshot(String bootId, long generation, String instance, List<String> nodes,
                 List<String> claimed) {
            this.bootId = bootId;
            this.generation = generation;
            this.instance = instance;
            this.nodes = Collections.unmodifiableList(new ArrayList<>(nodes));
            this.claimed = Collections.unmodifiableList(new ArrayList<>(claimed));
        }
    }

    static final class Result {
        final State state;
        final Snapshot snapshot;

        private Result(State state, Snapshot snapshot) {
            this.state = state;
            this.snapshot = snapshot;
        }

        static Result absent() {
            return new Result(State.ABSENT, null);
        }

        static Result verified(Snapshot snapshot) {
            return new Result(State.VERIFIED, snapshot);
        }

        static Result unknown() {
            return new Result(State.UNKNOWN, null);
        }
    }

    private final GoldController controller;

    GoldInputStatusClient(GoldController controller) {
        if (controller == null)
            throw new IllegalArgumentException("missing Gold controller");
        this.controller = controller;
    }

    Result query() {
        return query(GoldController.COMMAND_TIMEOUT_MS);
    }

    /** Blocking; callers must run this off the main thread. */
    Result query(long timeoutMs) {
        GoldController.GuardedResult guarded = controller.runGuarded(VERB, timeoutMs);
        if (isAbsent(guarded))
            return Result.absent();
        Snapshot snapshot = parseReady(guarded.result);
        return snapshot == null ? Result.unknown() : Result.verified(snapshot);
    }

    /**
     * Accepts exactly one canonical ready line and nothing else.
     *
     * @return the snapshot, or {@code null} when anything at all is off.
     */
    static Snapshot parseReady(SuCommand.CommandResult result) {
        if (result == null || !result.isClean())
            return null;

        String stdout = result.stdout;
        if (stdout.endsWith("\n"))
            stdout = stdout.substring(0, stdout.length() - 1);
        if (stdout.isEmpty() || stdout.indexOf('\n') >= 0)
            return null;

        String[] fields = stdout.split("\t", -1);
        // The claimed field is optional: it arrived after the first release, and
        // a Gold without it still answers the question it was asked. Its absence
        // falls back to the held set, which is the narrower answer -- correct,
        // just less protective.
        boolean hasClaimed = fields.length == 4 + FIELD_KEYS.length;
        if (fields.length != 3 + FIELD_KEYS.length && !hasClaimed)
            return null;
        if (!MAGIC.equals(fields[0]) || !VERB.equals(fields[1]) || !"ok".equals(fields[2]))
            return null;

        Map<String, String> values = new HashMap<>();
        for (int i = 0; i < FIELD_KEYS.length; i++) {
            String field = fields[3 + i];
            int equals = field.indexOf('=');
            if (equals <= 0)
                return null;
            String key = field.substring(0, equals);
            // Positional and exact: a reordered or duplicated field is not a
            // snapshot we can act on, even if every value looks plausible.
            if (!FIELD_KEYS[i].equals(key))
                return null;
            if (values.put(key, field.substring(equals + 1)) != null)
                return null;
        }

        if (!READY.equals(values.get("code")))
            return null;

        String bootId = values.get("boot_id");
        String instance = values.get("instance");
        if (!BOOT_ID.matcher(bootId).matches() || !INSTANCE.matcher(instance).matches())
            return null;

        long generation = parsePositiveLong(values.get("generation"));
        if (generation < 1)
            return null;

        int count = parseCount(values.get("count"));
        if (count < 0 || count > MAX_NODES)
            return null;

        List<String> nodes = parseNodes(values.get("nodes"), count);
        if (nodes == null)
            return null;

        List<String> claimed = nodes;
        if (hasClaimed) {
            String field = fields[3 + FIELD_KEYS.length];
            if (!field.startsWith(CLAIMED_PREFIX))
                return null;
            claimed = parseUncountedNodes(field.substring(CLAIMED_PREFIX.length()));
            if (claimed == null)
                return null;
        }

        return new Snapshot(bootId, generation, instance, nodes, claimed);
    }

    /**
     * A node list with no count to check it against. {@code -} means none;
     * otherwise the entries must be strictly ascending and strictly spelled,
     * which is the same rule that makes duplicates impossible in {@code nodes}.
     */
    private static List<String> parseUncountedNodes(String value) {
        if (value == null || value.isEmpty())
            return null;
        if ("-".equals(value))
            return Collections.emptyList();

        String[] parts = value.split(",", -1);
        if (parts.length > MAX_NODES)
            return null;

        List<String> nodes = new ArrayList<>(parts.length);
        String previous = null;
        for (String part : parts) {
            if (!EVENT_NODE.matcher(part).matches())
                return null;
            if (previous != null && previous.compareTo(part) >= 0)
                return null;
            nodes.add(part);
            previous = part;
        }
        return nodes;
    }

    /**
     * ABSENT needs the exact marker this call generated, plus a clean call: a
     * marker that arrived alongside stderr or a timeout proves nothing.
     */
    private static boolean isAbsent(GoldController.GuardedResult guarded) {
        return guarded.result.isClean() && guarded.result.stdout.trim().equals(guarded.marker);
    }

    private static long parsePositiveLong(String value) {
        if (value == null || value.isEmpty() || value.length() > 18)
            return -1;
        for (int i = 0; i < value.length(); i++) {
            if (value.charAt(i) < '0' || value.charAt(i) > '9')
                return -1;
        }
        try {
            long parsed = Long.parseLong(value);
            return parsed < 1 ? -1 : parsed;
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private static int parseCount(String value) {
        long parsed = parsePositiveLong(value);
        if (parsed > 0)
            return (int) parsed;
        // "0" is a legitimate count and parsePositiveLong rejects it by contract.
        return "0".equals(value) ? 0 : -1;
    }

    /**
     * {@code count=0} must be spelled {@code nodes=-}. Otherwise the list must
     * have exactly {@code count} strictly ascending, strictly-spelled entries —
     * ascending order is what makes duplicates impossible.
     */
    private static List<String> parseNodes(String value, int count) {
        if (value == null)
            return null;
        if (count == 0)
            return "-".equals(value) ? Collections.<String>emptyList() : null;

        String[] parts = value.split(",", -1);
        if (parts.length != count)
            return null;

        List<String> nodes = new ArrayList<>(count);
        String previous = null;
        for (String part : parts) {
            if (!EVENT_NODE.matcher(part).matches())
                return null;
            if (previous != null && previous.compareTo(part) >= 0)
                return null;
            nodes.add(part);
            previous = part;
        }
        return nodes;
    }
}
