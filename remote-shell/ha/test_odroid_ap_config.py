"""Host tests for remote-shell/ha/odroid_ap_config.py (pure, no board).

The generator renders the hostapd/dnsmasq/NAT files described in
ODROID-LAN-AP-KAMERA.md. It must never emit a config that would take the AP
off the air or fight the home network, so the checks focus on:

  * rejecting placeholders / short passwords (no guessed secrets),
  * band <-> channel agreement (2g vs 5g, DFS flag),
  * refusing an AP subnet that overlaps the home network 192.168.178.0/24,
  * dry-run by default and no clobbering without --force.
"""

import contextlib
import io
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import odroid_ap_config as g  # noqa: E402


def quiet_main(argv):
    """Run the generator with its stdout/stderr suppressed (keep test output clean)."""
    with contextlib.redirect_stdout(io.StringIO()), \
            contextlib.redirect_stderr(io.StringIO()):
        return g.main(argv)


CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def expect_error(fn, needle, msg):
    try:
        fn()
    except g.ConfigError as exc:
        check(needle in str(exc), f"{msg}: unerwartete Meldung: {exc}")
        return
    check(False, f"{msg}: kein ConfigError geworfen")


def test_ssid_validation():
    print("test_ssid_validation")
    check(g.validate_ssid("Wohnzimmer 5 Ghz") == "Wohnzimmer 5 Ghz", "gute SSID")
    expect_error(lambda: g.validate_ssid("<SSID>"), "Platzhalter", "Platzhalter")
    expect_error(lambda: g.validate_ssid(""), "Platzhalter", "leer")
    expect_error(lambda: g.validate_ssid("x" * 33), "32 Bytes", "zu lang")
    expect_error(lambda: g.validate_ssid("bad\nssid"), "Steuerzeichen", "Steuerzeichen")


def test_passphrase_validation():
    print("test_passphrase_validation")
    check(g.validate_passphrase("abcdefgh") == "abcdefgh", "8 Zeichen ok")
    expect_error(lambda: g.validate_passphrase("kurz"), "8..63", "zu kurz")
    expect_error(lambda: g.validate_passphrase("x" * 64), "8..63", "zu lang")
    expect_error(lambda: g.validate_passphrase("CHANGEME"), "Platzhalter", "Platzhalter")
    expect_error(lambda: g.validate_passphrase("mit\x00null"), "ASCII", "Nicht-ASCII")


def test_channel_band():
    print("test_channel_band")
    check(g.validate_channel("2g", 6) == 6, "2g Kanal 6")
    check(g.validate_channel("5g", 36) == 36, "5g Kanal 36")
    expect_error(lambda: g.validate_channel("2g", 36), "nicht erlaubt", "2g/36")
    expect_error(lambda: g.validate_channel("5g", 14), "nicht erlaubt", "5g/14")
    check(g.is_dfs_channel("5g", 52), "52 ist DFS")
    check(not g.is_dfs_channel("5g", 36), "36 ist nicht DFS")
    check(not g.is_dfs_channel("2g", 6), "2g hat kein DFS")


def test_subnet_overlap():
    print("test_subnet_overlap")
    check(str(g.validate_ap_subnet("192.168.50.0/24")) == "192.168.50.0/24", "ok")
    expect_error(lambda: g.validate_ap_subnet("192.168.178.0/24"),
                 "ueberlappt", "Heimnetz exakt")
    expect_error(lambda: g.validate_ap_subnet("192.168.178.128/25"),
                 "ueberlappt", "Heimnetz Teilbereich")
    expect_error(lambda: g.validate_ap_subnet("nonsense"),
                 "ungueltig", "Muell")


