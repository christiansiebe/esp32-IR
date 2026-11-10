"""Host tests for remote-shell/ha/deploy_ap.sh, revert_ap.sh and the dead-man
switch (odroid_ap_deadman.sh). No board, no network, no root.

Everything runs against a scratch tree: AP_ROOT is a path prefix for every
board path, and with a prefix set the scripts record system commands
(systemctl/nmcli/iptables/ip) to a log instead of executing them. So the whole
roll-out/roll-back flow is exercised for real - only the syscalls are captured.

The checks focus on what the plan (ODROID-LAN-AP-KAMERA.md section 6) requires:
  * dry-run is the default and changes nothing,
  * placeholders / missing secrets are refused before anything is installed,
  * apply is idempotent (running it twice is the same as once),
  * the dead-man switch reverts automatically when it was not confirmed,
  * a confirmed deploy is NOT reverted,
  * revert restores the initial state and is itself idempotent.
"""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import odroid_ap_config as g  # noqa: E402

DEPLOY = os.path.join(HERE, "deploy_ap.sh")
REVERT = os.path.join(HERE, "revert_ap.sh")
DEADMAN = os.path.join(HERE, "odroid_ap_deadman.sh")

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def run(script, args, root, extra_env=None):
    env = dict(os.environ)
    if root is not None:
        env["AP_ROOT"] = root
    if extra_env:
        env.update(extra_env)
    return subprocess.run(
        ["sh", script] + args,
        capture_output=True, text=True, env=env,
    )


def render(conf_dir, ssid="MeinAP", psk="geheim123", channel=6):
    with contextlib.redirect_stdout(io.StringIO()), \
            contextlib.redirect_stderr(io.StringIO()):
        rc = g.main(["--ssid", ssid, "--passphrase", psk,
                     "--channel", str(channel), "--out", conf_dir, "--force"])
    assert rc == 0, "Generator fehlgeschlagen"


def fresh_board():
    """Scratch board tree that mimics the pre-deploy state on the Odroid."""
    root = tempfile.mkdtemp(prefix="ap-board-")
    os.makedirs(os.path.join(root, "etc", "default"))
    with open(os.path.join(root, "etc", "default", "hostapd"), "w") as fh:
        fh.write('# defaults\nDAEMON_CONF=""\n')
    return root


def board_file(root, *parts):
    return os.path.join(root, *parts)


def cmdlog(root):
    p = board_file(root, "var", "log", "odroid-ap-commands.log")
    if not os.path.exists(p):
        return ""
    with open(p) as fh:
        return fh.read()


def test_dry_run_changes_nothing():
    print("test_dry_run_changes_nothing")
    root = fresh_board()
    conf = tempfile.mkdtemp(prefix="ap-conf-")
    render(conf)
    before = sorted(os.path.relpath(os.path.join(d, f), root)
                    for d, _, fs in os.walk(root) for f in fs)
    r = run(DEPLOY, ["--conf-dir", conf], root)
    check(r.returncode == 0, f"dry-run exit != 0: {r.stderr}")
    after = sorted(os.path.relpath(os.path.join(d, f), root)
                   for d, _, fs in os.walk(root) for f in fs)
    check(before == after, "dry-run hat Dateien angelegt")
    check("dry-run" in r.stdout, "dry-run nicht als solcher gekennzeichnet")
    check(not os.path.exists(board_file(root, "etc", "hostapd", "hostapd.conf")),
          "dry-run hat hostapd.conf installiert")
    shutil.rmtree(root, ignore_errors=True)
    shutil.rmtree(conf, ignore_errors=True)


def test_placeholder_rejected():
    print("test_placeholder_rejected")
    root = fresh_board()
    conf = tempfile.mkdtemp(prefix="ap-conf-")
    # A hand-written config that still carries the plan's placeholders.
    with open(os.path.join(conf, "hostapd.conf"), "w") as fh:
        fh.write("interface=wlx1\nssid=<SSID>\nchannel=6\nwpa_passphrase=<PASSWORT>\n")
    with open(os.path.join(conf, "odroid-ap.conf"), "w") as fh:
        fh.write("interface=wlx1\ndhcp-range=192.168.50.10,192.168.50.200,255.255.255.0,12h\n"
                 "dhcp-option=option:router,192.168.50.1\n")
    with open(os.path.join(conf, "odroid-ap-nat.sh"), "w") as fh:
        fh.write("#!/bin/sh\niptables -t nat -A POSTROUTING -o end0 -j MASQUERADE\n")
    r = run(DEPLOY, ["--conf-dir", conf, "--apply"], root)
    check(r.returncode != 0, "Platzhalter wurde akzeptiert")
    check("Platzhalter" in (r.stdout + r.stderr), "Platzhalter nicht gemeldet")
    check(not os.path.exists(board_file(root, "etc", "hostapd", "hostapd.conf")),
          "trotz Platzhalter installiert")
    shutil.rmtree(root, ignore_errors=True)
    shutil.rmtree(conf, ignore_errors=True)


