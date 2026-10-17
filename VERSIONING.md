# Versionierung

Kurz und praktisch. Es gibt **eine** Versionsnummer für die Firmware. Sie
steckt in einem Git-Tag; den Rest (welcher Commit, ob ungebaut) macht
`git describe` automatisch.

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

Aktueller Stand: **`v0.1.0`** ist der erste getaggte Baseline-Stand. Vorher gab
es keine Tags, deshalb war die Version `unknown`.

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
