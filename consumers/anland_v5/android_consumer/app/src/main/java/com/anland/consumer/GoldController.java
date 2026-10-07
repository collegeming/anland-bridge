package com.anland.consumer;

import java.io.File;
import java.security.SecureRandom;
import java.util.regex.Pattern;

/**
 * The one place that invokes Gold's controller script, and the only place that
 * knows how to tell "Gold is not installed" apart from "Gold did not answer".
 *
 * <p>Strictly read-only, and deliberately so. Anland never pauses, resumes,
 * restarts or leases Gold: a node Gold grabbed stays grabbed whether or not
 * anyone is watching. The only verb this class is ever given is
 * {@code input-status}.
 *
 * <p>Android-free on purpose, so the command-building and parsing contract can be
 * exercised by plain JVM tests.
 */
final class GoldController {
    /** Where the Magisk module installs the controller. */
    static final String DEFAULT_CONTROLLER =
            "/data/adb/modules/Gold_Keyboardremaps/bin/keyboardctl";

    /** Budget for one status call. Gold answers from a file, so this is generous. */
    static final long COMMAND_TIMEOUT_MS = 8000L;

    static final String ABSENT_MARKER_PREFIX = "GOLDCTL_ABSENT_";

    private static final Pattern TOKEN = Pattern.compile("[A-Za-z0-9_-]{1,128}");
    private static final String SHELL = "/system/bin/sh";

    /** A result paired with the absence marker that was generated for that call. */
    static final class GuardedResult {
        final SuCommand.CommandResult result;
        final String marker;

        GuardedResult(SuCommand.CommandResult result, String marker) {
            this.result = result;
            this.marker = marker;
        }
    }

    private final SuCommand.CommandRunner runner;
    private final String controllerPath;
    private final SecureRandom random;

    GoldController(SuCommand.CommandRunner runner, String controllerPath, SecureRandom random) {
        if (runner == null || controllerPath == null || controllerPath.isEmpty() || random == null)
            throw new IllegalArgumentException("missing Gold controller dependency");
        this.runner = runner;
        this.controllerPath = controllerPath;
        this.random = random;
    }

    GuardedResult runGuarded(String arguments) {
        return runGuarded(arguments, COMMAND_TIMEOUT_MS);
    }

    /**
     * @param timeoutMs budget for this call. Interactive callers pass less: a
     *                  user waiting on a key press is worse served by a complete
     *                  answer than by a timely one.
     */
    GuardedResult runGuarded(String arguments, long timeoutMs) {
        String marker = ABSENT_MARKER_PREFIX + newToken();
        return new GuardedResult(runner.run(buildCommand(arguments, marker), timeoutMs), marker);
    }

    /**
     * Builds the guarded invocation. The marker is generated fresh per call and
     * is echoed only by the branch that has actually proven the module directory
     * is missing, so no stale or unrelated output can be mistaken for absence.
     */
    String buildCommand(String arguments, String marker) {
        // The controller is invoked through sh, not executed directly: bin/ can
        // end up without its exec bit on an upgraded install.
        return "if [ ! -d " + SuCommand.shellQuote(moduleDirectory()) + " ]; then printf '%s\\n' '"
                + marker + "'; exit 0; fi; "
                + "exec " + SHELL + " " + SuCommand.shellQuote(controllerPath) + " " + arguments;
    }

    /** The module root: {@code <mod>/bin/keyboardctl} -> {@code <mod>}. */
    String moduleDirectory() {
        File bin = new File(controllerPath).getParentFile();
        File root = bin == null ? null : bin.getParentFile();
        return root == null ? controllerPath : root.getPath();
    }

    private String newToken() {
        // Rejection-free alphabet: the marker must survive the shell and the
        // strict token grammar both.
        StringBuilder sb = new StringBuilder(32);
        for (int i = 0; i < 4; i++) {
            sb.append(Long.toString(random.nextLong() & 0x7fffffffffffffffL, 36));
            sb.append('_');
        }
        String token = sb.toString();
        return TOKEN.matcher(token).matches() ? token : "marker_fallback_0000000000";
    }
}
