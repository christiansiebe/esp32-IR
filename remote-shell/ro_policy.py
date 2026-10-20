#!/usr/bin/env python3
"""Read-only command policy for the Odroid channel.

Shared by BOTH sides of the channel (defence in depth):

  * ``sandbox_term_server.py`` checks it before it sends a command to the board
    over the HTTP endpoint ``/ro`` (the path a tool-less caller such as a plain
    chat/browser uses), and
  * ``cmd_agent.py`` checks it again on the board before it runs a command that
    arrived with ``"ro": true`` - and then runs it *without* the interactive
    yes/no prompt, because a read-only command is safe to auto-approve.

Only a command that passes ``check()`` on the board is ever auto-executed. Every
other command keeps the normal human-confirmation flow.

The policy is deliberately a strict allowlist:

  * no shell metacharacters (``;`` ``&`` ``|`` is allowed as a pipeline,
    but no ``>`` ``<`` ``$(`` backtick, backslash, newline) - so no redirection,
    no command substitution, no chaining;
  * every pipeline segment must start with a binary from PURE_READ, or from
    SUBCOMMAND with an allowed subcommand;
  * a list of forbidden substrings (``vacuum``, ``mkfs``, ...) is rejected
    anywhere in the line, to close the gaps a first-word check alone leaves.

Anything not explicitly allowed is denied.
"""
import re
import shlex

# Binaries that only ever read/inspect. A pipeline segment starting with one of
# these is fine with any argument (the forbidden-substring list still applies).
PURE_READ = frozenset({
    # system / kernel
    "uptime", "uname", "hostname", "date", "whoami", "id", "groups", "nproc",
    "arch", "getconf", "lscpu", "lspci", "lsusb", "lsblk", "lsmod", "free",
    "vmstat", "sensors", "vcgencmd", "timedatectl", "hostnamectl", "locale",
    # files / text (all read-only forms)
    "cat", "tac", "head", "tail", "wc", "nl", "grep", "egrep", "fgrep", "rg",
    "cut", "tr", "sort", "uniq", "column", "jq", "ls", "stat", "file", "du",
    "df", "readlink", "realpath", "findmnt", "blkid", "fdisk", "smartctl",
    "lsattr", "getfacl", "basename", "dirname", "readelf", "strings", "md5sum",
    "sha1sum", "sha256sum",
    # logs / processes
    "journalctl", "dmesg", "ps", "pstree", "top", "htop", "pidof", "pgrep",
    # network (observation only)
    "ip", "ss", "netstat", "ifconfig", "ethtool", "iw", "iwconfig", "route",
    "arp", "ping", "dig", "nslookup", "vnstat",
    # misc
    "echo", "printf", "true", "false", "test", "env", "printenv",
})

# Binaries with subcommands: the first non-flag argument must be in the set.
SUBCOMMAND = {
    "systemctl": frozenset({
        "status", "list-units", "list-unit-files", "list-timers", "list-sockets",
        "show", "cat", "is-active", "is-enabled", "is-failed", "get-default",
        "list-dependencies", "--version",
    }),
    "btrfs": frozenset({
        "filesystem", "fi", "subvolume", "sub", "device", "de", "inspect-internal",
        "property", "qgroup", "scrub", "usage", "du", "version",
    }),
    "docker": frozenset({
        "ps", "images", "logs", "inspect", "stats", "version", "info", "top",
        "port", "df", "events", "history", "container", "image",
    }),
    "podman": frozenset({
        "ps", "images", "logs", "inspect", "stats", "version", "info", "top", "df",
    }),
}

