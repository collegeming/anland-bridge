#!/system/bin/sh
MODDIR=${0%/*}
SOCK=/data/local/tmp/display_daemon.sock

# service.sh can re-run (module update, late-load). The broker must stay
# single-instance or a second daemon steals the socket path. ponytail: if the
# daemon is alive but the socket vanished we exit anyway; toggle the module to recover.
pgrep -f display_daemon >/dev/null 2>&1 && exit 0

# Files this script creates stay root-only; the broker socket is exempt, since the
# compositor reaches it from inside a container as an unprivileged user. The daemon
# sets that socket's mode itself.
umask 077
rm -f "$SOCK"
"$MODDIR/display_daemon" "$SOCK" &
