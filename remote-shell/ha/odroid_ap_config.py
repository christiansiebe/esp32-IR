#!/usr/bin/env python3
"""Generator for the Odroid AP config (hostapd + dnsmasq + NAT).

Companion to ``ODROID-LAN-AP-KAMERA.md``. Pure string building, no board
access, no ``remote-shell`` channel: it only renders the files the plan
describes, so the values can be reviewed and diffed before anything touches
the Odroid.

Default is **dry-run** (print only). ``--out DIR`` writes the files; existing
files are kept unless ``--force`` is given, so an accidental run cannot
clobber a working configuration.

The validator refuses placeholders (``<SSID>``, ``CHANGEME`` …) and refuses an
AP subnet that overlaps the home network from the recon (192.168.178.0/24):
that would make the AP's DHCP hand out addresses the FRITZ!Box already owns.

Usage:
  python3 odroid_ap_config.py --ssid "Wohnzimmer 5 Ghz" --passphrase "secret"
  python3 odroid_ap_config.py --ssid ... --passphrase ... --band 5g \
      --channel 36 --out /tmp/odroid-ap
"""

import argparse
import ipaddress
import os
import re
import sys

# Values fixed by the recon (ODROID-NETZ-RECON.md). Not guesses.
AP_IFACE = "wlxdc627949ba79"
UP_IFACE = "end0"
HOME_NET = "192.168.178.0/24"
HOME_DNS = "192.168.178.1"

BAND_2G_CHANNELS = tuple(range(1, 14))
# 5 GHz: non-DFS (36-48, 149-165) and DFS (52-64, 100-144).
BAND_5G_NON_DFS = (36, 40, 44, 48, 149, 153, 157, 161, 165)
BAND_5G_DFS = tuple(range(52, 65, 4)) + tuple(range(100, 145, 4))
BAND_5G_CHANNELS = BAND_5G_NON_DFS + BAND_5G_DFS

# WPA2-PSK: 8..63 printable ASCII characters.
PASSPHRASE_MIN = 8
PASSPHRASE_MAX = 63
SSID_MAX_BYTES = 32

PLACEHOLDERS = {
    "",
    "ssid",
    "passwort",
    "password",
    "changeme",
    "change-me",
    "<ssid>",
    "<passwort>",
    "<password>",
    "<kanal>",
    "<channel>",
}


class ConfigError(ValueError):
    """A user-supplied value cannot be used as-is."""


def _is_placeholder(value):
    return value.strip().lower() in PLACEHOLDERS


def validate_ssid(ssid):
    if not isinstance(ssid, str) or _is_placeholder(ssid):
        raise ConfigError("SSID fehlt oder ist ein Platzhalter")
    if len(ssid.encode("utf-8")) > SSID_MAX_BYTES:
        raise ConfigError("SSID laenger als 32 Bytes")
    if any(ord(c) < 0x20 for c in ssid):
        raise ConfigError("SSID enthaelt Steuerzeichen")
    return ssid


def validate_passphrase(passphrase):
    if not isinstance(passphrase, str) or _is_placeholder(passphrase):
        raise ConfigError("Passwort fehlt oder ist ein Platzhalter")
    if not (PASSPHRASE_MIN <= len(passphrase) <= PASSPHRASE_MAX):
        raise ConfigError(
            f"WPA2-Passwort muss {PASSPHRASE_MIN}..{PASSPHRASE_MAX} Zeichen haben"
        )
    if any(ord(c) < 0x20 or ord(c) > 0x7E for c in passphrase):
        raise ConfigError("Passwort muss druckbares ASCII sein")
    return passphrase


def validate_channel(band, channel):
    if band == "2g":
        allowed = BAND_2G_CHANNELS
    elif band == "5g":
        allowed = BAND_5G_CHANNELS
    else:
        raise ConfigError(f"unbekanntes Band: {band!r} (erlaubt: 2g, 5g)")
    if channel not in allowed:
        raise ConfigError(
            f"Kanal {channel} ist auf {band} nicht erlaubt "
            f"(erlaubt: {allowed[0]}..{allowed[-1]})"
        )
    return channel


def is_dfs_channel(band, channel):
    return band == "5g" and channel in BAND_5G_DFS


def validate_ap_subnet(ap_subnet, home_net=HOME_NET):
    """Reject an AP subnet that overlaps (or equals) the home network."""
    try:
        ap = ipaddress.ip_network(ap_subnet, strict=False)
    except ValueError as exc:
        raise ConfigError(f"AP-Subnetz ungueltig: {ap_subnet!r} ({exc})") from exc
    home = ipaddress.ip_network(home_net, strict=False)
    if ap.overlaps(home):
        raise ConfigError(
            f"AP-Subnetz {ap} ueberlappt das Heimnetz {home} "
            "(DHCP wuerde Adressen doppelt vergeben)"
        )
    return ap