# Rejected anywhere in the line. Word-boundary regex, so a plain substring like
# "at " inside "stat" or "dd " inside "add" does not cause a false positive.
FORBIDDEN_PATTERNS = (
    r"\bvacuum\b", r"\brotate\b", r"\bflush\b", r"\bdelete\b", r"\bremove\b",
    r"\brmdir\b", r"\bunlink\b", r"\bmkfs", r"\bwipefs\b", r"\bdd\b",
    r"\btruncate\b", r"\bshred\b", r"\bchattr\b", r"\bchmod\b", r"\bchown\b",
    r"\bchgrp\b", r"\bmount\b", r"\bumount\b", r"\bswapoff\b", r"\bswapon\b",
    r"\bshutdown\b", r"\breboot\b", r"\bpoweroff\b", r"\bhalt\b",
    r"\bsystemctl\s+(start|stop|restart|reload|disable|enable|mask|kill|set-)",
    r"\b(kill|pkill|killall)\b",
    r"\bapt(-get)?\b", r"\bdpkg\b", r"\bsnap\b", r"\bpip3?\b", r"\bnpm\b",
    r"\byarn\b", r"\bcurl\b", r"\bwget\b", r"\bnc\b", r"\bncat\b",
    r"\btelnet\b", r"\bssh\b", r"\bscp\b", r"\brsync\b", r"\bsftp\b",
    r"\bcrontab\b", r"\bpasswd\b", r"\buser(add|mod|del)\b", r"\bgroupadd\b",
    r"\biptables\b", r"\bnft\b", r"\bufw\b", r"\bsysctl\b", r"\btee\b",
    r"\b(python3?|perl|ruby|node)\b", r"\b(bash|sh|zsh)\s+-c\b", r"\beval\b",
    r"\bexec\b", r"\bsource\b", r"\bsudo\b", r"\bsu\b", r"\binsmod\b",
    r"\bmodprobe\b", r"\brmmod\b",
)
FORBIDDEN_RE = re.compile("|".join(FORBIDDEN_PATTERNS), re.IGNORECASE)

# Paths that are read-only to read but must never leave the board: credential
# stores and private keys. Reading them would be exfiltration, not inspection.
SENSITIVE_RE = re.compile(
    r"(/etc/(shadow|gshadow|sudoers)|\.ssh/|id_rsa|id_ed25519|"
    r"\.vnc/PASSWORT|/token\b|\.api_key|\.netrc|\.pgpass|"
    r"/etc/odroid-cmd-agent\.env)",
    re.IGNORECASE,
)

# Characters that enable chaining, substitution or redirection. A pipeline '|'
# is allowed (each segment is checked separately); everything else is not.
FORBIDDEN_CHARS = (";", "&", "`", "$", ">", "<", "\n", "\r", "\\", "{", "}")

MAX_LEN = 500


def check(cmd):
    """Return ``(ok, reason)``. ``ok`` is True only for a read-only command."""
    if not isinstance(cmd, str) or not cmd.strip():
        return False, "leerer Befehl"
    s = cmd.strip()
    if len(s) > MAX_LEN:
        return False, f"zu lang (max {MAX_LEN} Zeichen)"

    for ch in FORBIDDEN_CHARS:
        if ch in s:
            return False, f"verbotenes Zeichen: {ch!r}"

    m = FORBIDDEN_RE.search(s)
    if m:
        return False, f"verboten: {m.group(0)!r}"

    m = SENSITIVE_RE.search(s)
    if m:
        return False, f"geschuetzter Pfad: {m.group(0)!r}"

    for segment in s.split("|"):
        segment = segment.strip()
        if not segment:
            return False, "leeres Pipeline-Segment"
        try:
            parts = shlex.split(segment)
        except ValueError as exc:
            return False, f"nicht lesbar: {exc}"
        if not parts:
            return False, "leeres Pipeline-Segment"
        base = parts[0].rsplit("/", 1)[-1]
        if base in SUBCOMMAND:
            allowed = SUBCOMMAND[base]
            subs = [p for p in parts[1:] if not p.startswith("-")]
            if subs:
                if subs[0] not in allowed:
                    return False, f"{base}: Unterbefehl {subs[0]!r} nicht erlaubt"
            elif not any(p in allowed for p in parts[1:]):
                return False, f"{base}: nur Flags - Unterbefehl fehlt"
        elif base not in PURE_READ:
            return False, f"nicht erlaubt: {base}"
    return True, "ok"


def help_text():
    """Plain-text help listing the allowed read-only commands."""
    pure = ", ".join(sorted(PURE_READ))
    sub = "\n".join(f"  {k}: {', '.join(sorted(v))}" for k, v in sorted(SUBCOMMAND.items()))
    return (
        "read-only Kanal zum Odroid\n\n"
        "Aufruf:  /ro?token=<TOKEN>&c=<befehl>\n\n"
        "Erlaubt (reine Lese-Befehle, Pipeline mit | moeglich):\n  " + pure + "\n\n"
        "Mit Unterbefehl:\n" + sub + "\n\n"
        "Verboten: ; & ` $ > < \\ sowie Schreib-/Aenderungsbefehle\n"
    )


if __name__ == "__main__":
    import sys
    for arg in sys.argv[1:] or [""]:
        ok, why = check(arg)
        print(f"{'OK  ' if ok else 'DENY'}  {arg!r}  -> {why}")
