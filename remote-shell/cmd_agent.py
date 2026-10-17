#!/usr/bin/env python3
"""Odroid-Agent: empfaengt Befehle aus einer oder MEHREREN Sandboxes und fuehrt
sie NUR nach lokaler Bestaetigung aus.

Start (als root auf dem Odroid, in einem eigenen Terminal):
    export SANDBOX_TERM_TOKEN='<token>'
    python3 cmd_agent.py 'wss://work-2-<id>.prod-runtime.all-hands.dev/'

Mehrere Ziele gleichzeitig (z.B. mehrere Chats/Sandboxes):
    export SANDBOX_TERM_URLS='wss://work-2-aaa.../ wss://work-2-bbb.../'
    # oder eine Datei, eine URL pro Zeile:
    export SANDBOX_TERM_URLS_FILE=/etc/odroid-cmd-agent.urls
    python3 cmd_agent.py        # Ziele kommen aus $SANDBOX_TERM_URLS(_FILE)

Ein einzelnes Ziel via Argument/$SANDBOX_TERM_URL bleibt als Fallback erhalten.
Alle Ziele teilen sich die Token-Variable $SANDBOX_TERM_TOKEN.

Verhalten:
  - Verbindet AUSGEHEND zur Sandbox (NAT-agnostisch); je Ziel eigener Reconnect.
  - Bei jedem Befehl: Anzeige + Rueckfrage  ->  [y]=ja  [N]=nein  [a]=immer ja
  - Ctrl-C schaltet die Bestaetigungspflicht AUS/EIN (Pause-Modus).
  - Antwort (Exit-Code + Ausgabe) geht zurueck an die jeweilige Sandbox.

Voraussetzung:  apt install -y python3-websockets
"""
import asyncio
import json
import os
import re
import signal
import subprocess
import sys
import urllib.parse

import websockets

AUTO = {"always": os.environ.get("CMD_AGENT_AUTO") == "1", "paused": False}


def tty_prompt(text):
    try:
        with open("/dev/tty", "w") as out, open("/dev/tty", "r") as inp:
            out.write(text)
            out.flush()
            return inp.readline().strip()
    except Exception:
        return ""


async def ask(cmd):
    loop = asyncio.get_running_loop()
    print("\n" + "=" * 60)
    print("BEFEHL AUS DER SANDBOX:")
    print("  " + cmd.replace("\n", "\n  "))
    print("=" * 60)
    if AUTO["always"] and not AUTO["paused"]:
        print("-> automatisch ausgefuehrt (immer-ja aktiv)")
        return True
    ans = await loop.run_in_executor(None, tty_prompt, "Ausfuehren? [y]=ja  [a]=immer ja  [N]=nein : ")
    ans = ans.lower()
    if ans in ("a", "always", "immer"):
        AUTO["always"] = True
        return True
    return ans in ("y", "yes", "j", "ja")


