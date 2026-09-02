#!/system/bin/sh
MODDIR=${0%/*}
SOCK=/data/local/tmp/display_daemon.sock

# service.sh can re-run (module update, late-load). The broker must stay
# single-instance or a second daemon steals the socket path. ponytail: if the
# daemon is alive but the socket vanished we exit anyway; toggle the module to recover.
pgrep -f display_daemon >/dev/null 2>&1 && exit 0

umask 077
rm -f "$SOCK"
"$MODDIR/display_daemon" "$SOCK" &
