#!/usr/bin/env python3
"""novnc_bridge.py — reine Logik + Plan-Ausgabe fuer start_novnc_bridge.sh.

Der Sandbox-Proxy veroeffentlicht Port 12000 oeffentlich als
``https://work-1-<RUNTIME_ID>.prod-runtime.all-hands.dev/``. Damit dieser Port
den XFCE-Desktop des Odroid zeigt, laeuft in der Sandbox ein ``websockify``,
das auf den **lokalen Endpunkt des Reverse-Tunnels** zeigt:

    Browser --HTTPS--> Proxy :12000 -> websockify (Sandbox)
            --(Reverse-Tunnel, lokal)--> TigerVNC 127.0.0.1:5901 (Odroid)

Dieses Modul kapselt die gesamte *reine* Logik (Endpunkt-Parsing, URL-Bau,
argv-Bau, Entscheidungslogik). Der einzige Netz-Zugriff (``--check-tunnel``)
und das Starten selbst liegen im Shell-Wrapper bzw. in ``main()``. Damit ist
die Logik ohne Board und ohne Netz testbar (``test_novnc_bridge.py``).

Shell-Schnittstelle: ``--plan`` gibt ``KEY=value``-Zeilen aus, die der Wrapper
mit ``eval`` liest. So gibt es genau EINE Quelle der Wahrheit fuer Argumente,
Ports und URLs.
"""

import argparse
import os
import shutil
import socket
import sys

# Oeffentlicher Proxy: work-1 -> Sandbox-Port 12000, work-2 -> 12001.
DEFAULT_DOMAIN = "prod-runtime.all-hands.dev"
DEFAULT_LISTEN = "0.0.0.0:12000"
DEFAULT_UPSTREAM = "127.0.0.1:5901"
DEFAULT_WORKER = 1
DEFAULT_PAGE = "vnc.html"

# Reihenfolge: Debian/Armbian-Paket zuerst, dann die pip-/Quell-Installationen.
WEBROOT_CANDIDATES = (
    "/usr/share/novnc",
    "/usr/share/webapps/novnc",
    "/opt/novnc",
    "/usr/local/share/novnc",
)


def validate_port(value, what="port"):
    """Port als int 1..65535. Wirft ValueError mit klarer Meldung."""
    try:
        number = int(str(value).strip())
    except (TypeError, ValueError):
        raise ValueError("%s: %r ist keine Zahl" % (what, value))
    if not 1 <= number <= 65535:
        raise ValueError("%s: %d ausserhalb 1..65535" % (what, number))
    return number


def parse_endpoint(spec, what="endpoint", default_host="127.0.0.1"):
    """``HOST:PORT`` | ``:PORT`` | ``PORT`` | ``[v6]:PORT`` -> ``(host, port)``.

    Ohne Host wird ``default_host`` benutzt: ``0.0.0.0`` fuer den Listener,
    ``127.0.0.1`` fuer das Tunnel-Ziel.
    """
    text = (spec or "").strip()
    if not text:
        raise ValueError("%s: leer" % what)
    if text.startswith("["):
        host, sep, rest = text[1:].partition("]")
        if not sep or not rest.startswith(":"):
            raise ValueError("%s: %r ist kein [IPv6]:PORT" % (what, spec))
        port_raw = rest[1:]
        host = host or "::"
    elif ":" in text:
        host, _, port_raw = text.rpartition(":")
        host = host or default_host
    else:
        host, port_raw = default_host, text
    return host, validate_port(port_raw, what)


def format_endpoint(host, port):
    """Umkehrung von parse_endpoint, fuer Anzeige und argv."""
    if ":" in host:  # IPv6 in eckige Klammern
        return "[%s]:%d" % (host, port)
    return "%s:%d" % (host, port)


def public_base_url(runtime_id, worker=DEFAULT_WORKER, domain=DEFAULT_DOMAIN):
    """Basis-URL des Sandbox-Proxys fuer den gegebenen Worker."""
    if not runtime_id:
        raise ValueError("RUNTIME_ID fehlt (weder --runtime-id noch $RUNTIME_ID)")
    worker = validate_worker(worker)
    return "https://work-%d-%s.%s" % (worker, runtime_id, domain)


