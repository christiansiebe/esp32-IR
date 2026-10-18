#!/usr/bin/env bash
# Keep the two sandbox-side helpers alive across crashes and sandbox restarts:
#   - channel_server.py (port 12001): the reverse command channel the Odroid polls
#   - upload-server/server.py (port 12000): serves /files for OTA pulls
# PID 1 is openhands-agent; there is no systemd or cron in this sandbox, so a
# bare background process dies for good when the sandbox restarts. This loop
# restarts them. Start it detached once:
#   setsid nohup bash /workspace/remote-shell/keepalive.sh >>/workspace/remote-shell/keepalive.log 2>&1 &
set -u

CHAN_DIR=/workspace/remote-shell
UP_DIR=/workspace/upload-server
LOCKFILE=/workspace/remote-shell/keepalive.lock

# Only one keepalive may run. Several callers (channel.py self-heal, a manual
# start) can race. flock is atomic and, crucially, it is released the moment this
# process dies - so a fresh keepalive can take over. The catch: a forked child
# would inherit the lock fd and keep the lock alive after the keeper dies, so
# every child is launched with fd 9 closed (see 9>&- below).
exec 9>"$LOCKFILE"
if ! flock -n 9; then
  echo "$(date -u +%H:%M:%SZ) keepalive already running - exit"
  exit 0
fi

log() { echo "$(date -u +%H:%M:%SZ) $*"; }

ensure_channel() {
  pgrep -f "python3 channel_server.py" >/dev/null && return
  ( cd "$CHAN_DIR" && exec setsid nohup python3 channel_server.py >>"$CHAN_DIR/channel_server.log" 2>&1 9>&- ) &
  log "started channel_server.py (12001)"
}

ensure_upload() {
  pgrep -f "upload-server/server.py" >/dev/null && return
  ( cd "$UP_DIR" && exec setsid nohup python3 "$UP_DIR/server.py" >>"$UP_DIR/server.log" 2>&1 9>&- ) &
  log "started upload-server (12000)"
}

# Keep the fleet watchdog alive: it wakes sibling chat agents whose sandbox was
# paused while they still have open work. It holds its own lock, so starting it
# when one is already running is a no-op.
ensure_fleet() {
  pgrep -f "fleet/fleet.py --loop" >/dev/null && return
  ( cd /workspace/fleet && exec setsid nohup python3 /workspace/fleet/fleet.py --loop --interval 300 >>/workspace/fleet/fleet.log 2>&1 9>&- ) &
  log "started fleet watchdog"
}

log "keepalive up (pid $$)"
while true; do
  ensure_channel
  ensure_upload
  ensure_fleet
  sleep 20 9>&-
done
