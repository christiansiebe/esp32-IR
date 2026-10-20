#!/usr/bin/env python3
"""Reine Logik, um die Kernel-Absturzaufzeichnung auf dem Odroid N2 zu verbessern.

Warum es diese Datei gibt
-------------------------
Der Oops vom 2026-10-20 (ESR `0x86000006`, Instruction Abort aus EL1) hat
**keine** Spur hinterlassen: `dmesg`, `journalctl -k`, `/var/log/kern.log` und
`/sys/fs/pstore` waren alle leer, der Reset erschien nur als
`hard-reset-unknown`. Zwei Luecken verursachen das:

  1. **Ein Oops ist kein Panic.** Ohne `panic_on_oops=1` protokolliert der
     Kernel den Oops und laeuft weiter (oder stirbt leise). `panic=10` und
     ramoops greifen dann **nie** — sie haengen beide am Panic-Pfad.
  2. **journald syncht zu langsam**, und ein harter Hang (PLL-/Spannungs-Lockup)
     erreicht den Panic-Pfad **gar nicht**. Fuer diesen Fall hilft nur ein
     Netz-Logger (netconsole), der die printk-Zeilen sofort wegschickt.

Diese Datei rendert die Konfigurationsartefakte und pruefet eine laufende
Konfiguration auf das, was noch fehlt. Sie ist **rein** (kein Board-Zugriff),
damit die Host-Tests sie abdecken koennen.
"""
from __future__ import annotations

import json
import sys

# ---------------------------------------------------------------------------
# 1. Kernel-Sysctls: einen Oops/Hang in einen Panic verwandeln
# ---------------------------------------------------------------------------
# Der Panic ist das *Ziel*, nicht das Problem: nur der Panic-Pfad schreibt
# ramoops und zieht `panic=10` (Reboot). Deshalb ist `panic_on_oops=1` der
# wichtigste Schalter — ohne ihn ist die naechste Oops-Spur wieder weg.
#
# `hung_task_panic`/`softlockup_panic` fangen einen *stillen* Hang als Panic
# ab (Watchdog-Alternative in Software). Die Zeitlimits sind bewusst
# konservativ: 120 s hung-task, 20 s softlockup — unter HA+Docker-Last auf
# 4-6 Kernen sicher, ohne bei normaler Last zu feuern.
SYSCTL_SETTINGS = {
    "kernel.panic_on_oops": ("1", "einen Oops zum Panic machen -> ramoops + panic=10 greifen"),
    "kernel.panic": ("10", "nach einem Panic 10 s spaeter rebooten (Sicherheitsnetz)"),
    "kernel.hung_task_panic": ("1", "stillen Hang als Panic behandeln"),
    "kernel.hung_task_timeout_secs": ("120", "Hang-Erkennung erst nach 120 s (kein Fehlalarm)"),
    "kernel.softlockup_panic": ("1", "weichen Lockup als Panic behandeln"),
    "kernel.panic_on_warn": ("0", "WARNungen NICHT zum Panic machen (zu viel Rauschen)"),
    "vm.panic_on_oom": ("0", "OOM-Killer soll NICHT rebooten (nur den Prozess toeten)"),
}

# ---------------------------------------------------------------------------
# 2. journald: Kernel-Log frueher auf Platte zwingen
# ---------------------------------------------------------------------------
# Default ist Storage=auto + SyncIntervalSec=5min. 5 min Sync heisst: bis zu
# 5 min Kernel-Log liegen nur im RAM und sind bei einem harten Reset weg.
JOURNALD_SETTINGS = {
    "Storage": ("persistent", "Journal ueber Reboots behalten (/var/log/journal)"),
    "SystemMaxUse": ("64M", "groesserer Deckel als die 20M-Default (mehr Historie)"),
    "RuntimeMaxUse": ("32M", "ausreichend RAM-Puffer fuer den laufenden Boot"),
    "SyncIntervalSec": ("1s", "schnell auf Platte flushen (Default 5min ist zu traege)"),
    "ForwardToSyslog": ("yes", "zusaetzlich nach /var/log/kern.log (rsyslog laeuft)"),
}