def render_hostapd(ssid, passphrase, band="2g", channel=6, iface=AP_IFACE):
    validate_ssid(ssid)
    validate_passphrase(passphrase)
    validate_channel(band, channel)

    lines = [
        "# Erzeugt von odroid_ap_config.py - Vorlage aus ODROID-LAN-AP-KAMERA.md",
        f"interface={iface}",
        "driver=nl80211",
        f"ssid={ssid}",
    ]
    if band == "2g":
        lines += [
            "hw_mode=g",
            f"channel={channel}",
            "ieee80211n=1",
        ]
    else:
        lines += [
            "hw_mode=a",
            f"channel={channel}",
            "ieee80211ac=1",
        ]
        if is_dfs_channel(band, channel):
            # DFS: radar detection + channel availability check on start.
            lines += ["ieee80211h=1"]
    lines += [
        "country_code=DE",
        "ieee80211d=1",
        "wmm_enabled=1",
        "auth_algs=1",
        "macaddr_acl=0",
        "ignore_broadcast_ssid=0",
        "wpa=2",
        f"wpa_passphrase={passphrase}",
        "wpa_key_mgmt=WPA-PSK",
        "rsn_pairwise=CCMP",
    ]
    return "\n".join(lines) + "\n"


def render_dnsmasq(ap_subnet, iface=AP_IFACE, home_dns=HOME_DNS):
    ap = validate_ap_subnet(ap_subnet)
    hosts = list(ap.hosts())
    if len(hosts) < 3:
        raise ConfigError(f"AP-Subnetz {ap} ist zu klein fuer DHCP")
    gateway = hosts[0]
    dhcp_low, dhcp_high = hosts[9], hosts[min(len(hosts) - 1, 199)]
    lines = [
        "# Erzeugt von odroid_ap_config.py - Vorlage aus ODROID-LAN-AP-KAMERA.md",
        "# DHCP/DNS nur auf dem AP-Interface.",
        f"interface={iface}",
        "bind-interfaces",
        "except-interface=lo",
        "",
        "# Adressbereich des AP-Subnetzes (NICHT das Heimnetz).",
        f"dhcp-range={dhcp_low},{dhcp_high},{ap.netmask},12h",
        f"dhcp-option=option:router,{gateway}",
        f"dhcp-option=option:dns-server,{gateway}",
        "dhcp-authoritative",
        "",
        "# Upstream-DNS = FRITZ!Box (aus Recon).",
        f"server={home_dns}",
        "domain-needed",
        "bogus-priv",
        "log-dhcp",
    ]
    return "\n".join(lines) + "\n"


def render_nat(ap_iface=AP_IFACE, up_iface=UP_IFACE):
    return (
        "#!/bin/sh\n"
        "# Erzeugt von odroid_ap_config.py - NAT/Forwarding fuer den AP auf "
        f"{up_iface}.\n"
        "# net.ipv4.ip_forward=1 ist laut Recon bereits gesetzt.\n"
        "set -eu\n"
        f'UP={up_iface}\n'
        f'AP={ap_iface}\n'
        'iptables -t nat -C POSTROUTING -o "$UP" -j MASQUERADE 2>/dev/null || '
        'iptables -t nat -A POSTROUTING -o "$UP" -j MASQUERADE\n'
        'iptables -C FORWARD -i "$AP" -o "$UP" -j ACCEPT 2>/dev/null || '
        'iptables -A FORWARD -i "$AP" -o "$UP" -j ACCEPT\n'
        'iptables -C FORWARD -i "$UP" -o "$AP" -m state --state '
        'RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || '
        'iptables -A FORWARD -i "$UP" -o "$AP" -m state --state '
        'RELATED,ESTABLISHED -j ACCEPT\n'
    )


def build_files(ssid, passphrase, band="2g", channel=6, ap_subnet="192.168.50.0/24"):
    """Return {relative_path: content} for the whole AP configuration."""
    return {
        "hostapd.conf": render_hostapd(ssid, passphrase, band, channel),
        "odroid-ap.conf": render_dnsmasq(ap_subnet),
        "odroid-ap-nat.sh": render_nat(),
    }


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ssid", help="WLAN-SSID des AP (Pflicht beim Schreiben)")
    parser.add_argument("--passphrase", help="WPA2-Passwort (Pflicht beim Schreiben)")
    parser.add_argument("--band", choices=("2g", "5g"), default="2g")
    parser.add_argument("--channel", type=int, default=None,
                        help="Funkkanal (Default: 6 fuer 2g, 36 fuer 5g)")
    parser.add_argument("--ap-subnet", default="192.168.50.0/24")
    parser.add_argument("--out", help="Zielverzeichnis; ohne Angabe nur Dry-Run")
    parser.add_argument("--force", action="store_true",
                        help="vorhandene Dateien ueberschreiben")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if args.channel is None:
        args.channel = 6 if args.band == "2g" else 36
    if not args.ssid or not args.passphrase:
        print("FEHLER: --ssid und --passphrase sind erforderlich "
              "(keine geratenen Werte).", file=sys.stderr)
        return 2
    try:
        files = build_files(args.ssid, args.passphrase, args.band,
                            args.channel, args.ap_subnet)
    except ConfigError as exc:
        print(f"FEHLER: {exc}", file=sys.stderr)
        return 2

    if not args.out:
        for name, content in files.items():
            print(f"# ===== {name} =====")
            print(content, end="")
        print("# (Dry-Run - nichts geschrieben. Mit --out DIR schreiben.)")
        return 0

    os.makedirs(args.out, exist_ok=True)
    for name, content in files.items():
        path = os.path.join(args.out, name)
        if os.path.exists(path) and not args.force:
            print(f"SKIP (existiert, --force fehlt): {path}", file=sys.stderr)
            continue
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(content)
        if name.endswith(".sh"):
            os.chmod(path, 0o755)
        print(f"geschrieben: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