def validate_worker(worker):
    try:
        number = int(worker)
    except (TypeError, ValueError):
        raise ValueError("worker: %r ist keine Zahl" % (worker,))
    if number not in (1, 2):
        raise ValueError("worker: %d ungueltig (nur 1 oder 2)" % number)
    return number


def vnc_page_url(base_url, page=DEFAULT_PAGE):
    """Haengt die noVNC-Seite an die Proxy-Basis an (``/vnc.html``)."""
    if not base_url:
        raise ValueError("base_url fehlt")
    page = (page or DEFAULT_PAGE).lstrip("/")
    return base_url.rstrip("/") + "/" + page


def pick_webroot(candidates=WEBROOT_CANDIDATES, isdir=os.path.isdir):
    """Erstes vorhandenes noVNC-Webroot oder ``None``.

    ``isdir`` ist injizierbar, damit der Test ohne Dateisystem auskommt.
    """
    for path in candidates:
        if isdir(path):
            return path
    return None


def build_websockify_argv(websockify, webroot, listen, upstream):
    """argv fuer ``websockify --web=<root> <listen> <upstream>``.

    ``listen``/``upstream`` sind ``(host, port)``-Tupel. Reihenfolge der
    Optionen ist fest, damit der Test sie exakt vergleichen kann.
    """
    argv = [websockify]
    if webroot:
        argv.append("--web=" + webroot)
    argv.append(format_endpoint(*listen))
    argv.append(format_endpoint(*upstream))
    return argv


def build_plan(listen_spec, upstream_spec, webroot, websockify,
               runtime_id, worker=DEFAULT_WORKER, domain=DEFAULT_DOMAIN,
               page=DEFAULT_PAGE, require_webroot=True):
    """Fasst alle abgeleiteten Werte zu einem dict zusammen (rein)."""
    if require_webroot and not webroot:
        raise ValueError(
            "kein noVNC-Webroot gefunden (gesucht: %s) - noVNC installieren "
            "oder --webroot angeben" % ", ".join(WEBROOT_CANDIDATES))
    listen = parse_endpoint(listen_spec, what="listen", default_host="0.0.0.0")
    upstream = parse_endpoint(upstream_spec, what="upstream",
                              default_host="127.0.0.1")
    if listen == upstream:
        raise ValueError("listen und upstream sind identisch (%s) - das wuerde "
                         "sich selbst ansprechen" % format_endpoint(*listen))
    base = public_base_url(runtime_id, worker, domain)
    return {
        "listen": listen,
        "upstream": upstream,
        "webroot": webroot,
        "websockify": websockify,
        "argv": build_websockify_argv(websockify, webroot, listen, upstream),
        "public_base_url": base,
        "vnc_page_url": vnc_page_url(base, page),
    }


def decide_action(already_running, upstream_open):
    """Reine Entscheidung fuer den Wrapper.

    ``already-running`` -> nichts tun (idempotent).
    ``no-tunnel``       -> Fehler: der Reverse-Tunnel steht nicht.
    ``start``           -> websockify starten.
    """
    if already_running:
        return "already-running"
    if not upstream_open:
        return "no-tunnel"
    return "start"


def loopback_host(host):
    """Wildcard-Bind-Adressen auf Loopback abbilden, um sie zu *testen*."""
    if host in ("", "0.0.0.0", "::", "*"):
        return "127.0.0.1"
    return host


def _connect_ok(host, port, timeout=3.0):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def _plan_lines(plan):
    return [
        "BRIDGE_LISTEN=%s" % format_endpoint(*plan["listen"]),
        "BRIDGE_UPSTREAM=%s" % format_endpoint(*plan["upstream"]),
        "BRIDGE_WEBROOT=%s" % plan["webroot"],
        "BRIDGE_WEBSOCKIFY=%s" % plan["websockify"],
        "BRIDGE_PUBLIC_URL=%s" % plan["vnc_page_url"],
    ]


