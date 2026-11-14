#!/bin/sh
# Install the FNB-48 auto-stop udev rule and its helper. Run as root:
#
#   sudo tools/udev/install-fnb48-stop.sh
#
# This is a repo artifact - it is NOT executed automatically and does not touch
# /etc on its own. It copies the reader + wrapper into /usr/local/lib/fnb48 and
# the rule into /etc/udev/rules.d/, then reloads udev. Re-run it after editing
# the rule or the reader.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
lib=/usr/local/lib/fnb48
rules=/etc/udev/rules.d/99-fnb48-stop.rules

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root: sudo $0" >&2
    exit 1
fi

install -d "$lib"
install -m 0644 "$here/../fnb48_read.py" "$lib/fnb48_read.py"
install -m 0755 "$here/fnb48-stop.sh" "$lib/fnb48-stop.sh"
install -m 0644 "$here/99-fnb48-stop.rules" "$rules"

udevadm control --reload
# Re-run the rule for a meter that is already plugged in.
udevadm trigger --subsystem-match=hidraw --action=add

echo "installed $rules (helper: $lib/fnb48-stop.sh)"
echo "check: udevadm test --action=add \$(udevadm info -q path -n /dev/hidraw0)"
