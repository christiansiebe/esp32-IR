#!/usr/bin/env python3
"""Off-board watchdog actor adapter for `watchdog_monitor.py`.

Background: `WATCHDOG-GAP.md` (incl. section 8, parent board verification).
The 60 s hardware watchdog did NOT reboot the board after a CPU-PLL hang; the
best remaining explanation is H3 (the reset never came through). The internal
safety net is therefore unreliable for raw PLL-OC experiments.

`watchdog_monitor.py` observes the board's heartbeat OFF-BOARD and decides
*when* to act (pure `decide()`). This module decides *how* the actuator is
driven. It is deliberately the same shape: the request is **built** as a pure
function, so the whole adapter is host-testable without a board, a broker or a
network. Nothing is sent unless `--execute` is given (default is a dry run).

Actuator kinds (see `WATCHDOG-ACTOR.md` for the trade-offs):

  * ``http``    -- HA switch / Tasmota / ESPHome REST relay. GET or POST a URL.
  * ``mqtt``    -- publish a topic (Tasmota/ESPHome relay). Builds the
                   ``mosquitto_pub`` argv; a broker is only contacted with
                   ``--execute``.
  * ``command`` -- a local command: USB relay CLI, GPIO tooling, or an ``ssh``
                   power-cut wrapper (what `watchdog_monitor.py --reboot-cmd`
                   already uses).

A wrong trigger is a **power cut for the board**, so a cooldown and an optional
quorum sit in front of every send.

Usage (dry run, prints what would happen)::

    python3 watchdog_actor.py --kind http \
        --url 'http://plug.local/cm?cmnd=Power%20TOGGLE'
    python3 watchdog_actor.py --kind mqtt \
        --topic 'cmnd/tasmota-plug/Power' --payload 'TOGGLE'
    python3 watchdog_actor.py --kind command \
        --off-cmd 'usbrelay REL1=0' --on-cmd 'usbrelay REL1=1' --pulse-seconds 5
    python3 watchdog_actor.py --kind http --url ... --execute   # really send
"""
import argparse
import subprocess
import sys
import time
import urllib.request

DEFAULTS = {
    "cooldown": 600.0,        # min. seconds between two real triggers
    "min_quorum": 1,          # independent confirmations needed (1 = off)
    "pulse_seconds": 5.0,     # power-cut length for the off/on pulse
    "request_timeout": 10.0,  # HTTP timeout [s]
}

VALID_KINDS = ("http", "mqtt", "command")
_TRUTHY = ("1", "true", "yes", "y", "ok", "on")


# --------------------------------------------------------------------------
# Pure request builders -- no I/O.
# --------------------------------------------------------------------------
def build_http_request(url, method="GET", body=None, headers=None, timeout=None):
    """Build an HTTP actuator request. Pure.

    Returns ``{"url", "method", "body", "headers", "timeout"}``. A POST/PUT
    without a body still sends ``Content-Length: 0`` so the relay sees a
    complete request.
    """
    if not url:
        raise ValueError("http actuator needs --url")
    method = (method or "GET").upper()
    if method not in ("GET", "POST", "PUT"):
        raise ValueError("unsupported http method: %s" % method)
    hdrs = dict(headers or {})
    payload = body
    if payload is not None:
        data = payload.encode("utf-8") if isinstance(payload, str) else payload
        hdrs.setdefault("Content-Type", "text/plain; charset=utf-8")
        payload = data
    elif method in ("POST", "PUT"):
        payload = b""
    if payload is not None:
        hdrs["Content-Length"] = str(len(payload))
    return {"url": url, "method": method, "body": payload, "headers": hdrs,
            "timeout": DEFAULTS["request_timeout"] if timeout is None else timeout}


def build_mqtt_request(topic, payload="TOGGLE", retain=False, username=None):
    """Build an MQTT publish request. Pure -- no broker connection.

    Returns ``{"topic", "payload", "retain", "username", "argv"}``; ``argv`` is
    the ``mosquitto_pub`` argument list, used only with ``--execute``.
    """
    if not topic:
        raise ValueError("mqtt actuator needs --topic")
    argv = ["mosquitto_pub", "-t", topic, "-m", str(payload)]
    if retain:
        argv.append("-r")
    if username:
        argv += ["-u", username]
    return {"topic": topic, "payload": payload, "retain": bool(retain),
            "username": username, "argv": argv}


