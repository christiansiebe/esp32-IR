#!/bin/bash
# Sandbox -> Odroid: reverse SSH tunnel + optional interactive shell.
# Connects OUTGOING from the sandbox, so no inbound port is needed on the Odroid.
#
# Usage:
#   SANDBOX_SSH_HOST=odroid SANDBOX_SSH_USER=me ./reverse_ssh.sh          # interactive shell
#   SANDBOX_SSH_HOST=odroid SANDBOX_SSH_USER=me ./reverse_ssh.sh -R       # only the tunnel
#
# Config via env:
#   SANDBOX_SSH_HOST   required, host or IP of the Odroid (reachable from here)
#   SANDBOX_SSH_USER   required, login user
#   SANDBOX_SSH_PORT   default 22
#   SANDBOX_SSH_KEY    default ~/.ssh/id_ed25519
#   SANDBOX_SSH_TOKEN  required for the -R tunnel: a secret you choose; the Odroid
#                      exposes the sandbox shell on 127.0.0.1:2222 (see README).
set -euo pipefail

HOST="${SANDBOX_SSH_HOST:?SANDBOX_SSH_HOST fehlt}"
USER="${SANDBOX_SSH_USER:?SANDBOX_SSH_USER fehlt}"
PORT="${SANDBOX_SSH_PORT:-22}"
KEY="${SANDBOX_SSH_KEY:-$HOME/.ssh/id_ed25519}"

OPTS=(-p "$PORT" -o BatchMode=yes -o StrictHostKeyChecking=accept-new
      -o ServerAliveInterval=30 -o ServerAliveCountMax=3)
[ -f "$KEY" ] && OPTS+=(-i "$KEY")

if [ "${1:-}" = "-R" ]; then
    : "${SANDBOX_SSH_TOKEN:?SANDBOX_SSH_TOKEN fehlt (fuer den Tunnel)}"
    echo "Baue Reverse-Tunnel: Odroid:127.0.0.1:2222 -> Sandbox:12001 (WS-Terminal)"
    exec ssh "${OPTS[@]}" -N -R 127.0.0.1:2222:127.0.0.1:12001 "$USER@$HOST"
else
    echo "Verbinde interaktive Shell: $USER@$HOST:$PORT"
    exec ssh "${OPTS[@]}" "$USER@$HOST"
fi