async def run_cmd(cmd, conn, timeout=1800):
    """Fuehrt den Befehl aus und schickt die Ausgabe LIVE zurueck (Fortschritt)."""
    try:
        p = await asyncio.create_subprocess_shell(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
    except Exception as exc:  # noqa: BLE001
        return {"code": None, "out": f"AUSFUEHRUNGSFEHLER: {exc}"}

    acc = []
    buf = b""

    async def stream(text):
        try:
            if conn.ws is not None:
                await conn.ws.send(json.dumps({"type": "progress", "id": conn.cid, "chunk": text}))
        except Exception:
            pass

    async def pump():
        nonlocal buf
        while True:
            chunk = await p.stdout.read(4096)
            if not chunk:
                break
            acc.append(chunk.decode(errors="replace"))
            buf += chunk
            while True:
                i = min([x for x in (buf.find(b"\n"), buf.find(b"\r")) if x >= 0] or [-1])
                if i < 0:
                    break
                line, buf = buf[:i + 1], buf[i + 1:]
                await stream(line.decode(errors="replace"))
        if buf:
            await stream(buf.decode(errors="replace"))

    try:
        await asyncio.wait_for(pump(), timeout=timeout)
    except asyncio.TimeoutError:
        p.kill()
        await stream("\n[TIMEOUT]\n")
        return {"code": None, "out": "".join(acc) + "TIMEOUT"}
    await p.wait()
    return {"code": p.returncode, "out": "".join(acc)}


def on_sigint(_sig, _frm):
    AUTO["paused"] = not AUTO["paused"]
    state = "AUS (Bestaetigung aus)" if AUTO["paused"] else "EIN (Bestaetigung noetig)"
    print(f"\n[Ctrl-C] Bestaetigungspflicht jetzt: {state}")


class Conn:
    """Eine Verbindung zu einer Sandbox."""

    def __init__(self, url):
        self.url = url
        self.ws = None
        self.cid = None


async def session(conn, token):
    q = urllib.parse.urlparse(conn.url)
    path = q.path.rstrip("/") + "/agent"
    query = urllib.parse.parse_qs(q.query)
    query["token"] = [token]
    full = q._replace(path=path, query=urllib.parse.urlencode(query, doseq=True)).geturl()
    async with websockets.connect(full, max_size=None, open_timeout=15) as ws:
        conn.ws = ws
        print(f"verbunden: {full.split('?')[0]}  (warte auf Befehle ...)")
        await ws.send(json.dumps({"type": "ready"}))
        async for message in ws:
            try:
                msg = json.loads(message)
            except ValueError:
                continue
            if msg.get("type") != "cmd":
                continue
            cid, cmd = msg.get("id"), msg.get("cmd", "")
            conn.cid = cid
            if await ask(cmd):
                res = await run_cmd(cmd, conn)
            else:
                print("-> ABGELEHNT")
                res = {"code": -1, "out": "ABGELEHNT (keine Bestaetigung)"}
            res.update({"type": "result", "id": cid})
            await ws.send(json.dumps(res))
            conn.cid = None


def next_delay(delay, exc, base=5, cap_dead=900, cap_live=60):
    """Backoff nach einem Fehlversuch (reine Funktion, host-getestet).

    Ein HTTP 404 heisst: die Sandbox gibt es nicht mehr -> sehr langsamer
    Backoff. Jeder andere Fehler (unser eigener Server war kurz weg) -> der
    normale, schnellere Backoff. Das ist der Unterschied, der verhindert, dass
    der Agent dauerhaft CPU gegen tote Register-Eintraege verbrennt."""
    gone = "404" in str(exc)
    return min(max(delay, base) * 2, cap_dead if gone else cap_live)


async def run_target(conn, token):
    """Haelt genau ein Ziel dauerhaft verbunden (eigener Reconnect/Backoff).

    Der Backoff ist kein Detail: das GitHub-Register listet jede Sandbox, die
    es je gab, und eine beendete Sandbox antwortet mit HTTP 404. Ohne Backoff
    laeuft der Agent dann in einer 5-s-Schleife gegen jeden toten Eintrag - das
    kostet dauerhaft CPU, hebt die Last und verhindert, dass die Kerne in den
    tiefen Takt fallen (gemessen: 9-14 % CPU, Last ~0,7). Deshalb:
      * 404 = Ziel existiert nicht mehr -> Backoff bis 900 s (sehr selten).
      * andere Fehler (unser eigener Server kurz weg) -> Backoff bis 60 s.
      * nach einem erfolgreichen Connect wieder bei 5 s beginnen."""
    delay = 5
    while True:
        try:
            await session(conn, token)
        except Exception as exc:  # noqa: BLE001
            print(f"[{conn.url}] {exc} - neuer Versuch in {delay}s ...")
            await asyncio.sleep(delay)
            delay = next_delay(delay, exc)
        else:
            delay = 5
            await asyncio.sleep(5)
        finally:
            conn.ws = None


def read_targets():
    """Ziel-URLs sammeln: $SANDBOX_TERM_URLS, dann $SANDBOX_TERM_URLS_FILE,
    dann die zentrale GitHub-Registry ($SANDBOX_TERM_REGISTRY), sonst Fallback
    Argument/$SANDBOX_TERM_URL. Duplikate werden entfernt."""
    urls = []
    for part in re.split(r"[,\s]+", os.environ.get("SANDBOX_TERM_URLS", "")):
        if part.strip():
            urls.append(part.strip())
    path = os.environ.get("SANDBOX_TERM_URLS_FILE", "")
    if path and os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if line and not line.startswith("#"):
                urls.append(line)
    # Zentrale Registry: der Odroid holt sich die Sandbox-Liste selbst.
    urls.extend(read_registry())
    if not urls:
        single = (sys.argv[1] if len(sys.argv) > 1 else "") or os.environ.get("SANDBOX_TERM_URL", "")
        if single:
            urls.append(single)
    seen, out = set(), []
    for u in urls:
        if u not in seen:
            seen.add(u)
            out.append(u)
    return out


def read_registry():
    """Sandbox-URLs aus dem GitHub-Register lesen (oeffentlich, ohne Token).
    $SANDBOX_TERM_REGISTRY = Roh-URL, z.B.
      https://raw.githubusercontent.com/<owner>/<repo>/main/remote-shell/memory/sandboxes.json
    Fehler werden still ignoriert (Kanal darf nie an der Registry scheitern)."""
    src = os.environ.get("SANDBOX_TERM_REGISTRY", "")
    if not src:
        return []
    try:
        import urllib.request
        with urllib.request.urlopen(src, timeout=15) as r:
            data = json.loads(r.read())
        return [v["url"] for v in (data.get("sandboxes") or {}).values() if v.get("url")]
    except Exception as exc:  # noqa: BLE001
        print(f"[registry] nicht lesbar: {exc}")
        return []


async def main():
    token = os.environ.get("SANDBOX_TERM_TOKEN", "")
    if not token:
        raise SystemExit("SANDBOX_TERM_TOKEN nicht gesetzt.")
    signal.signal(signal.SIGINT, on_sigint)

    # Erststart: falls gar keine Ziele bekannt sind, ist das ein Fehler.
    initial = read_targets()
    if not initial:
        raise SystemExit(
            "Kein Ziel. Aufruf: cmd_agent.py 'wss://work-<n>-<id>.prod-runtime.all-hands.dev/'"
            "  (oder $SANDBOX_TERM_URL / $SANDBOX_TERM_URLS / $SANDBOX_TERM_URLS_FILE"
            " / $SANDBOX_TERM_REGISTRY setzen)"
        )
    print(f"Ziele beim Start ({len(initial)}): " + ", ".join(u.split('?')[0] for u in initial))

    conns = {}      # url -> Conn
    tasks = {}      # url -> asyncio.Task

    async def ensure(u):
        if u in tasks and not tasks[u].done():
            return
        c = conns.get(u) or Conn(u)
        conns[u] = c
        tasks[u] = asyncio.create_task(run_target(c, token))

    for u in initial:
        await ensure(u)

    # Regelmaessig das Register neu lesen -> neue Sandboxes automatisch.
    interval = int(os.environ.get("SANDBOX_TERM_REGISTRY_INTERVAL", "60"))
    while True:
        await asyncio.sleep(interval)
        for u in read_targets():
            await ensure(u)


if __name__ == "__main__":
    asyncio.run(main())