def _num(value):
    """Format a number without a trailing ``.0`` (5.0 -> '5', 0.5 -> '0.5')."""
    f = float(value)
    return str(int(f)) if f.is_integer() else "%g" % f


def build_pulse_command(off_cmd, on_cmd, pulse_seconds=None):
    """Combine an off/on command pair into one shell line with a delay.

    A power-cut relay needs a *pulse*: off, wait, on. Splitting it into two
    commands would leave the board unpowered if the second step never ran.
    """
    if not off_cmd or not on_cmd:
        raise ValueError("pulse needs both --off-cmd and --on-cmd")
    secs = DEFAULTS["pulse_seconds"] if pulse_seconds is None else pulse_seconds
    if secs < 0:
        raise ValueError("pulse seconds must be >= 0")
    return "%s; sleep %s; %s" % (off_cmd, _num(secs), on_cmd)


def build_plan(kind, url="", method="GET", body=None, timeout=None, topic="",
               payload="TOGGLE", retain=False, username=None, command=""):
    """Describe what the actuator WOULD do, for the given kind. Pure.

    Returns a dict with at least ``kind``, ``target`` and ``payload``; http and
    mqtt also carry the built ``request``.
    """
    if kind == "http":
        req = build_http_request(url, method=method, body=body, timeout=timeout)
        return {"kind": "http", "request": req, "target": req["url"],
                "payload": body}
    if kind == "mqtt":
        req = build_mqtt_request(topic, payload, retain=retain, username=username)
        return {"kind": "mqtt", "request": req, "target": req["topic"],
                "payload": req["payload"], "argv": req["argv"]}
    if kind == "command":
        if not command:
            raise ValueError("command actuator needs --command")
        return {"kind": "command", "target": command, "payload": None,
                "argv": ["/bin/sh", "-c", command]}
    raise ValueError("unknown actuator kind: %s" % kind)


def format_plan(plan):
    """One human-readable line for logging / the dry run."""
    if plan["kind"] == "http":
        r = plan["request"]
        return "HTTP %s %s (timeout %ss)" % (r["method"], r["url"], r["timeout"])
    if plan["kind"] == "mqtt":
        r = plan["request"]
        return "MQTT publish %s = %r (retain=%s)" % (
            r["topic"], r["payload"], r["retain"])
    return "CMD %s" % plan["target"]


# --------------------------------------------------------------------------
# Guard logic -- pure.
# --------------------------------------------------------------------------
def cooldown_ok(now, last_fired, cooldown):
    """True if enough time passed since the last real trigger."""
    if last_fired is None:
        return True
    return (now - last_fired) >= cooldown


def quorum_ok(votes):
    """Number of truthy votes in an iterable."""
    return sum(1 for v in votes if v)


def evaluate(now, last_fired, cooldown, votes=None, min_quorum=None):
    """Gate a trigger. Returns ``(allowed: bool, reason: str)``.

    Checks quorum first, then the cooldown. ``votes=None`` means a single
    implicit confirmation (the monitor already applies its own hysteresis).
    """
    if min_quorum is None:
        min_quorum = DEFAULTS["min_quorum"]
    votes = [True] if votes is None else list(votes)
    n = quorum_ok(votes)
    if n < min_quorum:
        return False, "Quorum %d/%d nicht erreicht" % (n, min_quorum)
    if not cooldown_ok(now, last_fired, cooldown):
        return False, "Cooldown aktiv (%.0fs von %.0fs)" % (now - last_fired, cooldown)
    return True, "Trigger erlaubt (Quorum %d/%d)" % (n, min_quorum)