def test_hostapd_render():
    print("test_hostapd_render")
    out = g.render_hostapd("MeinAP", "geheim123", "2g", 6)
    for needle in ("interface=wlxdc627949ba79", "ssid=MeinAP", "hw_mode=g",
                   "channel=6", "wpa=2", "wpa_passphrase=geheim123",
                   "rsn_pairwise=CCMP", "country_code=DE"):
        check(needle in out, f"2g fehlt: {needle}")
    check("ieee80211h" not in out, "2g darf kein DFS-Flag haben")

    out5 = g.render_hostapd("MeinAP", "geheim123", "5g", 36)
    check("hw_mode=a" in out5, "5g hw_mode")
    check("ieee80211ac=1" in out5, "5g ac")
    check("ieee80211h" not in out5, "nicht-DFS 36 ohne ieee80211h")

    out5dfs = g.render_hostapd("MeinAP", "geheim123", "5g", 52)
    check("ieee80211h=1" in out5dfs, "DFS-Kanal braucht ieee80211h")


def test_dnsmasq_render():
    print("test_dnsmasq_render")
    out = g.render_dnsmasq("192.168.50.0/24")
    check("interface=wlxdc627949ba79" in out, "Interface")
    check("bind-interfaces" in out, "bind-interfaces")
    check("dhcp-range=192.168.50.10,192.168.50.200,255.255.255.0,12h" in out,
          f"dhcp-range: {out}")
    check("dhcp-option=option:router,192.168.50.1" in out, "gateway")
    check("server=192.168.178.1" in out, "Upstream FRITZ!Box")
    check("192.168.178" not in out.split("server=")[0],
          "DHCP-Bereich darf nicht im Heimnetz liegen")


def test_nat_render():
    print("test_nat_render")
    out = g.render_nat()
    check("UP=end0" in out, "Uplink end0")
    check("AP=wlxdc627949ba79" in out, "AP iface")
    check("MASQUERADE" in out, "NAT")
    check(out.count("-C ") >= 3 and out.count("-A ") >= 3, "idempotent -C/-A")


def test_dry_run_and_no_clobber():
    print("test_dry_run_and_no_clobber")
    # Dry-run writes nothing.
    with tempfile.TemporaryDirectory() as tmp:
        rc = quiet_main(["--ssid", "MeinAP", "--passphrase", "geheim123",
                     "--band", "2g", "--channel", "6"])
        check(rc == 0, "dry-run exit 0")
        check(os.listdir(tmp) == [], "dry-run schreibt nichts")

    with tempfile.TemporaryDirectory() as tmp:
        rc = quiet_main(["--ssid", "MeinAP", "--passphrase", "geheim123",
                     "--band", "2g", "--channel", "6", "--out", tmp])
        check(rc == 0, "write exit 0")
        names = sorted(os.listdir(tmp))
        check(names == ["hostapd.conf", "odroid-ap-nat.sh", "odroid-ap.conf"],
              f"Dateien: {names}")
        nat = os.path.join(tmp, "odroid-ap-nat.sh")
        check(os.access(nat, os.X_OK), "NAT-Skript ist ausfuehrbar")
        # Second run without --force must not clobber.
        with open(os.path.join(tmp, "hostapd.conf"), "a") as fh:
            fh.write("# sentinel\n")
        rc = quiet_main(["--ssid", "Anders", "--passphrase", "geheim456",
                     "--band", "2g", "--channel", "6", "--out", tmp])
        check(rc == 0, "re-run exit 0")
        with open(os.path.join(tmp, "hostapd.conf")) as fh:
            check("# sentinel" in fh.read(), "ohne --force nicht ueberschrieben")


def test_missing_secrets_exit_2():
    print("test_missing_secrets_exit_2")
    check(quiet_main(["--ssid", "MeinAP", "--out", "/tmp/does-not-matter"]) == 2,
          "fehlendes Passwort -> exit 2")


def main():
    for fn in (test_ssid_validation, test_passphrase_validation,
               test_channel_band, test_subnet_overlap, test_hostapd_render,
               test_dnsmasq_render, test_nat_render,
               test_dry_run_and_no_clobber, test_missing_secrets_exit_2):
        fn()
    print(f"\n{CHECKS} checks, {FAILURES} failures")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
