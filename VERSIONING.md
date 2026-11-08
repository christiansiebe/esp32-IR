# Versionierung

Kurz und praktisch. Es gibt **eine** Versionsnummer für die Firmware. Sie
steckt in einem Git-Tag; den Rest (welcher Commit, ob ungebaut) macht
`git describe` automatisch.

## ⛔ REGEL (verbindlich): Versionierung ist Pflicht

Jede Firmware, die **geflasht oder veröffentlicht** wird, bekommt einen
**SemVer-Tag** `vX.Y.Z` auf den Commit, der gebaut wird, und der Tag wird
**gepusht**. Das ist keine Kür:

1. **Tag setzen** auf den Commit, der geflasht wird (nicht auf einen älteren).
2. **Tag pushen** (`git push origin vX.Y.Z`) — sonst kann `git describe` ihn auf
   der CI oder auf einem anderen Rechner nicht auflösen.
3. **Der Tag MUSS auf der Linie liegen, von der gebaut wird.** Vor dem Bauen
   prüfen: `git describe --tags` muss `vX.Y.Z…` liefern, **keinen nackten SHA**.
   Ein Tag auf einem Seitenzweig, der nicht Vorfahre des Build-Commits ist,
   hilft nicht (genau das war der C6-Fehler: `v0.3.x` lag auf
   `fix/memory-repair`/`fix/sender-diag-stack`, der C6-Build zeigte deshalb nur
   `5c57283`).
4. **Diese „Getaggte Stände"-Zeile mitziehen** — sie ist die einzige Stelle, an
   der die aktuelle Nummer in der Doku steht, und sie ist in der Vergangenheit
   dreimal stehengeblieben.
5. Die CI **warnt**, wenn `git describe --tags` keinen Tag auflöst (Schutz gegen
   die Regression).

## Zählweise

SemVer `vMAJOR.MINOR.PATCH`, mit `v` davor und als Git-Tag:

- `v0.y.z` — **Entwicklung**. Solange das Wire-Format oder das
  Speicher-Layout noch bricht, bleibt MAJOR bei `0`.
- `v1.0.0` — erste **stabile** Version (Format eingefroren, im Feld erprobt).

Wann welche Zahl steigt:

| Teil  | wann                                            | Beispiel                        |
|-------|-------------------------------------------------|---------------------------------|
| MAJOR | inkompatibel: Wire-Protokoll (`ESPNOW_DIAG_VERSION`, `SML_TRANSPORT_VERSION`) oder SPIFFS-Layout ändert sich | `v0.9.0` → `v1.0.0` |
| MINOR | neue Funktion, abwärtskompatibel                | `v0.1.0` → `v0.2.0`             |
| PATCH | Bugfix, keine neue Funktion                     | `v0.1.0` → `v0.1.1`             |

Getaggte Stände: **`v0.1.0`** (erster Baseline), **`v0.1.1`** (2026-10-21),
**`v0.2.0`/`v0.2.1`/`v0.2.2`/`v0.2.4`** (Empfänger-Historie robust gegen eine
kaputte Datenbank), **`v0.3.0`** (Sender merkt sich den ESP-NOW-Kanal im NVS),
**`v0.3.1`** (Sender bestätigt das OTA-Image erst, wenn alle Subsysteme bereit
sind) und **`v0.4.0`** (Empfänger-Rolle auf den ESP32-C6 portiert; Wire- und
Speicher-Layout unverändert, daher MINOR). Vor `v0.1.0` gab es keine Tags,
deshalb war die Version `unknown`.

## Wie die Nummer ins Gerät kommt

Der Build ruft zur Konfigurationszeit
`git describe --tags --always --dirty` auf (siehe `CMakeLists.txt`) und schreibt
das Ergebnis nach `main/build_version.h` → `fw_version()`.

| Zustand                         | Anzeige               |
|---------------------------------|-----------------------|
| genau auf dem Tag               | `v0.1.0`              |
| 3 Commits nach dem Tag          | `v0.1.0-3-gabc1234`   |
| ungespeicherte Änderungen dabei | `v0.1.0-3-gabc1234-dirty` |
| kein Tag vorhanden              | `abc1234`             |

Die Zeichenkette erscheint:

- auf dem Dashboard als `FW: …`,
- im Counters-JSON als `"version"`,
- **auf der Receiver-Senderseite als `sender fw:`** — die Version des anderen
  Geräts, übertragen im Counters-Frame (`espnow_diag_counters_t.fw_version`).

So sieht man ohne Ausprobieren, welcher Sender-Build gerade sendet.

## Commit-Konvention (Sender vs. Receiver)

Jeder Commit enthält **nur die Dateien der Seite, die sich geändert hat**:

- **Sender:** `main/uart_sleep_window.*`, Sender-Pfade in `main/main.c`,
  `components/sml_parser/**`, `components/sml_transport/**`
- **Receiver:** Receiver-Pfade in `main/main.c`, `main/power_history.*`,
  `main/history_*`, `main/mqtt_*`, `main/web_*`
- **Gemeinsam:** `components/sml_transport/include/espnow_diag.h` (Wire-Format),
  `main/version.*`, `CMakeLists.txt`, `VERSIONING.md`

Betrifft eine Änderung **beide** Seiten, steht das im Commit-Titel mit `both`:

```
fix(sender): adaptive light sleep actually engages
feat(receiver): show the sender firmware version on the sender page
feat(both): carry the sender firmware version in the counters frame
```

`main/main.c` trägt beide Rollen. Ändern sich nur die Pfade einer Rolle, wird
der Diff hunk-genau geteilt (`git add -p` bzw. `git apply` je Hunk), damit kein
Commit fremde Änderungen mitnimmt.

## Taggen

Auf den Commit, der geflasht werden soll:

```bash
git tag -a v0.2.0 -m "Sender: …; Receiver: …"
git push origin v0.2.0
```

Der Tag ist die Wahrheit. Die Zahl im Quelltext gibt es nicht extra — sonst
driften Code und Tag auseinander.

## Vor jedem Flash prüfen (Schutz gegen die Regression)

```bash
# 1) Liegt der Tag auf MEINER Build-Linie? (kein nackter SHA!)
git describe --tags --always
#   gut: v0.4.0-3-gabc1234      schlecht: abc1234  -> Tag fehlt/ist auf einem
#                                                    Seitenzweig, NICHT bauen

# 2) Ist der Tag von der CI aus erreichbar (gepusht)?
git ls-remote --tags origin | grep "v0.4.0"
```

`git describe --tags` löst nur Tags auf, die **Vorfahre** des aktuellen Commits
sind. Ein Tag auf einem anderen Zweig ist unsichtbar — genau so entstand der
C6-Fehler: die Empfänger-Rolle wurde auf `feat/esp32c6-receiver`/`feat/c6-twt`
portiert, aber der letzte erreichbare Tag lag auf `fix/sender-diag-stack`
(`v0.3.1`), sodass das Gerät nur `5c57283` meldete. Nach dem C6-Port ist der
Tag **`v0.4.0`** auf der C6-Linie zu setzen und zu pushen.
