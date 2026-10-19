#!/usr/bin/env python3
"""Reine Policy fuer den Odroid-Agenten — ohne I/O, host-testbar.

Der Kanal selbst bleibt unveraendert: weiterhin ein Text-Frame
`{"type":"cmd","id":...,"cmd":"..."}`. Dieses Modul entscheidet nur, ob ein
Befehl ausgefuehrt werden darf, und loest die beiden Datei-Verben auf:

    @read  <relativer Pfad>             Datei lesen
    @write <relativer Pfad> <base64>    Datei schreiben

Beide Verben sind auf `root` (Default ~/odroid-channel-test) eingesperrt:
absolute Pfade, `..` und Symlinks, die aus dem Root herausfuehren, werden
abgelehnt. Ausserhalb der Verben gilt eine optionale Allowlist aus
verankerten Regexen (eine pro Zeile, `#` = Kommentar); ohne Allowlist wird
nichts ausgefuehrt (fail closed), sobald der Agent eingeschraenkt laeuft.
"""
import base64
import binascii
import os
import re

READ = "@read"
WRITE = "@write"
MAX_FILE_BYTES = 1024 * 1024


def load_allowlist(text):
    """Allowlist-Text -> Liste kompilierter, voll verankerter Regexe."""
    patterns = []
    for line in (text or "").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        patterns.append(re.compile(line))
    return patterns


def check_command(cmd, patterns):
    """(ok, grund) fuer einen Shell-Befehl gegen die Allowlist."""
    cmd = (cmd or "").strip()
    if not cmd:
        return False, "leerer Befehl"
    if not patterns:
        return False, "keine Allowlist geladen"
    for pattern in patterns:
        if pattern.fullmatch(cmd):
            return True, "ok"
    return False, "nicht in der Allowlist"


def resolve_scoped(root, rel):
    """Relativer Pfad -> absoluter Pfad innerhalb von root, sonst None."""
    if not rel:
        return None
    rel = rel.strip()
    if os.path.isabs(rel) or "\x00" in rel:
        return None
    root = os.path.realpath(os.path.expanduser(root))
    target = os.path.realpath(os.path.join(root, rel))
    if target != root and not target.startswith(root + os.sep):
        return None
    return target


def parse_file_op(cmd):
    """Frame-Text -> (op, rel, payload) oder (None, None, None)."""
    parts = (cmd or "").strip().split(None, 2)
    if not parts or parts[0] not in (READ, WRITE):
        return None, None, None
    if len(parts) < 2:
        return parts[0], None, None
    payload = parts[2] if len(parts) > 2 else ""
    return parts[0], parts[1], payload


def file_op(cmd, root):
    """Fuehrt @read/@write aus. Rueckgabe: dict(code, out)."""
    op, rel, payload = parse_file_op(cmd)
    if op is None:
        return {"code": 126, "out": "kein Datei-Verb"}
    target = resolve_scoped(root, rel)
    if target is None:
        return {"code": 126, "out": f"ABGELEHNT: Pfad ausserhalb von {root}"}
    if op == READ:
        try:
            with open(target, "rb") as fh:
                data = fh.read(MAX_FILE_BYTES + 1)
        except OSError as exc:
            return {"code": 1, "out": f"LESEFEHLER: {exc}"}
        if len(data) > MAX_FILE_BYTES:
            return {"code": 1, "out": f"LESEFEHLER: Datei > {MAX_FILE_BYTES} B"}
        return {"code": 0, "out": data.decode("utf-8", "replace")}
    try:
        data = base64.b64decode(payload.encode("ascii"), validate=True)
    except (binascii.Error, UnicodeEncodeError):
        return {"code": 1, "out": "SCHREIBFEHLER: Payload ist kein Base64"}
    try:
        os.makedirs(os.path.dirname(target) or root, exist_ok=True)
        with open(target, "wb") as fh:
            fh.write(data)
    except OSError as exc:
        return {"code": 1, "out": f"SCHREIBFEHLER: {exc}"}
    return {"code": 0, "out": f"{len(data)} Byte geschrieben: {rel}"}