# ---------------------------------------------------------------------------
# 3. Kernel-Cmdline: den Panic-Pfad schon im Kernel scharf stellen
# ---------------------------------------------------------------------------
# `panic_on_oops=1` steht bewusst doppelt (Sysctl + Cmdline): die Cmdline gilt
# ab dem ersten Kernel-Zeitpunkt, das Sysctl erst, wenn systemd die
# sysctl.d-Datei geladen hat. Beide Wege zusammen schliessen die Luecke
# zwischen Kernel-Start und Sysctl-Laden.
KERNEL_EXTRAARGS = ("panic_on_oops=1", "hung_task_panic=1")

# ---------------------------------------------------------------------------
# 4. netconsole: den Kernel-Log aus dem Board heraus auf einen anderen Host
# ---------------------------------------------------------------------------
# Der einzige Mechanismus, der auch einen HARTEN Hang (PLL/Spannung) sichtbar
# macht: printk geht sofort als UDP raus, unabhaengig von journald und von
# einem spaeteren Panic. Braucht einen zweiten Rechner als Empfaenger.
NETCONSOLE_MODULE_OPTS = (
    "netconsole=6666@%SRC_IP%/%IFACE%,6666@%DST_IP%/%DST_MAC%",
)


def render_sysctl_dropin() -> str:
    """Erzeugt eine /etc/sysctl.d-Datei, die einen Oops zum Panic macht."""
    lines = [
        "# Kernel-Absturzaufzeichnung (Odroid N2) - erzeugt von crash_capture.py",
        "# Ein Oops MUSS ein Panic werden, sonst greifen ramoops und panic=10 nicht.",
    ]
    # sysctl.d kennt KEINE Inline-Kommentare (der Wert wuerde sonst als
    # '1   # ...' geparst) -> die Begruendung steht in einer eigenen Zeile.
    for key, (value, why) in SYSCTL_SETTINGS.items():
        lines.append(f"# {why}")
        lines.append(f"{key} = {value}")
    return "\n".join(lines) + "\n"


def render_journald_dropin() -> str:
    """Erzeugt eine /etc/systemd/journald.conf.d-Datei."""
    lines = [
        "# Kernel-Log frueher sichern (Odroid N2) - erzeugt von crash_capture.py",
        "[Journal]",
    ]
    # systemd-Config kennt keine Inline-Kommentare -> eigene Kommentarzeile.
    for key, (value, why) in JOURNALD_SETTINGS.items():
        lines.append(f"# {why}")
        lines.append(f"{key}={value}")
    return "\n".join(lines) + "\n"


def merge_extraargs(current: str) -> str:
    """Gibt `extraargs` zurueck, um die fehlenden Crash-Token ergaenzt.

    `current` ist der Wert rechts vom `=` in `/boot/armbianEnv.txt`
    (oder "" wenn die Zeile fehlt). Vorhandene Token bleiben, Reihenfolge
    erhalten, keine Duplikate.
    """
    tokens = current.split()
    for token in KERNEL_EXTRAARGS:
        if token not in tokens:
            tokens.append(token)
    return " ".join(tokens)


def render_netconsole_module(src_ip: str, iface: str, dst_ip: str, dst_mac: str) -> str:
    """Erzeugt /etc/modprobe.d/netconsole.conf fuer einen Netz-Logger.

    Leere Werte sind erlaubt: dann bleibt der Platzhalter stehen und der
    Nutzer traegt ihn nach. netconsole ist bewusst OPTIONAL — es braucht einen
    zweiten Rechner, der die UDP-Pakete annimmt.
    """
    opts = NETCONSOLE_MODULE_OPTS[0]
    opts = (opts.replace("%SRC_IP%", src_ip or "<src-ip>")
                .replace("%IFACE%", iface or "<iface>")
                .replace("%DST_IP%", dst_ip or "<dst-ip>")
                .replace("%DST_MAC%", dst_mac or "<dst-mac>"))
    return (
        "# Kernel-Log per UDP auf einen zweiten Host (Odroid N2)\n"
        "# Auch ein harter Hang wird so sichtbar. Erzeugt von crash_capture.py.\n"
        f"options netconsole {opts}\n"
    )