def _add_common(ap):
    ap.add_argument("--listen", default=os.environ.get("BRIDGE_LISTEN", DEFAULT_LISTEN),
                    help="Adresse, auf der websockify lauscht (Default %s)" % DEFAULT_LISTEN)
    ap.add_argument("--upstream", default=os.environ.get("BRIDGE_UPSTREAM", DEFAULT_UPSTREAM),
                    help="lokaler Endpunkt des Reverse-Tunnels (Default %s)" % DEFAULT_UPSTREAM)
    ap.add_argument("--webroot", default=os.environ.get("BRIDGE_WEBROOT", ""),
                    help="noVNC-Webroot (Default: Autodetect)")
    ap.add_argument("--websockify", default=os.environ.get("BRIDGE_WEBSOCKIFY", ""),
                    help="Pfad zum websockify-Binary (Default: aus $PATH)")
    ap.add_argument("--runtime-id", default=os.environ.get("RUNTIME_ID", ""),
                    help="Sandbox-RUNTIME_ID fuer die oeffentliche URL")
    ap.add_argument("--worker", default=os.environ.get("BRIDGE_WORKER", str(DEFAULT_WORKER)),
                    help="Proxy-Worker 1 (Port 12000) oder 2 (Port 12001)")
    ap.add_argument("--domain", default=os.environ.get("BRIDGE_DOMAIN", DEFAULT_DOMAIN),
                    help="Proxy-Domain (Default %s)" % DEFAULT_DOMAIN)
    ap.add_argument("--page", default=os.environ.get("BRIDGE_PAGE", DEFAULT_PAGE),
                    help="noVNC-Seite (Default %s)" % DEFAULT_PAGE)


def _resolve(args):
    """Optionen -> (plan, error_text). Reine Ableitung bis auf which()."""
    websockify = args.websockify or shutil.which("websockify") or ""
    webroot = args.webroot or pick_webroot() or ""
    try:
        plan = build_plan(args.listen, args.upstream, webroot, websockify,
                          args.runtime_id, args.worker, args.domain, args.page)
    except ValueError as exc:
        return None, str(exc)
    return plan, None


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(
        prog="novnc_bridge.py",
        description="Reine Logik fuer start_novnc_bridge.sh (Plan/URL/argv).")
    ap.add_argument("--plan", action="store_true",
                    help="validierten Plan als KEY=value-Zeilen ausgeben")
    ap.add_argument("--check-tunnel", action="store_true",
                    help="nur pruefen, ob der Tunnel-Endpunkt erreichbar ist")
    ap.add_argument("--check-listen", action="store_true",
                    help="nur pruefen, ob der Listen-Port schon belegt ist")
    ap.add_argument("--print-url", action="store_true",
                    help="nur die oeffentliche noVNC-URL ausgeben")
    _add_common(ap)
    args = ap.parse_args(argv)

    if args.check_tunnel:
        upstream = parse_endpoint(args.upstream, what="upstream",
                                  default_host="127.0.0.1")
        ok = _connect_ok(*upstream)
        print("%s %s" % ("offen" if ok else "geschlossen",
                         format_endpoint(*upstream)))
        return 0 if ok else 1

    if args.check_listen:
        listen = parse_endpoint(args.listen, what="listen", default_host="0.0.0.0")
        ok = _connect_ok(loopback_host(listen[0]), listen[1])
        print("%s %s" % ("belegt" if ok else "frei",
                         format_endpoint(*listen)))
        return 0 if ok else 1

    if args.print_url:
        try:
            print(vnc_page_url(public_base_url(args.runtime_id, args.worker,
                                               args.domain), args.page))
        except ValueError as exc:
            print("FEHLER: %s" % exc, file=sys.stderr)
            return 2
        return 0

    if args.plan:
        plan, err = _resolve(args)
        if err:
            print("FEHLER: %s" % err, file=sys.stderr)
            return 2
        for line in _plan_lines(plan):
            print(line)
        return 0

    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
