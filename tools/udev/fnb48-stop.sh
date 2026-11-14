#!/bin/sh
# udev RUN+ helper: clear the stream the FNB-48 (0483:003a) may have left
# running, so its buffer cannot fill up and freeze the live values.
#
# Invoked by 99-fnb48-stop.rules with the hidraw node as $1. Bounded twice
# over: the reader's --stop-only path uses a non-blocking hidraw fd and select
# timeouts, and `timeout` caps the whole call so a wedged node can never stall
# udev. No USB unbind/reset here (that has hard-reset this board before).
set -eu
dev="${1:-/dev/hidraw0}"
py="${PYTHON:-python3}"
exec timeout 5 "$py" /usr/local/lib/fnb48/fnb48_read.py --stop-only -d "$dev"