def test_missing_values_rejected():
    print("test_missing_values_rejected")
    root = fresh_board()
    conf = tempfile.mkdtemp(prefix="ap-conf-")
    # No ssid, short password, no channel.
    with open(os.path.join(conf, "hostapd.conf"), "w") as fh:
        fh.write("interface=wlx1\nwpa_passphrase=kurz\n")
    with open(os.path.join(conf, "odroid-ap.conf"), "w") as fh:
        fh.write("interface=wlx1\ndhcp-option=option:router,192.168.50.1\n")  # no dhcp-range
    with open(os.path.join(conf, "odroid-ap-nat.sh"), "w") as fh:
        fh.write("#!/bin/sh\n")
    r = run(DEPLOY, ["--conf-dir", conf, "--apply"], root)
    check(r.returncode != 0, "fehlende Werte wurden akzeptiert")
    check(not os.path.exists(board_file(root, "etc", "hostapd", "hostapd.conf")),
          "trotz fehlender Werte installiert")
    shutil.rmtree(root, ignore_errors=True)
    shutil.rmtree(conf, ignore_errors=True)


def test_home_net_overlap_rejected():
    print("test_home_net_overlap_rejected")
    root = fresh_board()
    conf = tempfile.mkdtemp(prefix="ap-conf-")
    render(conf)
    r = run(DEPLOY, ["--conf-dir", conf, "--apply", "--ap-addr", "192.168.178.5/24"], root)
    check(r.returncode != 0, "Heimnetz-Ueberlappung akzeptiert")
    check("Heimnetz" in (r.stdout + r.stderr), "Ueberlappung nicht gemeldet")
    shutil.rmtree(root, ignore_errors=True)
    shutil.rmtree(conf, ignore_errors=True)


def test_apply_and_idempotent():
    print("test_apply_and_idempotent")
    root = fresh_board()
    conf = tempfile.mkdtemp(prefix="ap-conf-")
    render(conf)
    r1 = run(DEPLOY, ["--conf-dir", conf, "--apply"], root)
    check(r1.returncode == 0, f"apply exit != 0: {r1.stderr}")
    for rel in ("etc/hostapd/hostapd.conf", "etc/dnsmasq.d/odroid-ap.conf",
                "usr/local/sbin/odroid-ap-nat.sh", "usr/local/sbin/odroid-ap-revert.sh",
                "etc/odroid-ap.state", "etc/odroid-ap.deadman",
                "etc/systemd/system/odroid-ap-deadman.timer",
                "etc/systemd/system/odroid-ap-deadman.service"):
        check(os.path.exists(board_file(root, *rel.split("/"))), f"fehlt: {rel}")
    log1 = cmdlog(root)
    for needle in ("systemctl unmask hostapd dnsmasq",
                   "nmcli device set wlxdc627949ba79 managed no",
                   "ip addr replace 192.168.50.1/24 dev wlxdc627949ba79",
                   "odroid-ap-nat.sh", "odroid-ap-deadman.timer"):
        check(any(needle in ln for ln in log1.splitlines()), f"Kommando fehlt: {needle}")
    # Idempotent: a second apply leaves the same files (no duplicate DAEMON_CONF).
    r2 = run(DEPLOY, ["--conf-dir", conf, "--apply"], root)
    check(r2.returncode == 0, f"2. apply exit != 0: {r2.stderr}")
    with open(board_file(root, "etc", "default", "hostapd")) as fh:
        body = fh.read()
    check(body.count("DAEMON_CONF=") == 1, "DAEMON_CONF doppelt eingetragen")
    shutil.rmtree(root, ignore_errors=True)
    shutil.rmtree(conf, ignore_errors=True)


def test_deadman_reverts_unconfirmed():
    print("test_deadman_reverts_unconfirmed")
    root = fresh_board()
    conf = tempfile.mkdtemp(prefix="ap-conf-")
    render(conf)
    run(DEPLOY, ["--conf-dir", conf, "--apply"], root)
    check(os.path.exists(board_file(root, "etc", "odroid-ap.deadman")), "Marker fehlt")
    r = run(DEADMAN, [], root)
    check(r.returncode == 0, f"deadman exit != 0: {r.stderr}")
    check(not os.path.exists(board_file(root, "etc", "hostapd", "hostapd.conf")),
          "deadman hat hostapd.conf nicht entfernt")
    check(not os.path.exists(board_file(root, "etc", "odroid-ap.deadman")),
          "deadman hat den Marker nicht geloescht")
    log = cmdlog(root)
    check("systemctl mask hostapd dnsmasq" in log, "hostapd/dnsmasq nicht wieder maskiert")
    check("managed yes" in log, "Funkmodul nicht an NM zurueckgegeben")
    check("-D POSTROUTING" in log, "NAT-Regel nicht entfernt")
    # /etc/default/hostapd must be back to the original content.
    with open(board_file(root, "etc", "default", "hostapd")) as fh:
        check('DAEMON_CONF=""' in fh.read(), "hostapd-Default nicht wiederhergestellt")
    shutil.rmtree(root, ignore_errors=True)
    shutil.rmtree(conf, ignore_errors=True)