# ---------------------------------------------------------------------------
# Audit: was fehlt der laufenden Konfiguration noch?
# ---------------------------------------------------------------------------
# `state` ist ein dict mit dem, was der Collector am Board gelesen hat:
#   sysctl:      {"kernel.panic_on_oops": "1", ...}   (aus /proc/sys)
#   journald:    {"Storage": "persistent", ...}       (aus journald.conf*)
#   extraargs:   "cpufreq.default_governor=performance panic_on_oops=1"
#   pstore_present: bool   (ramoops-Dump vorhanden/Region reserviert)
#   ramoops_loaded: bool   (lsmod)
# Die Funktion ist rein und liefert eine Liste von Luecken als dicts.


def _num(value: str) -> float | None:
    try:
        return float(str(value).strip())
    except (TypeError, ValueError):
        return None


_DURATION_UNITS = {"us": 1e-6, "ms": 1e-3, "s": 1.0, "sec": 1.0,
                   "min": 60.0, "m": 60.0, "h": 3600.0, "d": 86400.0}


def _seconds(value: str) -> float | None:
    """Parst systemd-Zeitangaben ('1s', '5min', '2h', '500ms') in Sekunden."""
    text = str(value).strip().lower()
    num = _num(text)
    if num is not None:
        return num
    for unit in sorted(_DURATION_UNITS, key=len, reverse=True):
        if text.endswith(unit):
            base = _num(text[: -len(unit)])
            if base is not None:
                return base * _DURATION_UNITS[unit]
    return None


def audit(state: dict) -> list[dict]:
    """Meldet jede Aufzeichnungs-Luecke als {kind, name, detail, fix}."""
    gaps: list[dict] = []
    sysctl = state.get("sysctl", {}) or {}
    journald = state.get("journald", {}) or {}
    extraargs = state.get("extraargs", "") or ""

    def want_sysctl(key: str) -> None:
        expected = SYSCTL_SETTINGS[key][0]
        actual = sysctl.get(key)
        if str(actual).strip() != expected:
            gaps.append({
                "kind": "sysctl",
                "name": key,
                "detail": f"ist {actual!r}, sollte {expected!r} sein",
                "fix": f"{key}={expected}",
            })

    for key in SYSCTL_SETTINGS:
        want_sysctl(key)

    # panic=10 darf nicht versehentlich 0 sein (dann kein Reboot nach Panic).
    if _num(sysctl.get("kernel.panic", "10")) == 0:
        gaps.append({
            "kind": "sysctl", "name": "kernel.panic",
            "detail": "0 -> kein Reboot nach einem Panic",
            "fix": "kernel.panic=10",
        })

    for key, (expected, _why) in JOURNALD_SETTINGS.items():
        actual = journald.get(key)
        if actual is None:
            gaps.append({
                "kind": "journald", "name": key,
                "detail": "nicht gesetzt (Default)",
                "fix": f"{key}={expected}",
            })
        elif key == "SyncIntervalSec":
            # kleiner ist besser: nur melden, wenn langsamer als gewuenscht
            want_s = _seconds(expected)
            got_s = _seconds(actual)
            if got_s is None or (want_s is not None and got_s > want_s):
                gaps.append({
                    "kind": "journald", "name": key,
                    "detail": f"{actual} ist traeger als {expected}",
                    "fix": f"{key}={expected}",
                })
        elif str(actual).strip().lower() != expected.lower():
            gaps.append({
                "kind": "journald", "name": key,
                "detail": f"ist {actual!r}, sollte {expected!r} sein",
                "fix": f"{key}={expected}",
            })

    for token in KERNEL_EXTRAARGS:
        if token not in extraargs.split():
            gaps.append({
                "kind": "cmdline", "name": token,
                "detail": "fehlt in /boot/armbianEnv.txt extraargs",
                "fix": f"extraargs += {token}",
            })

    if not state.get("ramoops_loaded"):
        gaps.append({
            "kind": "ramoops", "name": "ramoops",
            "detail": "Modul nicht geladen -> Panic-Dmesg geht verloren",
            "fix": "deploy_ramoops.sh (PR #45)",
        })

    return gaps


