package com.anland.consumer;

import java.util.Collections;
import java.util.Set;
import java.util.TreeSet;
import java.util.regex.Pattern;

/**
 * Works out which nodes a direct session should ask for, and which ones it must
 * keep its hands off.
 *
 * <p>Two grammars, deliberately different:
 * <ul>
 *   <li>What the user saved in Settings is checked with the permissive
 *       {@code event[0-9]+}, so a preference written by an older build (or by
 *       hand) keeps working. {@code event01} stays valid here.</li>
 *   <li>What Gold reports as occupied is checked with the strict
 *       {@code event(?:0|[1-9][0-9]*)} — the same grammar Gold itself emits.
 *       Anything else is dropped rather than trusted.</li>
 * </ul>
 *
 * <p>Nothing here writes to SharedPreferences. The user's saved intent is theirs;
 * this only computes what the current session can actually honour.
 */
final class ImmersiveDirectSelection {
    private static final Pattern EVENT_NODE = Pattern.compile("event[0-9]+");
    private static final Pattern GOLD_EVENT_NODE = Pattern.compile("event(?:0|[1-9][0-9]*)");

    static final class Result {
        /** {@code null} means "automatic": let the helper pick. */
        final Set<String> selectedNodes;
        final Set<String> excludedNodes;
        final boolean invalid;
        /** Every saved node is held by Gold, so there is nothing left to take. */
        final boolean allFiltered;

        private Result(Set<String> selectedNodes, Set<String> excludedNodes, boolean invalid,
                       boolean allFiltered) {
            this.selectedNodes = selectedNodes == null ? null
                    : Collections.unmodifiableSet(new TreeSet<>(selectedNodes));
            this.excludedNodes = Collections.unmodifiableSet(new TreeSet<>(
                    excludedNodes == null ? Collections.<String>emptySet() : excludedNodes));
            this.invalid = invalid;
            this.allFiltered = allFiltered;
        }
    }

    private ImmersiveDirectSelection() {
    }

    /**
     * @param automatic whether the user left the node list on "auto"
     * @param saved     the persisted selection, or {@code null}
     * @param occupied  nodes Gold reported as VERIFIED-occupied; ignored when unverifiable
     * @param maxNodes  the helper's device cap
     */
    static Result resolve(boolean automatic, Set<String> saved, Set<String> occupied, int maxNodes) {
        TreeSet<String> exclusions = sanitizeOccupied(occupied);

        // Automatic stays automatic. An unverifiable Gold contributes no
        // exclusions, but it is also never quietly turned into a manual
        // selection — the UI is what tells the user we could not check.
        if (automatic)
            return new Result(null, exclusions, false, false);

        if (saved == null || saved.isEmpty() || saved.size() > maxNodes)
            return new Result(Collections.<String>emptySet(), exclusions, true, false);

        TreeSet<String> selected = new TreeSet<>();
        for (String node : saved) {
            if (node == null || !EVENT_NODE.matcher(node).matches())
                return new Result(Collections.<String>emptySet(), exclusions, true, false);
            selected.add(node);
        }
        // Collapsed duplicates: the saved list said the same node twice.
        if (selected.size() != saved.size())
            return new Result(Collections.<String>emptySet(), exclusions, true, false);

        selected.removeAll(exclusions);
        // Empty after filtering is not a reason to fall back to automatic: that
        // would silently grab nodes the user explicitly did not choose.
        return new Result(selected, exclusions, false, selected.isEmpty());
    }

    private static TreeSet<String> sanitizeOccupied(Set<String> occupied) {
        TreeSet<String> result = new TreeSet<>();
        if (occupied == null)
            return result;
        for (String node : occupied) {
            if (node != null && GOLD_EVENT_NODE.matcher(node).matches())
                result.add(node);
        }
        return result;
    }
}
