package com.anland.consumer;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

/**
 * Runs one shell command as root and reports how it went, bounded in both time
 * and bytes so a wedged helper can never pin the caller.
 *
 * <p>Android-free on purpose: everything that decides what a result means lives
 * in the callers, and they are unit tested on the JVM.
 */
final class SuCommand {

    /** Anything past this is a runaway process, not an answer. */
    static final int MAX_OUTPUT_BYTES = 4096;

    /** Runs one already-built shell command. */
    interface CommandRunner {
        CommandResult run(String command, long timeoutMs);
    }

    static final class CommandResult {
        final int exitCode;
        final String stdout;
        final String stderr;
        final boolean timedOut;
        final boolean truncated;
        /** {@code su} itself never produced a process to talk to. */
        final boolean unavailable;

        CommandResult(int exitCode, String stdout, String stderr, boolean timedOut,
                      boolean truncated, boolean unavailable) {
            this.exitCode = exitCode;
            this.stdout = stdout == null ? "" : stdout;
            this.stderr = stderr == null ? "" : stderr;
            this.timedOut = timedOut;
            this.truncated = truncated;
            this.unavailable = unavailable;
        }

        /** Whether the call itself was clean enough for its stdout to mean anything. */
        boolean isClean() {
            return !unavailable && !timedOut && !truncated && exitCode == 0
                    && stderr.trim().isEmpty();
        }
    }

    private SuCommand() {
    }

    static String shellQuote(String value) {
        return "'" + value.replace("'", "'\\''") + "'";
    }

    /** Production runner: one {@code su -c} per call. */
    static final class SuRunner implements CommandRunner {
        @Override
        public CommandResult run(String command, long timeoutMs) {
            Process process;
            try {
                process = new ProcessBuilder("su", "-c", command).start();
            } catch (IOException e) {
                return new CommandResult(-1, "", String.valueOf(e.getMessage()), false, false,
                        true);
            }

            StreamDrain out = new StreamDrain(process.getInputStream());
            StreamDrain err = new StreamDrain(process.getErrorStream());
            Thread tOut = drain(out, "su-stdout");
            Thread tErr = drain(err, "su-stderr");

            boolean finished;
            try {
                finished = process.waitFor(timeoutMs, TimeUnit.MILLISECONDS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                finished = false;
            }
            if (!finished) {
                process.destroy();
                try {
                    process.waitFor(500, TimeUnit.MILLISECONDS);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
            join(tOut);
            join(tErr);
            if (!finished)
                return new CommandResult(-1, out.text(), err.text(), true,
                        out.truncated() || err.truncated(), false);
            return new CommandResult(process.exitValue(), out.text(), err.text(), false,
                    out.truncated() || err.truncated(), false);
        }

        private static Thread drain(StreamDrain drain, String name) {
            Thread thread = new Thread(drain, name);
            thread.setDaemon(true);
            thread.start();
            return thread;
        }

        private static void join(Thread thread) {
            try {
                thread.join(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }

    /** Reads one stream to EOF, keeping at most {@link #MAX_OUTPUT_BYTES}. */
    private static final class StreamDrain implements Runnable {
        private final InputStream in;
        private final StringBuilder text = new StringBuilder();
        /** Written by the drain thread, read by the caller after a bounded join. */
        private volatile boolean over;

        StreamDrain(InputStream in) {
            this.in = in;
        }

        @Override
        public void run() {
            byte[] buffer = new byte[512];
            try {
                int read;
                while ((read = in.read(buffer)) != -1) {
                    synchronized (text) {
                        int room = MAX_OUTPUT_BYTES - text.length();
                        if (room <= 0) {
                            over = true;
                            continue;
                        }
                        int take = Math.min(read, room);
                        text.append(new String(buffer, 0, take, StandardCharsets.UTF_8));
                        if (take < read || text.length() >= MAX_OUTPUT_BYTES)
                            over = true;
                    }
                }
            } catch (IOException ignored) {
                // Closed by destroy(), or the child exited mid-read.
            }
        }

        String text() {
            synchronized (text) {
                return text.toString();
            }
        }

        boolean truncated() {
            return over;
        }
    }
}
