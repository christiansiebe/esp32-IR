# Watchdog-Luecke nach einem CPU-Hang (Odroid N2+)

**Status:** Analyse + Haertungs-Vorschlag. Reine Repo-Arbeit, **kein Board-Zugriff**.
Alles, was nicht direkt beobachtet oder in einer Datei belegt ist, steht unter
*HYPOTHESE* und ist als solche markiert.

Referenziert (nicht veraendert):
`remote-shell/ha/PLL-OC-README.md` (der Hang selbst),
`remote-shell/memory/SHARED.md`, `remote-shell/ha/UV-README.md`,
`remote-shell/ha/LOWFREQ-README.md` (Watchdog-Zusagen),
das (repo-fremde) `watchdog-guard.timer` auf dem Board.

---

## 1. Befund

### 1.1 BELEGT (real beobachtet, 2026-10-18)

- Ein **PLL-Hang bei 2532 MHz** (big-Cluster, N=211; siehe `PLL-OC-README.md`)
  hat das Board zum Stehen gebracht. Es lief danach wieder normal (Stock-Takt).
- Der **60-s-Hardware-Watchdog hat das Board NICHT rebootet.** Erwartet war ein
  Reboot nach ~60 s.
- `journalctl --list-boots` zeigt eine **Luecke von ~66 min**:
  Vor-Boot endete **02:15:01**, der naechste Boot startete **03:21:33**
  (= 3992 s, ~66,5 min). Das ist der belegte Kern des Befunds.
- Aus dem Tagesprotokoll (`.openhands/memory/2026-10-18.md`): das Board war
  gefuehlt **~7 h** ueber den Kanal nicht erreichbar; der Kanal-Agent kam erst
  nach einem **Server-Neustart in der Sandbox** zurueck.

### 1.2 Wichtige Praezisierung (ebenfalls BELEGT)

Die beiden Zahlen **widersprechen sich nicht, messen aber Verschiedenes**:

- **66 min** = Luecke zwischen zwei *Journal-Boots* — also die Zeit, in der das
  Board **nicht geloggt** hat.
- **~7 h** = Zeit, in der der **Kanal** (Sandbox-Server + Odroid-Agent) nicht
  antwortete.

Die Journal-Luecke ist die *harte* Zahl. Ein Board, das 66 min nicht loggt und
dann von selbst neu bootet, ist nicht dasselbe wie ein Board, das 7 h tot ist.
**Fuer die Watchdog-Frage zaehlt nur die Journal-Luecke.** (Die 7 h deuten
zusaetzlich auf ein Kanal-/Sandbox-Problem hin, das hier nicht das Thema ist.)

### 1.3 Die zugesagte Sicherheitsleine (BELEGT aus dem Repo)

| Mechanismus | Zusage im Repo | Quelle |
|---|---|---|
| systemd HW-Watchdog | `RuntimeWatchdogUSec=1min` | `SHARED.md`, `UV-README.md` |
| periodische Kontrolle | `watchdog-guard.timer` aktiv | `SHARED.md`, `UV-README.md` |
| Kernel-Panic | `kernel.panic=10` (`panic=10`) | `SHARED.md`, `UV-README.md` |
| HW-Watchdog-Device | `/dev/watchdog0` (Meson) | Meson-WDT-Treiber, nicht im Repo |