def test_confirm_prevents_revert():
    print("test_confirm_prevents_revert")
    root = fresh_board()
    conf = tempfile.mkdtemp(prefix="ap-conf-")
    render(conf)
    run(DEPLOY, ["--conf-dir", conf, "--apply"], root)
    r = run(DEPLOY, ["--confirm"], root)
    check(r.returncode == 0, f"confirm exit != 0: {r.stderr}")
    check(not os.path.exists(board_file(root, "etc", "odroid-ap.deadman")),
          "confirm hat den Marker nicht entfernt")
    check("disable odroid-ap-deadman.timer" in cmdlog(root),
          "confirm hat den Timer nicht gestoppt")
    # Dead-man now finds no marker -> it must NOT revert.
    run(DEADMAN, [], root)
    check(os.path.exists(board_file(root, "etc", "hostapd", "hostapd.conf")),
          "bestaetigter AP wurde trotzdem zurueckgenommen")
    shutil.rmtree(root, ignore_errors=True)
    shutil.rmtree(conf, ignore_errors=True)


def test_revert_restores_and_is_idempotent():
    print("test_revert_restores_and_is_idempotent")
    root = fresh_board()
    conf = tempfile.mkdtemp(prefix="ap-conf-")
    render(conf)
    logrel = os.path.join("var", "log", "odroid-ap-commands.log")
    # Snapshot of the tree, ignoring the command log (it records the very
    # commands we run and would otherwise mask real changes).
    def snapshot():
        return sorted(os.path.relpath(os.path.join(d, f), root)
                      for d, _, fs in os.walk(root) for f in fs
                      if os.path.relpath(os.path.join(d, f), root) != logrel)
    before = snapshot()
    run(DEPLOY, ["--conf-dir", conf, "--apply"], root)
    check(snapshot() != before, "apply hat nichts geaendert")
    r1 = run(REVERT, ["--apply"], root)
    check(r1.returncode == 0, f"revert exit != 0: {r1.stderr}")
    after = snapshot()
    check(after == before, f"revert nicht identisch zum Ausgangszustand: {after}")
    with open(board_file(root, "etc", "default", "hostapd")) as fh:
        check('DAEMON_CONF=""' in fh.read(), "hostapd-Default nicht zurueckgesetzt")
    # Idempotent: a second revert must also succeed and change nothing.
    r2 = run(REVERT, ["--apply"], root)
    check(r2.returncode == 0, f"2. revert exit != 0: {r2.stderr}")
    check(snapshot() == after, "2. revert hat etwas geaendert")
    shutil.rmtree(root, ignore_errors=True)
    shutil.rmtree(conf, ignore_errors=True)


def test_revert_dry_run_changes_nothing():
    print("test_revert_dry_run_changes_nothing")
    root = fresh_board()
    conf = tempfile.mkdtemp(prefix="ap-conf-")
    render(conf)
    run(DEPLOY, ["--conf-dir", conf, "--apply"], root)
    def snapshot():
        return sorted(os.path.relpath(os.path.join(d, f), root)
                      for d, _, fs in os.walk(root) for f in fs)
    before = snapshot()
    r = run(REVERT, [], root)
    check(r.returncode == 0, f"revert dry-run exit != 0: {r.stderr}")
    check(snapshot() == before, "revert dry-run hat etwas geaendert")
    check(os.path.exists(board_file(root, "etc", "hostapd", "hostapd.conf")),
          "revert dry-run hat hostapd.conf entfernt")
    shutil.rmtree(root, ignore_errors=True)
    shutil.rmtree(conf, ignore_errors=True)


def main():
    for fn in (test_dry_run_changes_nothing,
               test_placeholder_rejected,
               test_missing_values_rejected,
               test_home_net_overlap_rejected,
               test_apply_and_idempotent,
               test_deadman_reverts_unconfirmed,
               test_confirm_prevents_revert,
               test_revert_restores_and_is_idempotent,
               test_revert_dry_run_changes_nothing):
        fn()
    print(f"\n{CHECKS} checks, {FAILURES} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
