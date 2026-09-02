#include <signal.h>
#include <sys/stat.h>

#include "../libdisplay_daemon/display_daemon.h"

/* Standalone daemon: a plain ELF started by the magisk module. The daemon logic
 * lives in libdisplay_daemon; this file only wires up process-global concerns
 * (signals) that the library deliberately leaves to its host. */

static daemon_ctx *g_ctx;

static void handle_signal(int sig)
{
    (void)sig;
    if (g_ctx)
        daemon_stop(g_ctx);
}

int main(int argc, char **argv)
{
    const char *sock_path = (argc > 1) ? argv[1] : "/data/local/tmp/display_daemon.sock";

    /* Module hosts may run us with umask 0; the broker socket must stay
     * root-only (all peers connect through root fd handoff anyway). */
    umask(0077);

    if (daemon_create(&g_ctx, sock_path) < 0)
        return 1;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    daemon_run(g_ctx);
    daemon_destroy(g_ctx);
    return 0;
}