Der Repo-Anspruch („ein Hang rebootet das Board") ist durch diesen Befund
**widerlegt** — jedenfalls fuer diesen Hang.

---

## 2. Mechanismen: was greift wann (und was nicht)

### 2.1 systemd `RuntimeWatchdogUSec`

- systemd (PID 1) **oeffnet beim Boot** das Kernel-Watchdog-Device
  (`/dev/watchdog0`) und stellt dessen Timeout auf `RuntimeWatchdogUSec`.
- Danach **pingt systemd** das Device in einem festen Rhythmus (typisch die
  Haelfte des Timeouts). **Der Kernel pingt NICHT selbst.**
- Bleibt der Ping aus (PID 1 kommt nicht zum Laufen), **laeuft das Timeout ab
  und der Treiber/SOC rebootet**.
- Kann systemd das Device **nicht oeffnen** (z. B. weil es jemand anderes
  exklusiv haelt), loggt systemd eine Warnung und der **HW-Watchdog ist nicht
  scharf**.
- Fuer die **Reboot-/Shutdown-Phase** gibt es `RebootWatchdogUSec`
  (Default **10 min**): systemd stellt den Timeout um und pingt weiter, um einen
  *haengenden Reboot* abzufangen. Ein haengender Reboot wird also nicht nach
  60 s, sondern erst nach bis zu ~10 min zurueckgesetzt.

### 2.2 Meson-Watchdog `/dev/watchdog0`

- Amlogic-SoC-Watchdog-Treiber (`meson_wdt`), Device `/dev/watchdog0`,
  Sysfs unter `/sys/class/watchdog/watchdog0/`.
- Modul-Parameter **`nowayout`** (auf vielen Kernen **Default 0**): mit
  `nowayout=0` **entwaffnet ein `close()` des Devices den Watchdog**, solange
  nicht das „magic close"-Zeichen (`V`) geschrieben wurde. Der Watchdog ist
  damit *abschaltbar* — anders als bei `nowayout=1`, wo er nach dem ersten Arm
  nie mehr ausgeht.
- **`bootstatus`** (Bit `WDIOF_CARDRESET`) sagt beim *naechsten* Boot, ob der
  letzte Reset vom Watchdog kam. Das ist der entscheidende Nachweis — siehe
  Abschnitt 4.

### 2.3 `kernel.panic=10`

- `panic=10` (bzw. `kernel.panic=10`) bedeutet: **nach einem Kernel-Panic**
  rebootet der Kernel 10 s spaeter.
- Ein **stiller Hang** (CPU steht, keine Exception) erzeugt **keinen Panic**.
  `panic=10` hilft dort **ueberhaupt nicht**. Der Irrtum „panic=10 faengt einen
  Hang" ist genau die Luecke.
- Nur ein **Panic** (z. B. `panic_on_oops`, `hung_task_panic`,
  `softlockup_panic`, Watchdog mit `pretimeout`-Panic) zieht `panic=10` ueberhaupt
  heran.

### 2.4 `watchdog-guard.timer` (repo-fremd)

- Auf dem Board aktiv (laut `SHARED.md`), **nicht in diesem Repo**. Zweck hier
  nicht einsehbar (HYPOTHESE: prueft/heilt die Watchdog-Konfiguration
  periodisch).
- **HYPOTHESE:** Wenn dieser Timer das Watchdog-Device ebenfalls oeffnet und
  wieder schliesst, ist er mit `nowayout=0` ein *Kandidat fuer ein versehentliches
  Entwaffnen* (Abschnitt 3, H1) oder fuer ein „Device busy" beim systemd-Open
  (H2). **Nicht verifiziert** — dafuer muesste man das Unit am Board lesen
  (macht der Parent).

### 2.5 Warum ein PLL-Hang ueberhaupt anders ist

- Der **Arch-Timer** (`perf_counter`-Zeitbasis in `PLL-OC-README.md`) laeuft
  **PLL-unabhaengig** weiter. Zeit vergeht also weiter, auch wenn die CPU steht.
- Aber **jeder Ping braucht eine laufende CPU**. Steht der ganze SoC (nicht nur
  der Takt, sondern die Ausfuehrung), kommt **kein Ping mehr durch** → das
  Timeout *muesste* ablaufen → Reset.
- Genau das ist der Widerspruch zum Befund. Deshalb dreht sich alles um die
  Frage: **War der Watchdog ueberhaupt scharf, und konnte der Reset greifen?**

---

## 3. HYPOTHESEN: warum der HW-Watchdog nicht in 60 s feuerte

Klar getrennt: **keine erfundenen Messwerte.** Reihenfolge = absteigende
Plausibilitaet nach meiner Einschaetzung, jeweils mit Gegenprobe.

### H1 — Der Watchdog war **entwaffnet** (`nowayout=0` + `close()`)
Mit `nowayout=0` schaltet ein `close()` ohne Magic-Char den Watchdog ab. Wer
schliesst?
- ein **konkurrierender Opener** (z. B. `watchdog-guard.timer`, ein Testskript,
  `wdctl`), der das Device oeffnet und wieder schliesst, **oder**
- ein **Reboot-Pfad**: systemd schreibt beim geordneten Reboot `V`
  (Magic-Close → entwaffnen), der Reboot scheitert dann am PLL — das Board bleibt
  **ohne scharfen Watchdog** haengen.
**Gegenprobe:** `bootstatus` beim naechsten Boot = 0 (kein Watchdog-Reset), und
im Journal der letzten Minuten vor 02:15:01 ein `close`/`V`/„Watchdog" Hinweis.
**Passt gut** zu „66 min Luecke" (niemand kann mehr rebooten).

### H2 — systemd konnte das Device **nicht oeffnen** („Device busy")
Haelt ein anderer Prozess `/dev/watchdog0` exklusiv, bekommt systemd es nicht,
loggt eine Warnung und **armt den Watchdog nie**. Dann gibt es *keine*
60-s-Sicherung.
**Gegenprobe:** Journal-Suche nach `Failed to open /dev/watchdog`, `Device or
resource busy`, `watchdog`.

### H3 — Der Watchdog **feuerte**, aber der **Reset kam nicht durch**
Ein abgelaufenes Watchdog-Timeout *behauptet* einen Reset — es garantiert nicht,
dass der SoC sauber neu startet. Haengt die CPU-PLL (nie „lock"), kann die
Reset-/Boot-ROM-Logik im selben defekten Takt-Domain haengen bleiben: der Chip
sitzt in einem Reset-/Boot-Loop fest, bis ein **echter** Power-Cycle kommt.
**Gegenprobe:** `bootstatus` = 1 (Watchdog-Reset **war** die Ursache) *und*
trotzdem 66 min Luecke → der Reset hat lange gebraucht oder ist erst spaeter
gelungen.
**Passt** zum Umstand, dass das Board *von selbst* wieder kam (irgendwann).

### H4 — Das effektive Timeout war **laenger als 60 s**
Treiber runden Timeouts oft auf unterstuetzte Werte; ausserdem stellt systemd in
der **Shutdown-Phase** auf `RebootWatchdogUSec` (Default 10 min) um. Ein
haengender *Reboot* wird also nicht nach 60 s, sondern nach bis zu 10 min
zurueckgesetzt.
**Gegenprobe:** `cat /sys/class/watchdog/watchdog0/timeout` zur Laufzeit; Journal
`RebootWatchdog`. Erklaert **max. ~10 min, nicht 66 min** — daher nicht
allein ausreichend.

### H5 — „Der Kick-Task friert mit ein" (die genannte Hypothese)
Formal richtig, aber **in die falsche Richtung**: Wenn der Kick-Task einfriert,
**bleibt der Kick aus** → das Timeout laeuft ab → Reset. Das *erklaert keinen
ausgebliebenen* Reboot, es sei denn H1/H2/H3 gelten zusaetzlich.
**Fazit:** Als *alleinige* Erklaerung **unplausibel**. Sie ist nur dann
relevant, wenn der Watchdog **gar nicht scharf war** (H1/H2).

### H6 — Externer Watchdog-Hold / SoC-Zustand
Manche Boards haben einen **externen Watchdog-/Power-Hold**, der den SoC erst
loslaesst, wenn ein GPIO/PMIC-Signal kommt. Haengt dieses Signal, hilft der
interne Watchdog nicht.
**Gegenprobe:** Schaltplan/DTS nach `watchdog`-GPIOs bzw. PMIC-Reset-Leitungen
durchsuchen. **Ungeprueft** (kein Board-Zugriff hier).

### H7 — Der Hang war gar kein voller CPU-Hang
Wenn nur der **big-Cluster** (2532 MHz) stand und little weiterlief, wuerde
systemd weiter pingen → kein Reboot. Dann war „das Board" nicht ganz tot.
**Gegenprobe:** Im Befund steht „Board lief danach wieder normal" — ohne
Zwischenbeobachtung ist H7 nicht auszuschliessen. Wuerde aber eher *kurze*
Stoerung als 66 min bedeuten.

**Meine Gewichtung:** H1 und H2 sind die besten Erklaerungen fuer eine *lange*
Luecke (Watchdog war nie scharf bzw. wurde entwaffnet). H3 erklaert, warum ein
Reset lange brauchen kann. H4/H5/H7 erklaeren 66 min nicht allein.

---

## 4. Was der Parent am Board pruefen sollte (kleine, gezielte Befehle)

> **Nicht** in diesem Chat ausfuehren — der Kanal ist geteilt. Nur die Befehle
> nennen, damit der Parent sie bewusst und **sequenziell** fahren kann
> (keine rekursiven Suchen! Siehe `SHARED.md`, Kanal-Regeln).

```sh
# 1) Kam der letzte Reset vom Watchdog? 1 = ja, 0 = nein  (Kernnachweis)
cat /sys/class/watchdog/watchdog0/bootstatus
cat /sys/class/watchdog/watchdog0/state
cat /sys/class/watchdog/watchdog0/timeout

# 2) Konnte systemd das Device armen? (H2)
journalctl -b -1 -u systemd --no-pager | grep -i -e watchdog -e 'watchdog0'

# 3) Lief der Guard-Timer, und hat er das Device angefasst? (H1)
systemctl status watchdog-guard.timer --no-pager
systemctl cat watchdog-guard.timer

# 4) Wer haelt das Device offen? (H1/H2)
fuser -v /dev/watchdog0 2>&1 ; ls -l /proc/*/fd 2>/dev/null | grep watchdog

# 5) Effektives Timeout + Reboot-Timeout (H4)
grep -i -e RuntimeWatchdog -e RebootWatchdog /etc/systemd/system.conf
```

`bootstatus` allein entscheidet **H3 (feuerte, Reset kam nicht durch)** gegen
**H1/H2 (feuerte nie)** — das ist der wichtigste Einzelwert.

---

## 5. Haertungs-Vorschlag

### 5.1 Warum der interne Watchdog nicht reicht

Alle Mechanismen aus Abschnitt 2 haengen **am selben SoC**:
- systemd pingt **von der CPU**, die haengt;
- `panic=10` braucht einen **Panic**;
- `watchdog-guard.timer` ist selbst ein systemd-Objekt.

Ein *zweiter Software-Watchdog auf derselben CPU* scheitert **genau gleich**.
Ein wirklich unabhaengiger Pfad braucht **zwei** unabhaengige Dinge:

1. eine **unabhaengige Zeitbasis + Beobachter** (ausserhalb des SoC), und
2. einen **unabhaengigen Reset-Aktor** (Stromunterbrechung/Reset-Leitung),
   den der haengende SoC **nicht** selbst ausfuehren muss.

### 5.2 Vorgeschlagenes Design: externer Heartbeat + Monitor

**Board-Seite (Emitter):** ein systemd-Timer schreibt alle `INTERVAL` Sekunden
einen monotonen Heartbeat (Zeitstempel + Boot-ID) an einen Ort, den der
Beobachter lesen kann — z. B. `odroid/heartbeat` (retained) per MQTT, oder eine
Datei, die der Kanal ohnehin abholt.

**Beobachter-Seite (Monitor):** ein **off-board** Prozess (Sandbox-Kanal,
zweiter Rechner, oder ein billiger MCU) prueft die **Alter** des Heartbeats. Ist
er aelter als `INTERVAL + GRACE`, gilt das Board als haengend → **Aktor**
ausloesen.

**Aktor (der entscheidende Teil):** eine **Stromunterbrechung** (schaltbare
Steckdose / Power-Cut-Relais) oder die **Reset-Leitung** des Boards — **nicht**
`reboot -f` ueber denselben haengenden SoC (das kann nicht mehr ausfuehren).

Repo-Dateien:
- `watchdog-heartbeat.sh` — Emitter (reines `sh`, kein Python noetig).
- `watchdog-heartbeat.service` + `watchdog-heartbeat.timer` — Board-Units.
- `deploy_heartbeat.sh` — installiert nur die **Board-Seite** (Emitter + Timer).
- `watchdog_monitor.py` — Beobachter mit **reiner Entscheidungslogik**
  (`decide()`), host-getestet ohne Mocks.
- `test_watchdog_monitor.py` — die Tests dazu.

Der Monitor startet den Aktor nur ueber einen **konfigurierbaren Befehl**
(`--reboot-cmd`), damit keine Annahme ueber die Board-Umgebung eingebaut wird.

### 5.3 Vorteile

- **Unabhaengig** von der CPU, vom Kernel und von systemd — die drei Dinge, die
  im Befund versagt haben.
- **Erkennt genau die Luecke** aus Abschnitt 1: ausbleibender Heartbeat ueber
  `GRACE` Sekunden.
- **Kein Board-Zugriff noetig**, um die Logik zu testen: die Entscheidung ist
  eine reine Funktion.
- **Idempotent/robust**: Hysterese (mehrere Fehlschlaege) und **Cooldown**
  verhindern Reboot-Stuerme.
- Erzwingt die richtige Frage: *Was ist der Aktor?* Ohne echten Power-Cut ist
  die Haertung nur halb.

### 5.4 Restrisiken (ehrlich)

- **Der Beobachter ist ein neuer Single Point of Failure.** Faellt er aus,
  gibt es keine Sicherung — oder schlimmer, **Fehl-Reboots**.
- **Fehlalarme**: ein Netz-/Kanal-Aussetzer sieht wie ein Hang aus → unnoetiger
  Power-Cut. Gegenmittel: `GRACE` gross genug + mehrere Fehlschlaege + Cooldown.
  *Trade-off:* grosses `GRACE` = langsameres Eingreifen.
- **Zeit-/Uhrenproblem**: nutzt man den Board-Zeitstempel, kann eine falsche
  Board-Uhr „stale" vortaeuschen. **Besser**: der Beobachter stempelt den
  **Empfang**, nicht der Sender.
- **Teil-Hang**: haengt nur ein Cluster, laeuft der Heartbeat weiter → kein
  Reboot. Das ist gewollt (Board lebt), aber es faengt **keinen** partiellen
  Stillstand ab, der nur die Nutzlast trifft.
- **Aktor-Ausfall**: klemmt die schaltbare Steckdose, ist alles wirkungslos.
  Der Aktor sollte **fail-safe** sein (stromlos = aus) und selbst ueberwacht
  werden.
- **Boot-Schleife**: ein zu kurzes `GRACE` kann eine langsame, aber gesunde
  Boot-Phase als Hang deuten → Reboot-Schleife. Gegenmittel: `GRACE` > erwartete
  Boot-Dauer, und in der Boot-Phase den Monitor aussetzen.
- **Der interne Watchdog bleibt noetig.** Der externe Pfad ergaenzt, ersetzt
  nicht — H1/H2/H3 sollten zusaetzlich *direkt* gefixt werden (z. B.
  `nowayout=1` fuer `meson_wdt`, wenn der Treiber das erlaubt).

### 5.5 Sofortmassnahmen ohne neuen Aktor (falls kein Power-Cut verfuegbar)

- **`nowayout=1`** fuer `meson_wdt` erzwingen (Modul-Parameter/Kernel-Cmdline),
  damit ein `close()` den Watchdog **nicht** entwaffnet → adressiert H1 direkt.
  *Risiko:* Bei `nowayout=1` ist ein *sauberer* Reboot ohne Magic-Close nicht
  moeglich; systemd macht den Magic-Close, aber ein Absturz **waehrend** des
  Reboots fuehrt zu einem harten Reset (gewollt).
- **`hung_task_panic=1` + `hung_task_timeout_secs=60`**: der Kernel-Panic wegen
  haengender Tasks zieht dann `panic=10` heran. Hilft nur bei **Teil**-Haengen
  (der Detektor ist selbst ein Kernel-Thread).
- **Watchdog-`pretimeout`** nutzen: der Treiber kann vor dem Reset eine
  Panic/NMI ausloesen → sauberes Log vor dem Reset.

Diese drei sind **Hypothesen-adressierend** und brauchen Board-Verifikation
(Parent). Sie ersetzen den externen Pfad nicht.

---

## 6. Abnahme / Testbarkeit

- `watchdog_monitor.py` enthaelt die Entscheidung als **reine Funktionen**;
  `test_watchdog_monitor.py` prueft sie ohne Mocks, ohne Netz, ohne Board:
  ```sh
  sh -n remote-shell/ha/watchdog-heartbeat.sh
  python3 remote-shell/ha/test_watchdog_monitor.py
  ```
- Die **Board-Verifikation** (bootstatus, Timeout, Guard-Timer) macht der
  **Parent** — dieser Chat fasst das Board nicht an.

## 7. Offene Punkte (an den Parent)

1. `bootstatus` beim naechsten Boot lesen → H1/H2 vs. H3 entscheiden.
2. `watchdog-guard.timer`-Unit zeigen (`systemctl cat`) → Kandidat fuer H1/H2.
3. `fuser -v /dev/watchdog0` → wer haelt das Device.
4. Aktor festlegen (Power-Cut vorhanden? schaltbare Steckdose? GPIO am MCU?).
5. Erst danach `watchdog_monitor.py` produktiv anschliessen (Board-Zugriff).

---

## 8. Parent-Verifikation am Board (2026-10-18, Kanal `remote-shell/cmd_send.sh`)

Der Parent hat die in Abschnitt 4/7 genannten Befehle **sequenziell** am Board
gefahren. Ergebnis: **zwei Hypothesen sind widerlegt, und der in Abschnitt 4 als
„Kernnachweis" bezeichnete Wert ist auf dieser Hardware unbrauchbar.**

### 8.1 `bootstatus` ist KEIN Kernnachweis (Korrektur zu Abschnitt 4/7)

- `cat /sys/class/watchdog/watchdog0/bootstatus` → **`0`**.
- Der Grund steht im **eigenen** Board-Werkzeug `boot_journal.py`
  (`/usr/local/sbin/boot_journal.py`, Kopfkommentar):
  *„Die Amlogic-Hardware liefert KEINEN Reset-Grund (`watchdog/bootstatus` ist
  immer 0, pstore bleibt leer)."*
- **Folge:** `bootstatus` entscheidet hier **nichts** — weder H1/H2 noch H3.
  Der Satz in Abschnitt 4 („`bootstatus` allein entscheidet H3 gegen H1/H2 —
  der wichtigste Einzelwert") gilt fuer diese Hardware **nicht**. Die
  Entscheidung faellt ueber die **anderen** Spuren (8.2–8.5).

### 8.2 H2 (systemd konnte das Device nicht oeffnen) — WIDERLEGT

Am laufenden Board **und** im Journal des Hang-Boots (-4):

```
systemd[1]: Using hardware watchdog 'Meson GXBB Watchdog', version 0, device /dev/watchdog0
systemd[1]: Watchdog running with a hardware timeout of 1min.
fuser -v /dev/watchdog0   ->  /dev/watchdog0:  root  PID 1  F....  systemd
wdctl                      ->  Identity: Meson GXBB Watchdog [version 0]
                               Timeout: 60 seconds   Timeleft: 36 seconds
```

systemd hat das Device **erfolgreich geoeffnet, gehalten (PID 1) und auf 60 s
gestellt** — im Hang-Boot (-4) zeigen dieselben Zeilen um 02:14:55 dasselbe.
Ein „Device busy"/fehlgeschlagenes Open (H2) ist damit **ausgeschlossen**.

### 8.3 H1 ueber den Guard-Timer — WIDERLEGT

`systemctl cat watchdog-guard.{timer,service}` + `/usr/local/sbin/watchdog-guard.sh`:

- Der Guard liest **nur** `/sys/class/watchdog/watchdog0/state` (sysfs-Attribut)
  — er **oeffnet das Char-Device `/dev/watchdog0` nicht**.
- Damit ist er **kein** `close()`-Kandidat, der den Watchdog entwaffnen koennte
  (`nowayout=0`, aber ohne Opener/Closer gibt es nichts zu entwaffnen).
- Sein Zweck ist umgekehrt: bei `state != active` **3x** in Folge → `reboot`.

### 8.4 Kein sauberer Reboot-Pfad im Hang-Boot (H1-Variante „Magic-Close")

`journalctl -b -4` endet **mitten im fruehen Boot** (letzte Zeile 02:15:01,
`armbian-hardware-optimization`), **ohne** jede
`systemd-shutdown`/`Shutting down`/`reboot`-Sequenz. Die H1-Variante „systemd
schreibt beim geordneten Reboot `V` (Magic-Close), der Reboot haengt am PLL" ist
fuer **diesen** Hang **widerlegt** — es gab gar keinen Shutdown.

### 8.5 Boot-Journal (persistenter Reset-Grund)

`/var/lib/boot-journal/state.json` fuehrt `history` mit dem Grund je Boot
(`boot_journal.py`, `classify_reset`). Der Hang-Boot -4 (02:14:55–02:15:01) →
Recovery-Boot -3 (03:21:33) ist als **`hard-reset-unknown`** klassifiziert
(`prev_uptime_s ≈ 904 s`, kein Panic, kein pstore). Das persistierte Log endet
nach 6 s (Armbian-Ramlog verliert den RAM-Puffer beim Hard-Reset), waehrend der
Boot ~15 min lief. **Kein Panic** → `panic=10` konnte nicht greifen.

### 8.6 Rest-Bild

| Hypothese | Stand nach Parent-Verifikation |
|---|---|
| H1 (entwaffnet via `close()`) | **schwach**: Guard scheidet aus, kein Shutdown im Hang-Boot; nur `nowayout=0` als theoretische Luecke |
| H2 (Device busy, nie armiert) | **widerlegt** (systemd haelt `/dev/watchdog0`, 60 s) |
| H3 (feuerte, Reset kam nicht durch) | **beste verbleibende Erklaerung**: `hard-reset-unknown`, kein Panic, Board kam von selbst zurueck — passt zu einem PLL-Hang, der die Reset-/Boot-ROM-Domain mitnimmt |
| H4 (Timeout > 60 s) | traegt nur ~10 min bei, nicht die ~52–66 min |
| H5 (Kick-Task friert ein) | als Alleinursache unplausibel (Kick-Ausfall → Reset, nicht Ausbleiben) |
| H7 (nur big-Cluster stand) | **nicht auszuschliessen**: `prev_uptime_s` = 904 s ohne Zwischenbeobachtung |

**Konsequenz:** Das 60-s-HW-Watchdog-Sicherheitsnetz ist fuer **rohe PLL-OC-
Experimente nicht verlaesslich** (H3 bleibt plausibel). Die Haertung aus
Abschnitt 5 (externer Heartbeat + off-board Aktor) bleibt richtig; die in 5.5
genannten Sofortmassnahmen (`nowayout=1`, `hung_task_panic=1`) adressieren H1/H7
und sind weiterhin sinnvoll, aber **kein** Ersatz fuer den externen Pfad.

### 8.7 Zustand des Boards waehrend der Verifikation

- `user_overlays=lowfreq ramoops` in `/boot/armbianEnv.txt` → **kein OC-Overlay
  aktiv**, Board auf Stock-Takt.
- `/dev/video0` = **Amlogic Video Decoder** (`meson-video-decoder`), **keine
  Kamera** (bestaetigt den Net-Recon).
- Ethernet `end0` **NO-CARRIER** (Kabel am AP), WLAN-STA `192.168.178.71` aktiv —
  unveraendert (kein Eingriff).
