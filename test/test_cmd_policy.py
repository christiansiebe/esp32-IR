#!/usr/bin/env python3
"""Host-Test fuer die Agent-Policy (remote-shell/cmd_policy.py).

Deckt ab: Allowlist (fail closed, voll verankert), Pfad-Einsperrung inkl.
Symlink-Ausbruch, @read/@write-Roundtrip und Fehlerfaelle.
"""
import base64
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "remote-shell"))

import cmd_policy as p

checks = failures = 0


def check(cond, label):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {label}")


# --- Allowlist -----------------------------------------------------------
pats = p.load_allowlist("# kommentar\n\nhostname\nid\n")
check(len(pats) == 2, "Kommentare/Leerzeilen ignoriert")
check(p.check_command("hostname", pats)[0], "exakt erlaubt")
check(not p.check_command("hostname; rm -rf /", pats)[0], "Zusatz abgelehnt (fullmatch)")
check(not p.check_command("hostname -f", pats)[0], "Argument abgelehnt")
check(not p.check_command("", pats)[0], "leer abgelehnt")
check(not p.check_command("hostname", [])[0], "leere Allowlist = fail closed")

# --- Pfad-Einsperrung ----------------------------------------------------
with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as outside:
    check(p.resolve_scoped(root, "a/b.txt").startswith(os.path.realpath(root)),
          "relativer Pfad erlaubt")
    check(p.resolve_scoped(root, "../x") is None, ".. abgelehnt")
    check(p.resolve_scoped(root, "/etc/passwd") is None, "absolut abgelehnt")
    check(p.resolve_scoped(root, "") is None, "leer abgelehnt")
    link = os.path.join(root, "link")
    os.symlink(outside, link)
    check(p.resolve_scoped(root, "link/secret") is None, "Symlink-Ausbruch abgelehnt")

    # --- @write / @read --------------------------------------------------
    blob = base64.b64encode(b"hallo odroid\n").decode()
    r = p.file_op(f"@write sub/test.txt {blob}", root)
    check(r["code"] == 0, "@write ok")
    check(open(os.path.join(root, "sub", "test.txt"), "rb").read() == b"hallo odroid\n",
          "@write Inhalt")
    r = p.file_op("@read sub/test.txt", root)
    check(r["code"] == 0 and r["out"] == "hallo odroid\n", "@read Roundtrip")
    check(p.file_op("@read missing.txt", root)["code"] == 1, "@read fehlende Datei")
    check(p.file_op("@write ../escape.txt aGk=", root)["code"] == 126, "@write Ausbruch")
    check(p.file_op("@write x.txt kein-base64!", root)["code"] == 1, "@write bad base64")
    check(p.file_op("hostname", root)["code"] == 126, "kein Datei-Verb")
    check(os.path.exists(os.path.join(outside, "secret")) is False, "nichts nach aussen")

print(f"cmd_policy: {checks} Checks, {failures} Fehler")
sys.exit(1 if failures else 0)