# --------------------------------------------------------------------------
# Execution -- only reached with --execute.
# --------------------------------------------------------------------------
def execute_plan(plan, log=print):
    """Perform the actuator action. Only called with ``--execute``."""
    if plan["kind"] == "http":
        req = plan["request"]
        request = urllib.request.Request(
            req["url"], data=req["body"], headers=req["headers"],
            method=req["method"])
        with urllib.request.urlopen(request, timeout=req["timeout"]) as resp:
            log("http -> %s" % resp.status)
        return True
    if plan["kind"] == "mqtt":
        return subprocess.call(plan["argv"]) == 0
    return subprocess.call(plan["argv"]) == 0


def _read_last_fired(state_file):
    if not state_file:
        return None
    try:
        with open(state_file, "r") as fh:
            return float(fh.read().strip())
    except (OSError, ValueError):
        return None


def _write_last_fired(state_file, value):
    if not state_file:
        return
    try:
        with open(state_file, "w") as fh:
            fh.write("%s\n" % value)
    except OSError as exc:
        print("state-file nicht schreibbar: %s" % exc, file=sys.stderr)


def _parse_votes(text):
    if not text:
        return None
    votes = [p.strip().lower() in _TRUTHY for p in text.split(",") if p.strip()]
    return votes or None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kind", choices=VALID_KINDS, default="command")
    ap.add_argument("--url", default="")
    ap.add_argument("--method", default="GET")
    ap.add_argument("--body", default=None)
    ap.add_argument("--timeout", type=float, default=DEFAULTS["request_timeout"])
    ap.add_argument("--topic", default="")
    ap.add_argument("--payload", default="TOGGLE")
    ap.add_argument("--retain", action="store_true")
    ap.add_argument("--username", default=None)
    ap.add_argument("--command", default="")
    ap.add_argument("--off-cmd", default="")
    ap.add_argument("--on-cmd", default="")
    ap.add_argument("--pulse-seconds", type=float, default=DEFAULTS["pulse_seconds"])
    ap.add_argument("--cooldown", type=float, default=DEFAULTS["cooldown"])
    ap.add_argument("--min-quorum", type=int, default=DEFAULTS["min_quorum"])
    ap.add_argument("--quorum-votes", default="",
                    help="comma-separated confirmations, e.g. 'true,true'")
    ap.add_argument("--last-fired", type=float, default=None,
                    help="epoch of the previous trigger (else use --state-file)")
    ap.add_argument("--state-file", default="",
                    help="persist last-fired epoch across runs")
    ap.add_argument("--now", type=float, default=None,
                    help="override the clock (testing/dry-run)")
    ap.add_argument("--execute", action="store_true",
                    help="really drive the actuator (default: dry run)")
    args = ap.parse_args(argv)

    now = args.now if args.now is not None else time.time()
    last_fired = args.last_fired
    if last_fired is None:
        last_fired = _read_last_fired(args.state_file)

    command = args.command
    if args.off_cmd and args.on_cmd:
        command = build_pulse_command(args.off_cmd, args.on_cmd,
                                      args.pulse_seconds)
    elif args.off_cmd or args.on_cmd:
        print("pulse braucht --off-cmd UND --on-cmd", file=sys.stderr)
        return 2

    try:
        plan = build_plan(args.kind, url=args.url, method=args.method,
                          body=args.body, timeout=args.timeout, topic=args.topic,
                          payload=args.payload, retain=args.retain,
                          username=args.username, command=command)
    except ValueError as exc:
        print("Aktor-Plan ungueltig: %s" % exc, file=sys.stderr)
        return 2

    allowed, reason = evaluate(now, last_fired, args.cooldown,
                               votes=_parse_votes(args.quorum_votes),
                               min_quorum=args.min_quorum)
    print("Aktor: %s" % format_plan(plan))
    print("Gate: %s (%s)" % ("erlaubt" if allowed else "blockiert", reason))
    if not allowed:
        return 0

    if not args.execute:
        print("Trockenlauf: nichts gesendet (--execute fehlt)")
        return 0

    print("FEUERE Aktor")
    ok = execute_plan(plan)
    if ok:
        _write_last_fired(args.state_file, now)
    else:
        print("Aktor fehlgeschlagen", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