def format_audit(gaps: list[dict]) -> str:
    """Menschenlesbare Zusammenfassung der Luecken."""
    if not gaps:
        return "OK: alle Aufzeichnungs-Mechanismen sind scharf."
    out = [f"{len(gaps)} Aufzeichnungs-Luecke(n):"]
    for gap in gaps:
        out.append(f"  [{gap['kind']:9}] {gap['name']}: {gap['detail']}")
        out.append(f"              Fix: {gap['fix']}")
    return "\n".join(out)


def collect_state(sysctl_keys=None) -> dict:
    """Liest die laufende Konfiguration vom Board (nur auf dem Board sinnvoll).

    Fehlende Pfade werden zu None -> `audit` meldet sie dann als Luecke.
    """
    import os
    import subprocess

    sysctl = {}
    for key in SYSCTL_SETTINGS:
        path = "/proc/sys/" + key.replace(".", "/")
        try:
            with open(path) as fh:
                sysctl[key] = fh.read().strip()
        except OSError:
            sysctl[key] = None

    journald = {}
    conf = "/etc/systemd/journald.conf"
    if os.path.exists(conf):
        with open(conf) as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, _, v = line.partition("=")
                    journald[k.strip()] = v.strip()
    dropin_dir = "/etc/systemd/journald.conf.d"
    if os.path.isdir(dropin_dir):
        for name in sorted(os.listdir(dropin_dir)):
            if not name.endswith(".conf"):
                continue
            with open(os.path.join(dropin_dir, name)) as fh:
                for line in fh:
                    line = line.strip()
                    if line and not line.startswith("#") and "=" in line:
                        k, _, v = line.partition("=")
                        journald[k.strip()] = v.strip()

    extraargs = ""
    env = "/boot/armbianEnv.txt"
    if os.path.exists(env):
        with open(env) as fh:
            for line in fh:
                if line.startswith("extraargs="):
                    extraargs = line.split("=", 1)[1].strip()

    ramoops_loaded = False
    try:
        mods = subprocess.run(["lsmod"], capture_output=True, text=True, timeout=5)
        ramoops_loaded = "ramoops" in mods.stdout
    except Exception:
        pass

    return {
        "sysctl": sysctl,
        "journald": journald,
        "extraargs": extraargs,
        "ramoops_loaded": ramoops_loaded,
        "pstore_present": os.path.isdir("/sys/fs/pstore"),
    }


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "render-sysctl":
        sys.stdout.write(render_sysctl_dropin())
    elif cmd == "render-journald":
        sys.stdout.write(render_journald_dropin())
    elif cmd == "render-extraargs":
        current = argv[2] if len(argv) > 2 else ""
        print(merge_extraargs(current))
    elif cmd == "render-netconsole":
        parts = (argv[2:6] + ["", "", "", ""])[:4]
        sys.stdout.write(render_netconsole_module(*parts))
    elif cmd == "audit":
        state = collect_state()
        print(format_audit(audit(state)))
    elif cmd == "audit-json":
        # nimmt den Zustand als JSON auf stdin (fuer Tests/andere Collector)
        print(format_audit(audit(json.load(sys.stdin))))
    else:
        print(f"unbekannt: {cmd}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
