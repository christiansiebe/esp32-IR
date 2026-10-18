#!/bin/sh
# start_novnc_bridge.sh — XFCE-Desktop des Odroid ueber den Sandbox-Proxy zeigen.
#
# WAS DAS MACHT (nur in DIESER Sandbox, kein Board-Zugriff):
#   Der Proxy veroeffentlicht Port 12000 oeffentlich als
#       https://work-1-<RUNTIME_ID>.prod-runtime.all-hands.dev/
#   Dieses Skript startet hier ein `websockify`, das auf Port 12000 lauscht und
#   auf den LOKALEN Endpunkt des Reverse-Tunnels zeigt (Default 127.0.0.1:5901).
#   Der Tunnel (auf dem Odroid aufgebaut) reicht bis TigerVNC 127.0.0.1:5901.
#
#   Browser --HTTPS--> Proxy :12000 -> websockify -> (Tunnel) -> Odroid :5901
#
#   Der Tunnel wird von DIESEM Skript NICHT aufgebaut: er laeuft auf dem Odroid
#   und ist Voraussetzung. Fehlt er, bricht das Skript mit klarer Meldung ab.
#
# AUFRUF
#   sh start_novnc_bridge.sh                 # starten (idempotent)
#   sh start_novnc_bridge.sh --dry-run       # nur zeigen, was passieren wuerde
#   sh start_novnc_bridge.sh --status        # laeuft der Bridge-Prozess?
#   sh start_novnc_bridge.sh --stop          # Prozess stoppen (PIDfile)
#   sh start_novnc_bridge.sh --help
#
# Optionen (auch als Umgebungsvariable):
#   --listen HOST:PORT     Default 0.0.0.0:12000
#   --upstream HOST:PORT   Default 127.0.0.1:5901  (Tunnel-Endpunkt)
#   --webroot DIR          Default: Autodetect
#   --websockify PFAD      Default: aus $PATH
#   --runtime-id ID        Default: $RUNTIME_ID
#   --worker 1|2           Default 1 (Port 12000); 2 -> Port 12001
#   --page NAME            Default vnc.html
#
# Die reine Logik (Ports, URL, argv) steckt in novnc_bridge.py und ist ohne
# Board/Netz getestet (test_novnc_bridge.py).
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LOGIC="$HERE/novnc_bridge.py"
PY="${BRIDGE_PYTHON:-python3}"
PIDFILE="${BRIDGE_PIDFILE:-/tmp/novnc_bridge.pid}"
LOGFILE="${BRIDGE_LOG:-/tmp/novnc_bridge.log}"

die() {
    msg="$*"
    case "$msg" in FEHLER:*) ;; *) msg="FEHLER: $msg" ;; esac
    echo "$msg" >&2
    exit 1
}

usage() {
    awk 'NR>1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"
    echo
    echo "Optionen werden an novnc_bridge.py durchgereicht (siehe --help dort)."
}

# --- Argumente -----------------------------------------------------------------
DRY_RUN=0
ACTION=start
PASSTHRU=""
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --dry-run) DRY_RUN=1 ;;
        --status) ACTION=status ;;
        --stop) ACTION=stop ;;
        --listen|--upstream|--webroot|--websockify|--runtime-id|--worker|--domain|--page)
            [ $# -ge 2 ] || die "$1 braucht einen Wert"
            PASSTHRU="$PASSTHRU $1 $2"; shift ;;
        --*) die "unbekannte Option: $1 (--help)" ;;
        *) die "unerwartetes Argument: $1 (--help)" ;;
    esac
    shift
done

[ -f "$LOGIC" ] || die "$LOGIC fehlt (liegt neben diesem Skript)"

# shellcheck disable=SC2086
plan() { $PY "$LOGIC" --plan $PASSTHRU; }

# --- status / stop (nur PIDfile, kein Board) -----------------------------------
case "$ACTION" in
    status)
        if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            echo "laeuft (PID $(cat "$PIDFILE"))"
            exit 0
        fi
        echo "laeuft nicht"
        exit 1 ;;
    stop)
        if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
            pid=$(cat "$PIDFILE")
            kill "$pid" 2>/dev/null || true
            rm -f "$PIDFILE"
            echo "gestoppt (PID $pid)"
        else
            echo "nichts zu stoppen"
            rm -f "$PIDFILE"
        fi
        exit 0 ;;
esac

# --- Plan ableiten (reine Logik) -----------------------------------------------
set +e
PLAN=$(plan 2>&1)
rc=$?
set -e
if [ $rc -ne 0 ]; then
    die "$PLAN"
