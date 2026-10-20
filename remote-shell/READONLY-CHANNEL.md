# Read-only Kanal zum Odroid (fuer Aufrufer OHNE Werkzeuge)

Ziel: ein einfacher Chat/Browser (z.B. ChatGPT im normalen Web-Browsing, ohne
MCP/Plugin) soll den Odroid **beobachten** koennen - Status, Logs, Temperaturen -
ohne dass ein Fehler Schaden anrichten kann. Es gibt genau einen Endpunkt, der
nur reine Lese-Befehle ausfuehrt.

## Aufruf

```
GET https://work-2-<sandbox-id>.prod-runtime.all-hands.dev/ro?token=<TOKEN>&c=<befehl>
```

* `<TOKEN>` = `SANDBOX_TERM_TOKEN` (derselbe Token wie der Kommando-Kanal).
* `<befehl>` = ein erlaubter Lese-Befehl, URL-kodiert (Leerzeichen = `+`).
* Ohne `c` liefert der Endpunkt die Hilfe (Liste der erlaubten Befehle).

Beispiele:

```
/ro?token=...&c=uptime
/ro?token=...&c=cat+/proc/cpuinfo
/ro?token=...&c=journalctl+-u+hassio-supervisor.service+-n+50
/ro?token=...&c=df+-h
/ro?token=...&c=dmesg+|+tail+-20
```

Antwort: **reiner Klartext** (die Ausgabe des Befehls), Status `200`.
Fehler kommen als Text mit passendem Status: `403` (Token/Befehl abgelehnt),
`503` (kein Odroid-Agent verbunden), `504` (Timeout).

## Was erlaubt ist

Die Policy steht in `ro_policy.py` und ist eine **strikte Allowlist**:

* nur die dort gelisteten reinen Lese-Binaries (`cat`, `grep`, `journalctl`,
  `df`, `lsblk`, `sensors`, `systemctl status`, `btrfs ... list/usage`, ...);
* Pipeline mit `|` ist erlaubt, aber **jedes** Segment wird einzeln geprueft;
* verboten sind `;` `&` `` ` `` `$` `>` `<` `\` `{` `}` (keine Verkettung,
  keine Substitution, keine Umleitung);
* verboten sind Schreib-/Aenderungsbefehle (Regex-Wortgrenzen: `rm`, `reboot`,
  `systemctl restart`, `apt`, `dd`, `curl`, `wget`, `python`, `sudo`, ...);
* geschuetzte Pfade werden nie ausgeliefert: `/etc/shadow`, `~/.ssh/`, private
  Keys, der Kanal-Token selbst.

Alles, was nicht ausdruecklich erlaubt ist, wird abgelehnt.

## Zwei Gates (Defence in depth)

1. **Server** (`sandbox_term_server.py`, `process_request` -> `_ro_handler`):
   prueft Token und `ro_policy.check`, bevor ein Befehl ueberhaupt rausgeht.
2. **Board** (`cmd_agent.py`): prueft die Policy ein **zweites Mal**, bevor es
   einen als `"ro": true` markierten Befehl **ohne Rueckfrage** ausfuehrt.

Ein normales (nicht-`ro`) Kommando laeuft weiterhin nur nach lokaler Bestaetigung
am Odroid. Der `ro`-Kanal ist der einzige Weg ohne Rueckfrage - und er ist auf
Lese-Befehle beschraenkt.

## Grenzen / Warnung

* Der Endpunkt macht aus dem Heim-Odroid eine **aus dem Internet erreichbare**
  Lese-Schnittstelle. Er ist token-gated, aber der Token steht in der URL und
  kann in Logs/History landen. Token behandeln wie ein Passwort; bei Verdacht
  rotieren (Datei `remote-shell/token` auf der Sandbox ueberschreiben, der Server
  liest den Token pro Verbindung neu).
* Es gibt **bewusst keinen Schreibpfad** in diesem Kanal. Schreibende Befehle
  laufen weiter ueber `cmd_send.sh` mit lokaler Bestaetigung.
* Read-only heisst nicht harmlos: `cat` einer grossen Datei oder ein voller
  `journalctl` kann viel Text liefern. Deshalb `-n`/`head`/`tail` verwenden.

## Test

```
python3 remote-shell/test_ro_policy.py
```

Laeuft auch in CI (`.github/workflows/esp-idf-build.yml`, Job `host-tests`).
