package com.anland.consumer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

/**
 * The one thing standing between a user-supplied string and a command run as
 * root. Everything the app passes to {@code su -c} goes through this, and the
 * values are not all ours: the daemon socket path comes from a saved preference
 * and from the launch Intent of an exported activity.
 *
 * <p>The property that matters is not how the result looks but what a shell
 * makes of it, so most of this test unwraps the quoting by the shell's own
 * rules and checks the original comes back. A test that only compared against a
 * literal would agree with a wrong implementation as long as it was
 * consistently wrong.
 */
public class SuCommandTest {
    /**
     * The inverse of single-quote quoting, as a POSIX shell reads it: everything
     * between quotes is literal, and a quote cannot appear inside one, so an
     * embedded quote is written by closing, escaping and reopening.
     */
    private static String unquoteAsShellWould(String quoted) {
        StringBuilder out = new StringBuilder();
        int i = 0;
        while (i < quoted.length()) {
            char c = quoted.charAt(i);
            if (c == '\'') {
                i++;
                while (i < quoted.length() && quoted.charAt(i) != '\'')
                    out.append(quoted.charAt(i++));
                assertTrue("unterminated quote in " + quoted, i < quoted.length());
                i++; // closing quote
            } else if (c == '\\') {
                assertTrue("dangling backslash in " + quoted, i + 1 < quoted.length());
                out.append(quoted.charAt(++i));
                i++;
            } else {
                out.append(c);
                i++;
            }
        }
        return out.toString();
    }

    private static void survivesTheShell(String input) {
        String quoted = SuCommand.shellQuote(input);
        assertEquals("round trip through a shell: " + input, input,
                unquoteAsShellWould(quoted));
    }

    @Test
    public void aPlainPathIsQuotedWhole() {
        assertEquals("'/data/local/tmp/display_daemon.sock'",
                SuCommand.shellQuote("/data/local/tmp/display_daemon.sock"));
    }

    @Test
    public void anEmbeddedQuoteIsClosedEscapedAndReopened() {
        assertEquals("'a'\\''b'", SuCommand.shellQuote("a'b"));
    }

    @Test
    public void anythingAShellWouldActOnComesBackLiteral() {
        // Each of these ends the word, starts a substitution, or splits the
        // command when it is not quoted. Under quoting all of them are just
        // characters in a path.
        String[] hostile = {
                "/tmp/x; id",
                "/tmp/x && id",
                "/tmp/x | id",
                "/tmp/x `id`",
                "/tmp/x $(id)",
                "/tmp/x > /data/local/tmp/pwned",
                "/tmp/x\nid",
                "/tmp/x ' id",
                "/tmp/x \" id",
                "/tmp/*",
                "/tmp/x\\",
                "'",
                "''",
                "\\",
                "",
        };
        for (String input : hostile)
            survivesTheShell(input);
    }

    @Test
    public void aQuoteInsideAQuoteIsStillJustAQuote() {
        survivesTheShell("a'b'c");
        survivesTheShell("'; id; '");
        survivesTheShell("''''");
    }

    @Test
    public void theResultIsASingleShellWord() {
        // No unquoted whitespace outside the wrapping quotes: the shell must see
        // one argument, not several.
        for (String input : new String[]{"a b", "a\tb", "a\nb", " a ", "a  b"}) {
            String quoted = SuCommand.shellQuote(input);
            assertEquals("must open with a quote", '\'', quoted.charAt(0));
            assertEquals("must close with a quote", '\'', quoted.charAt(quoted.length() - 1));
            assertEquals("must be one word", input, unquoteAsShellWould(quoted));
        }
    }
}
