# Off-Board-Aktor fuer den externen Watchdog

**Status:** Design + Adapter. Reine Repo-Arbeit, **kein Board-Zugriff**.
Nicht belegte Punkte sind als *HYPOTHESE* markiert.

Referenziert (nicht veraendert):
`remote-shell/ha/WATCHDOG-GAP.md` (Befund + Haertungsplan, **Abschnitt 8** =
Parent-Verifikation: H2/H1 widerlegt, `bootstatus` unbrauchbar, **H3** „Reset
kam nicht durch" beste verbleibende Erklaerung),
`remote-shell/ha/watchdog_monitor.py` (Beobachter, reine `decide()`-Logik),
`remote-shell/ha/watchdog-heartbeat.{sh,service,timer}` (Board-Emitter),
`remote-shell/ha/deploy_heartbeat.sh`.

---

## 1. Warum der Aktor der entscheidende Teil ist

`WATCHDOG-GAP.md` Abschnitt 5.2 trennt den externen Pfad in **zwei** Bausteine:

1. **Beobachter** (off-board): sieht, dass der Heartbeat ausbleibt
   (`watchdog_monitor.py`).
2. **Aktor** (off-board): unterbricht dem haengenden Board den Strom bzw. zieht
   die Reset-Leitung.

Der Aktor ist der Teil, der **wirklich** hilft. `reboot -f` ueber denselben
haengenden SoC scheidet aus — er kann nicht mehr ausfuehren. Genau das ist der
Kern von H3: der interne 60-s-Watchdog *feuerte* vermutlich, aber die
Reset-/Boot-ROM-Domain hing im defekten PLL-Takt und der Reset kam nicht durch.
Ein Aktor muss daher **ausserhalb** dieser Domain liegen: ein Relais, das die
Stromzufuhr auftrennt, oder eine Reset-Leitung, die ein *zweiter* Rechner zieht.

Dieses Dokument beantwortet die Frage aus `WATCHDOG-GAP.md` Abschnitt 7.4:
**Was ist der Aktor, und wie wird er sicher angeschlossen?**

---

## 2. Die drei Anschluss-Optionen

Kurzvergleich (Details in 2.1–2.3, Gewichtung in Abschnitt 3):

| | (a) Schaltbare Steckdose | (b) USB-Relais/GPIO am 2. Rechner | (c) Reset-Leitung |
|---|---|---|---|
| Wirkt auf | Netzstrom (echter Power-Cycle) | Netzstrom (echter Power-Cycle) | Reset-Pin (kein Power-Cycle) |
| Trennt PLL-Domain? | **ja** | **ja** | **nein** (H3-Risiko) |
| Neue Hardware | Steckdose (vorhanden?) | Relais + 2. Rechner | Draht an Header |
| Angriff/Fehlausloesung | Stromausfall Board | Stromausfall Board | Reboot mitten im Betrieb |
| Erreichbar aus der Sandbox | ja (HTTP/MQTT) | ja (ueber 2. Rechner) | nein (physisch) |
| Fail-safe moeglich | ja (stromlos = Board aus **oder** an, s. 2.1) | ja | teilweise |

> **Kernregel:** Jede Fehlausloesung ist ein **Stromausfall** (a/b) bzw. ein
> **unerwarteter Reset** (c). Der Aktor braucht deshalb **zwei** Bremsen:
> ein **Cooldown** (zeitlich) und ein **Quorum** (Mehrheit unabhaengiger
> Bestaetigungen). Beides ist in `watchdog_actor.py` implementiert
> (Abschnitt 5) und in Abschnitt 4 beschrieben.

### 2.0 Voraussetzung bei allen Optionen: Heartbeat-Transport

Der Beobachter braucht den Board-Heartbeat (`watchdog-heartbeat.timer` schreibt
ihn nach `/run/board-heartbeat`). Der Beobachter ist **off-board**, also muss
der Heartbeat den SoC verlassen. `WATCHDOG-GAP.md` 5.2 nennt zwei Wege:

- **MQTT retained** Topic (z. B. `odroid/heartbeat`) — passt zu Option (a) und
  (b), wenn dort ohnehin ein Broker laeuft; der Beobachter abonniert und
  **stempelt den Empfang** (nie die Board-Uhr).
- **Datei, die der Kanal abholt** (scp/rsync/HTTP-Pull) — passt zur
  Sandbox-Kanal-Variante.

*Wichtig:* Der Aktor (dieses Dokument) und der Heartbeat-Transport sind
getrennte Fragen. Der Transport darf **nicht** ueber denselben Pfad laufen, den
der Aktor abschaltet (sonst schneidet der Aktor seine eigene Rueckmeldung ab).
Der Monitor stempelt den **Empfang**, nicht die Sender-Zeit — eine falsche
Board-Uhr darf keinen Fehlalarm erzeugen (siehe `watchdog_monitor.py`).

### 2.1 (a) Schaltbare Steckdose (Tasmota / ESPHome / HA-Switch)

**Idee:** Eine smarte Steckdose, an der das Board-Netzteil haengt. Der Aktor
schaltet sie aus (Power-Cut) und wieder ein.

**Voraussetzungen**

- Steckdose mit **lokalem** HTTP- oder MQTT-Interface: Tasmota (`/cm?cmnd=...`),
  ESPHome (REST-API oder MQTT), oder ein HA-Switch (via HA-REST/WebSocket).
- Feste IP / erreichbarer Hostname vom Beobachter aus; **kein Cloud-Zwang** —
  ein Cloud-Relay macht den Aktor von einem Fremdsystem abhaengig.
- Falls die Steckdose das Board **und** den Beobachter versorgt: **nicht
  dieselbe Steckdose** verwenden (sonst schaltet sich der Beobachter selbst ab).
- Auth-Token/Passwort, wenn konfiguriert (Tasmota `WebPassword`, ESPHome
  `api`/`web_server` Auth).

**Anschluss**

HTTP (Tasmota, Power togglen oder gezielt aus/ein):

```sh
# Power-Cut: erst AUS, dann nach Puls-Sekunden EIN (siehe 2.3-Puls-Hinweis)
python3 watchdog_actor.py --kind http \
  --url 'http://plug.local/cm?cmnd=Power%20OFF' --execute
# ... und der Gegenbefehl --url '...cmnd=Power%20ON'
```

MQTT (Tasmota/ESPHome, Broker noetig):

```sh
python3 watchdog_actor.py --kind mqtt \
  --topic 'cmnd/tasmota-plug/Power' --payload 'OFF' --execute
```

**Sicherheitsrisiko**

- **Fehlausloesung = Stromausfall** fuer alles an der Steckdose. Ist dort mehr
  als das Board angeschlossen (Router, Broker, NAS), faellt mehr aus als
  gewollt → **Steckdose nur fuer das Board**.
- **Netz-/WLAN-Aussetzer** sehen wie ein Hang aus → unnoetiger Power-Cut.
  Gegenmittel: `GRACE` gross genug + mehrere Fehlschlaege + Cooldown.
- **Steckdose selbst haengt** (Firmware-Reset, WLAN weg): Aktor wirkt nicht
  mehr. Die Steckdose ist ein eigener Single Point of Failure und sollte
  ueberwacht werden.
- **Harter Stromausfall** statt sauberem Reboot: Risiko fuer das Dateisystem
  (eMMC/SD-Karte) und das Netzteil (Einschaltstrom). Deshalb **Cooldown** und
  ein **Puls** (nicht Dauer-Aus).

**Cooldown / Quorum**

- Puls statt Dauer-Aus: AUS, `pulse_seconds` warten, EIN. `build_pulse_command`
  (fuer `command`) bzw. zwei HTTP-Calls; ein zu langer AUS-Zustand ist
  gefaehrlicher als ein zu kurzer.
- `--cooldown` gross (>= 900 s), Monitor-`--cooldown` gleich setzen.
- Quorum optional: eine zweite unabhaengige Bestaetigung (z. B. „HA sieht das
  Board offline" **und** „Heartbeat fehlt") bevor geschaltet wird.

### 2.2 (b) USB-Relais / GPIO an einem zweiten Rechner

**Idee:** Ein zweiter, unabhaengiger Rechner (der **Odroid-MCU/ESP32**, ein
Raspberry Pi, oder ein kleiner MCU) haelt ein Relais, das die Netzspannung bzw.
das Netzteil des Boards schaltet. Der Beobachter schickt ihm einen Befehl
(HTTP, MQTT oder `ssh`), das Relais zieht fuer den Puls an.

**Voraussetzungen**

- **Ein zweiter Rechner**, der **nicht** vom Board abhaengt (eigene
  Stromversorgung!). Ein MCU (ESP32/ESP8266) mit Netzteil ist ideal: er
  ueberlebt den Hang des Boards.
- **Relais**, das die Netzspannung schalten darf: ein fertiges
  **USB-Relais** (z. B. HID-Relais, `/dev/hidraw*`) oder ein
  SSR/Mechanik-Relais am GPIO. **Sicherheitshinweis:** Netzspannung (230 V)
  nur mit geeigneter, gepruefter Hardware; im Zweifel **nur die
  DC-Seite/Netzteil-Eingang** schalten.
- Betriebsrechte fuer den Aktor-Prozess (`udev`-Regel fuer das USB-Relais,
  `gpio`-Gruppe fuer GPIO) — nicht als `root` betreiben, wenn vermeidbar.
- Ein Weg vom Beobachter zum 2. Rechner: HTTP-Endpunkt, MQTT-Topic, oder `ssh`
  mit keybasiertem Login (kein Passwort-Prompt).

**Anschluss**

`command`-Aktor mit Puls (USB-Relais-CLI bzw. GPIO-Tooling):

```sh
python3 watchdog_actor.py --kind command \
  --off-cmd 'usbrelay REL1=0' --on-cmd 'usbrelay REL1=1' \
  --pulse-seconds 5 --execute --state-file /var/lib/watchdog/actor-state
```

Oder ueber den zweiten Rechner per `ssh` (das ist genau der `--reboot-cmd`-Pfad
des Monitors):

```sh
ssh powercut@relay-host 'usbrelay REL1=0; sleep 5; usbrelay REL1=1'
```

Auf einem ESP32 als Relais-Controller ist der Aktor ein HTTP-Call an den MCU
(z. B. ESPHome `web_server` oder ein minimaler HTTP-Handler):

```sh
python3 watchdog_actor.py --kind http --url 'http://relay-mcu.local/powercycle' \
  --timeout 5 --execute
```

**Sicherheitsrisiko**

- **Fehlausloesung = Stromausfall.** Wie (a); zusaetzlich: ein **falsch
  verdrahtetes** Relais kann die falsche Dose schalten → vor Inbetriebnahme
  **Trockenlauf** (Abschnitt 5) und eine markierte Testdose.
- **Relais haengt** (klebt) → Board bleibt aus. Fail-safe: so verdrahten, dass
  **stromlos = Board EIN** (Relais muss *anziehen*, um abzuschalten). Dann
  bleibt bei einem Controller-Ausfall das Board versorgt.
- **`ssh`-Haenger**: kein Timeout → der Monitor blockiert im Aktor. Timeout
  setzen (`ConnectTimeout`, `-o BatchMode=yes`) und das Relais so bauen, dass
  ein **abgebrochener** Puls nicht dauerhaft aus ist.
- **Der 2. Rechner ist ein neuer Single Point of Failure** — faellt er aus,
  gibt es keine Sicherung (oder schlimmer: er loest selbst aus).

**Cooldown / Quorum**

- Puls (AUS→warten→EIN) als **ein** Befehl, damit ein Abbruch nach dem AUS
  nicht das Board dauerhaft abschaltet (`build_pulse_command`).
- `--cooldown` und `--state-file` (durable ueber Neustarts, Abschnitt 5).
- Quorum ueber zwei Sensoren: z. B. MCU-Heartbeat **und** Board-Heartbeat.

### 2.3 (c) Reset-Leitung

**Idee:** Statt den Strom zu trennen, zieht der Aktor den **Reset-Pin** des
Boards (Header) auf GND (Open-Drain), wie der Reset-Taster.

**Voraussetzungen**

- Zugang zum **Reset-Header** des Boards (Pinout/Schaltplan) und ein
  **gemeinsames GND** zwischen Board und Aktor-Hardware.
- Open-Drain/Open-Collector-Stufe oder Optokoppler — **kein** Push-Pull-Treiber
  gegen die Board-Versorgung (sonst Strom in den Pin).
- Relais/Transistor am 2. Rechner oder MCU-GPIO; `pulse_seconds` kurz (z. B.
  0,5–1 s reicht fuer einen Reset).

**Anschluss**

```sh
python3 watchdog_actor.py --kind command \
  --off-cmd 'gpioset gpiochip0 17=1' --on-cmd 'gpioset gpiochip0 17=0' \
  --pulse-seconds 1 --execute
```

**Sicherheitsrisiko**

- **Kein Power-Cut.** Genau das ist das H3-Problem: haengt die PLL und nimmt
  die Reset-/Boot-ROM-Domain mit, **kommt der Reset nicht durch** (siehe
  `WATCHDOG-GAP.md` 8.6). Die Reset-Leitung ist damit die **schwaechste**
  Option fuer *diesen* Fehlerfall.
- **Fehlausloesung = Reset mitten im Betrieb** → Datenverlust, kein sauberer
  Shutdown. Weniger drastisch als ein Power-Cut, aber immer noch schaedlich.
- **Pegel/Logikfehler** koennen das Board in einen dauerhaften Reset ziehen
  (Reset „festgehalten") → Board tot, bis jemand physisch eingreift.

**Cooldown / Quorum**

- Wegen des H3-Risikos nur als **Ergaenzung** zum Power-Cut, nicht als Ersatz.
- Kurzer Puls (<= 1 s), langes Cooldown, Quorum **zwingend** (da der Nutzen
  unsicher ist, darf der Fehlalarm nicht leichtfertig passieren).

---

## 3. Empfehlung / Gewichtung

1. **Erste Wahl: (a) schaltbare Steckdose** — echter Power-Cut, oft schon
   vorhanden, ohne neue Hardware und aus der Sandbox per HTTP/MQTT erreichbar.
   Grenze: trennt nur, wenn sie *nur* das Board versorgt.
2. **Zweite Wahl: (b) USB-Relais/GPIO am 2. Rechner** — gleicher Effekt, mehr
   Kontrolle (eigener MCU), aber neue Hardware und ein zweiter Rechner, der
   selbst ueberwacht werden muss.
3. **Nur Ergaenzung: (c) Reset-Leitung** — wegen H3 unsicher fuer einen
   PLL-Hang; als zusaetzlicher Pfad sinnvoll, wenn Power-Cut nicht moeglich ist.

Unabhaengig von der Wahl bleibt der Hinweis aus `WATCHDOG-GAP.md` 5.4/8.6
gueltig: **der interne Watchdog bleibt noetig.** Der externe Pfad ergaenzt,
ersetzt nicht; H1/H7 sollten zusaetzlich direkt gefixt werden
(`nowayout=1`, `hung_task_panic=1`).

---

## 4. Sicheres Cooldown-/Quorum-Konzept

Beide Bremsen greifen **vor** jedem realen Schalten. `watchdog_actor.py`
implementiert sie als reine Funktionen (`evaluate`, `cooldown_ok`, `quorum_ok`).

### 4.1 Cooldown

- Der **Monitor** hat bereits einen Cooldown (`--cooldown`, Default 600 s) und
  feuert den Aktor nur bei `action == "reboot"`.
- Der **Aktor** hat einen **zweiten, durablen** Cooldown ueber
  `--state-file`. Grund: der Monitor haelt `last_reboot` nur **im RAM**. Startet
  er neu (z. B. `Restart=always` nach einem Absturz), vergisst er den Cooldown
  und koennte sofort wieder feuern. Die **Datei** des Aktors ueberlebt das.
- Beide Cooldowns **gleich** setzen (z. B. 900 s), damit sich die Systeme nicht
  widersprechen.

### 4.2 Quorum

- `--min-quorum N` verlangt **N** unabhaengige Bestaetigungen
  (`--quorum-votes true,true`).
- Quorum wird **vor** dem Cooldown geprueft: fehlt es, meldet der Aktor
  „Quorum nicht erreicht" und schaltet nicht.
- Sinnvolle Quellen: Board-Heartbeat **und** eine zweite Beobachtung (HA sieht
  das Board offline, Ping aus, MQTT-LWT `offline`). Ein einzelner Kanal-Aussetzer
  darf keinen Power-Cut ausloesen.

### 4.3 Weitere Leitplanken

- **Trockenlauf ist der Default.** Ohne `--execute` sendet der Aktor **nichts**.
  Erst nach Trockenlauf und markierter Testdose scharf schalten.
- **Puls, kein Dauer-Aus** (`build_pulse_command`): AUS→`pulse_seconds`→EIN in
  **einem** Befehl, damit ein Abbruch nach dem AUS nicht das Board abschaltet.
- **Fail-safe-Verdrahtung:** stromlos = Board **versorgt** (Relais zieht zum
  Abschalten an). Ein toter Controller laesst das Board dann laufen.
- **Erst nach dem Cut pruefen:** Der Monitor sollte nach einem Reboot einen
  **neuen `boot_id`** verlangen, bevor er wieder als „ok" gilt (macht
  `watchdog_monitor.decide` bereits ueber den Boot-ID-Wechsel).
- **Zeitstempel vom Beobachter**, nicht vom Board (falsche Board-Uhr).

---

## 5. Der Adapter: `watchdog_actor.py`

Reine, host-testbare Logik — der eigentliche Aktor wird **nicht** ausgeloest.
Drei Aktor-Arten:

| `--kind` | Baut | Ausfuehrung (nur mit `--execute`) |
|---|---|---|
| `http` | `{url, method, body, headers, timeout}` | `urllib.request` (GET/POST/PUT) |
| `mqtt` | `mosquitto_pub`-`argv` | `subprocess.call(argv)` |
| `command` | `/bin/sh -c <command>` | `subprocess.call(argv)` |

Reine Funktionen (in `test_watchdog_actor.py` ohne Mocks geprueft):

- `build_http_request(url, method, body, headers, timeout)` — setzt
  `Content-Length`/`Content-Type` korrekt (auch POST ohne Body).
- `build_mqtt_request(topic, payload, retain, username)` — baut die
  `mosquitto_pub`-Argumente, **verbindet keinen Broker**.
- `build_pulse_command(off_cmd, on_cmd, pulse_seconds)` — AUS→`sleep`→EIN.
- `build_plan(kind, ...)` / `format_plan(plan)` — „was wuerde passieren".
- `cooldown_ok`, `quorum_ok`, `evaluate` — die Bremsen (Abschnitt 4).

Beispiele:

```sh
# Trockenlauf (Default): zeigt nur den Plan, sendet nichts.
python3 watchdog_actor.py --kind http --url 'http://plug.local/cm?cmnd=Power%20OFF'

# MQTT
python3 watchdog_actor.py --kind mqtt --topic 'cmnd/tasmota-plug/Power' \
  --payload 'OFF'

# USB-Relais-Puls, mit durablem Cooldown
python3 watchdog_actor.py --kind command \
  --off-cmd 'usbrelay REL1=0' --on-cmd 'usbrelay REL1=1' --pulse-seconds 5 \
  --cooldown 900 --state-file /var/lib/watchdog/actor-state

# Quorum: erst mit zwei Bestaetigungen schalten
python3 watchdog_actor.py --kind http --url 'http://plug.local/cm?cmnd=Power%20OFF' \
  --min-quorum 2 --quorum-votes 'true,true'

# Scharf schalten (erst nach Trockenlauf!)
python3 watchdog_actor.py --kind http --url 'http://plug.local/cm?cmnd=Power%20OFF' \
  --execute
```

Exit-Codes: `0` ok/blockiert/Trockenlauf, `1` Aktor fehlgeschlagen, `2`
ungueltiger Plan (fehlende `--url`/`--topic`/`--command`, halber Puls).

### 5.1 Zusammenspiel mit `watchdog_monitor.py` (unveraendert)

Der Monitor startet den Aktor ueber `--reboot-cmd` (Shell-Befehl). Empfohlen ist
ein **Wrapper**, damit die verschachtelten Anfuehrungszeichen nicht zur
Fehlerquelle werden:

```sh
# /usr/local/sbin/watchdog-powercut.sh  (off-board, vom Monitor gerufen)
#!/bin/sh
exec /usr/bin/python3 /usr/local/sbin/watchdog_actor.py \
  --kind command --off-cmd '/usr/bin/usbrelay REL1=0' \
  --on-cmd '/usr/bin/usbrelay REL1=1' --pulse-seconds 5 \
  --cooldown 900 --state-file /var/lib/watchdog/actor-state --execute
```

```sh
python3 watchdog_monitor.py --heartbeat /var/lib/watchdog/board-heartbeat \
  --interval 60 --grace 30 --miss-needed 3 --cooldown 900 \
  --reboot-cmd /usr/local/sbin/watchdog-powercut.sh
```

---

## 6. Produktivstart in der `watchdog-heartbeat.timer`-Umgebung

Die **Board-Seite** bleibt wie gemergt: `watchdog-heartbeat.timer` schreibt den
Heartbeat (`/run/board-heartbeat`). Der **Beobachter** laeuft auf dem
**OFF-BOARD-Host** (Sandbox-Kanal, zweiter Rechner oder MCU). Hier ein
systemd-Unit-Vorschlag fuer diesen Host.

`/etc/systemd/system/watchdog-monitor.service`:

```ini
[Unit]
Description=Off-board watchdog monitor (external heartbeat + power-cut actor)
Documentation=file:/usr/local/share/remote-shell/WATCHDOG-ACTOR.md
# Netz nicht zwingend (der Heartbeat kann eine lokale Datei sein), aber der
# Aktor (HTTP/MQTT) braucht es meist -> nach dem Netz starten.
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
# Heartbeat + Aktor + Cooldown muessen zu watchdog-heartbeat.timer passen
# (INTERVAL 60 s). --reboot-cmd ruft den Wrapper (Abschnitt 5.1), der den
# Aktor mit --execute und durablem Cooldown startet.
ExecStart=/usr/bin/python3 /usr/local/sbin/watchdog_monitor.py \
    --heartbeat /var/lib/watchdog/board-heartbeat \
    --interval 60 --grace 30 --miss-needed 3 --cooldown 900 \
    --reboot-cmd /usr/local/sbin/watchdog-powercut.sh
# Der Beobachter ist selbst ein Single Point of Failure -> immer neu starten.
Restart=always
RestartSec=10
StateDirectory=watchdog

# Haertung: der Monitor braucht nur Datei lesen + Aktor rufen.
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
ReadWritePaths=/var/lib/watchdog

[Install]
WantedBy=multi-user.target
```

Installation / Betrieb:

```sh
sudo install -m 0755 watchdog_monitor.py  /usr/local/sbin/watchdog_monitor.py
sudo install -m 0755 watchdog_actor.py    /usr/local/sbin/watchdog_actor.py
sudo install -m 0755 watchdog-powercut.sh /usr/local/sbin/watchdog-powercut.sh
sudo install -m 0644 watchdog-monitor.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now watchdog-monitor.service
journalctl -u watchdog-monitor -f     # Trockenlauf zuerst pruefen!
```

**Reihenfolge (wichtig):**

1. **Trockenlauf**: erst ohne `--execute` (Abschnitt 5) und mit
   `--reboot-cmd` **leer** starten → der Monitor loggt nur „waere rebootet".
2. Board-Heartbeat-Transport pruefen (Abschnitt 2.0): kommt der Heartbeat beim
   Beobachter an, und **aendert er sich** (seq steigt)?
3. Aktor-Wrapper mit markierter **Testdose** und `--execute` testen.
4. Erst dann am echten Board scharf schalten. Cooldowns (`--cooldown` Monitor
   **und** Aktor) gleich setzen.

**Grenzen / Betriebsnotizen**

- Der Beobachter ist ein **neuer Single Point of Failure**; `Restart=always` +
  Ueberwachung des Dienstes sind Pflicht.
- `GRACE` > erwartete Boot-Dauer waehlen, sonst deutet ein langsamer Boot als
  Hang → Reboot-Schleife (`WATCHDOG-GAP.md` 5.4).
- Der Aktor-Wrapper sollte `--state-file` in einem **persistenten** Verzeichnis
  fuehren (`/var/lib/watchdog`), damit der Cooldown einen Monitor-Neustart
  ueberlebt.
- Der interne HW-Watchdog bleibt zusaetzlich noetig (Abschnitt 3).

---

## 7. Abgrenzung (was dieses Dokument NICHT tut)

- **Kein Board-Zugriff** und **kein** `remote-shell`-Kanal (`tools/remote-shell/*`,
  `cmd_send.sh`, `/dev/mem`, `systemctl` am Board). Der Kanal ist geteilt;
  parallele Nutzung fuehrt zu doppelt ausgefuehrten Befehlen und Haengern.
- `watchdog_monitor.py`, `test_watchdog_monitor.py`, `watchdog-heartbeat.*` und
  `WATCHDOG-GAP.md` bleiben **unveraendert** (nur referenziert).
- Der reale Aktor wird **nicht** ausgeloest; alles ist Trockenlauf-faehig.

## 8. Testbarkeit

```sh
python3 remote-shell/ha/test_watchdog_actor.py
python3 remote-shell/ha/test_watchdog_monitor.py   # unveraendert
```

Beide sind in `test/run_host_tests.sh` eingehaengt und laufen ohne Board, ohne
Netz und ohne Mocks (reine Logik). Der echte Power-Cut braucht Hardware und ist
absichtlich **nicht** Teil der Tests.

## 9. Offene Punkte (an den Parent)

1. **Aktor festlegen** (Abschnitt 3): schaltbare Steckdose vorhanden? Nur fuer
   das Board? Erreichbar per HTTP oder MQTT?
2. **Heartbeat-Transport** (Abschnitt 2.0) waehlen (MQTT retained oder
   Datei-Pull) und testen, dass die `seq` steigt.
3. **Fail-safe-Verdrahtung** bestaetigen (stromlos = Board versorgt).
4. **Cooldowns** festlegen (Vorschlag 900 s) und in Monitor **und** Aktor gleich
   setzen.
5. Erst danach scharf schalten; vorher Trockenlauf mit markierter Testdose.