fi
# KEY=value-Zeilen einlesen (nur erwartete Keys, kein beliebiges eval).
BRIDGE_LISTEN=""; BRIDGE_UPSTREAM=""; BRIDGE_WEBROOT=""
BRIDGE_WEBSOCKIFY=""; BRIDGE_PUBLIC_URL=""
set -f  # keine Glob-Expansion beim Wortzerlegen (Werte sind leerzeichenfrei)
for line in $PLAN; do
    case "$line" in
        BRIDGE_LISTEN=*)    BRIDGE_LISTEN=${line#*=} ;;
        BRIDGE_UPSTREAM=*)  BRIDGE_UPSTREAM=${line#*=} ;;
        BRIDGE_WEBROOT=*)   BRIDGE_WEBROOT=${line#*=} ;;
        BRIDGE_WEBSOCKIFY=*) BRIDGE_WEBSOCKIFY=${line#*=} ;;
        BRIDGE_PUBLIC_URL=*) BRIDGE_PUBLIC_URL=${line#*=} ;;
    esac
done
set +f
[ -n "$BRIDGE_WEBSOCKIFY" ] || die "websockify nicht gefunden (PATH oder --websockify)"
[ -n "$BRIDGE_WEBROOT" ] || die "noVNC-Webroot nicht gefunden (--webroot)"

# --- Idempotenz: laeuft schon? --------------------------------------------------
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "Bridge laeuft schon (PID $(cat "$PIDFILE")) -> $BRIDGE_PUBLIC_URL"
    exit 0
fi
set +e
$PY "$LOGIC" --check-listen --listen "$BRIDGE_LISTEN" >/dev/null 2>&1
listen_rc=$?
set -e
if [ $listen_rc -eq 0 ]; then
    echo "Port $BRIDGE_LISTEN ist schon belegt (fremder Prozess)."
    echo "Erst pruefen: ss -lntp | grep 12000   (oder --listen aendern)"
    exit 1
fi

# --- Dry-Run (zeigt alles, bricht nicht ab) -------------------------------------
if [ "$DRY_RUN" -eq 1 ]; then
    set +e
    $PY "$LOGIC" --check-tunnel --upstream "$BRIDGE_UPSTREAM" >/dev/null 2>&1
    tunnel_rc=$?
    set -e
    echo "dry-run: wuerde starten"
    echo "  websockify : $BRIDGE_WEBSOCKIFY"
    echo "  listen     : $BRIDGE_LISTEN"
    echo "  upstream   : $BRIDGE_UPSTREAM"
    echo "  webroot    : $BRIDGE_WEBROOT"
    echo "  URL        : $BRIDGE_PUBLIC_URL"
    echo "  log        : $LOGFILE"
    echo "  pidfile    : $PIDFILE"
    if [ $tunnel_rc -eq 0 ]; then
        echo "  Tunnel     : offen"
    else
        echo "  Tunnel     : GESCHLOSSEN -> ein echter Start wuerde abbrechen"
    fi
    exit 0
fi

# --- Tunnel vorhanden? ----------------------------------------------------------
set +e
$PY "$LOGIC" --check-tunnel --upstream "$BRIDGE_UPSTREAM" >/dev/null 2>&1
tunnel_rc=$?
set -e
if [ $tunnel_rc -ne 0 ]; then
    echo "FEHLER: kein Reverse-Tunnel auf $BRIDGE_UPSTREAM." >&2
    echo "  Der Tunnel wird auf dem Odroid aufgebaut und ist Voraussetzung." >&2
    echo "  Er muss hier einen lokalen Port 5901 bereitstellen, z.B.:" >&2
    echo "    ssh -N -R 127.0.0.1:5901:127.0.0.1:5901 <odroid>" >&2
    echo "  (siehe novnc-external-README.md, Abschnitt 'Tunnel')." >&2
    exit 1
fi

# --- Start ----------------------------------------------------------------------
echo "Starte websockify: $BRIDGE_LISTEN -> $BRIDGE_UPSTREAM"
# shellcheck disable=SC2086
setsid nohup "$BRIDGE_WEBSOCKIFY" --web="$BRIDGE_WEBROOT" \
    "$BRIDGE_LISTEN" "$BRIDGE_UPSTREAM" >>"$LOGFILE" 2>&1 &
pid=$!
echo "$pid" > "$PIDFILE"

# Kurz auf den Listener warten (max ~5 s).
i=0
while [ $i -lt 25 ]; do
    if $PY "$LOGIC" --check-listen --listen "$BRIDGE_LISTEN" >/dev/null 2>&1; then
        echo "Bridge laeuft (PID $pid)"
        echo "  http://127.0.0.1:${BRIDGE_LISTEN##*:}/vnc.html  (lokal)"
        echo "  $BRIDGE_PUBLIC_URL  (oeffentlich)"
        exit 0
    fi
    kill -0 "$pid" 2>/dev/null || { rm -f "$PIDFILE"; die "websockify sofort beendet - siehe $LOGFILE"; }
    sleep 0.2
    i=$((i + 1))
done
rm -f "$PIDFILE"
die "websockify hat nicht gelauscht (siehe $LOGFILE)"
