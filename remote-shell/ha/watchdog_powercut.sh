#!/bin/sh
# Off-board power-cut wrapper: the single actuator entry point.
#
# Called by watchdog_monitor.py via --reboot-cmd (see WATCHDOG-ACTOR.md 5.1).
# It builds the watchdog_actor.py invocation from environment variables, so the
# monitor never has to quote nested arguments, and it keeps the DURABLE
# cooldown state (--state-file) in a persistent directory.
#
# DRY RUN IS THE DEFAULT. Nothing is switched unless WATCHDOG_POWERCUT_ARMED=1
# (or --armed) is given; without it watchdog_actor.py runs without --execute and
# only prints what it WOULD do. Arming is a deliberate operator action.
#
# Env (defaults):
#   WATCHDOG_ACTOR          watchdog_actor.py path  (/usr/local/sbin/watchdog_actor.py)
#   WATCHDOG_ACTOR_KIND     command|http|mqtt       (command)
#   WATCHDOG_OFF_CMD/ON_CMD relay off/on commands   (usbrelay example)
#   WATCHDOG_COMMAND        single command (command kind, no pulse)
#   WATCHDOG_URL            http target             (http kind)
#   WATCHDOG_TOPIC/PAYLOAD  mqtt target             (mqtt kind)
#   WATCHDOG_PULSE_SECONDS  power-cut length [s]    (5)
#   WATCHDOG_COOLDOWN       durable cooldown [s]    (900)  <- match the monitor
#   WATCHDOG_STATE_FILE     durable state path      (/var/lib/watchdog/actor-state)
#   WATCHDOG_MIN_QUORUM     confirmations needed    (1)
#   WATCHDOG_QUORUM_VOTES   e.g. 'true,true'
#   WATCHDOG_NOW            clock override (tests only; unset in production)
#   WATCHDOG_POWERCUT_ARMED 1 = really switch
#
# Exit codes mirror watchdog_actor.py: 0 ok/blocked/dry-run, 1 actor failed,
# 2 invalid plan.
set -eu

ACTOR="${WATCHDOG_ACTOR:-/usr/local/sbin/watchdog_actor.py}"
KIND="${WATCHDOG_ACTOR_KIND:-command}"
COOLDOWN="${WATCHDOG_COOLDOWN:-900}"
PULSE="${WATCHDOG_PULSE_SECONDS:-5}"
STATE="${WATCHDOG_STATE_FILE:-/var/lib/watchdog/actor-state}"
MIN_QUORUM="${WATCHDOG_MIN_QUORUM:-1}"
QUORUM_VOTES="${WATCHDOG_QUORUM_VOTES:-}"
ARMED="${WATCHDOG_POWERCUT_ARMED:-0}"

# --armed / --dry-run on the command line are equivalent to the env switch.
for arg in "$@"; do
    case "$arg" in
        --armed) ARMED=1 ;;
        --dry-run) ARMED=0 ;;
    esac
done

if [ ! -f "$ACTOR" ]; then
    echo "FEHLER: watchdog_actor.py nicht gefunden: $ACTOR" >&2
    exit 2
fi

# Persistenz-Verzeichnis anlegen, damit der Cooldown den Monitor-Neustart
# ueberlebt (best effort; der Aktor meldet einen Fehler sonst selbst).
STATE_DIR="$(dirname "$STATE")"
if [ ! -d "$STATE_DIR" ]; then
    mkdir -p "$STATE_DIR" 2>/dev/null || true
fi

set -- --kind "$KIND"
case "$KIND" in
    command)
        COMMAND="${WATCHDOG_COMMAND:-}"
        if [ -n "$COMMAND" ]; then
            set -- "$@" --command "$COMMAND"
        else
            set -- "$@" --off-cmd "${WATCHDOG_OFF_CMD:-usbrelay REL1=0}" \
                --on-cmd "${WATCHDOG_ON_CMD:-usbrelay REL1=1}" \
                --pulse-seconds "$PULSE"
        fi
        ;;
    http)
        if [ -z "${WATCHDOG_URL:-}" ]; then
            echo "FEHLER: http braucht WATCHDOG_URL" >&2
            exit 2
        fi
        set -- "$@" --url "$WATCHDOG_URL"
        ;;
    mqtt)
        if [ -z "${WATCHDOG_TOPIC:-}" ]; then
            echo "FEHLER: mqtt braucht WATCHDOG_TOPIC" >&2
            exit 2
        fi
        set -- "$@" --topic "$WATCHDOG_TOPIC" \
            --payload "${WATCHDOG_PAYLOAD:-TOGGLE}"
        ;;
    *)
        echo "FEHLER: unbekannter WATCHDOG_ACTOR_KIND: $KIND" >&2
        exit 2
        ;;
esac

set -- "$@" --cooldown "$COOLDOWN" --state-file "$STATE" --min-quorum "$MIN_QUORUM"
if [ -n "$QUORUM_VOTES" ]; then
    set -- "$@" --quorum-votes "$QUORUM_VOTES"
fi
if [ -n "${WATCHDOG_NOW:-}" ]; then
    set -- "$@" --now "$WATCHDOG_NOW"
fi
if [ "$ARMED" = "1" ]; then
    set -- "$@" --execute
fi

exec python3 "$ACTOR" "$@"
