# C6-Stromzähler: MQTT- und Home-Assistant-Anbindung

Stand: 2026-11-13 · Branch `feat/c6-mqtt-cmd` · Firmware `v0.4.0-32-gb2e6541`
(Gerät `b2e6541`) · Gerät IP 192.168.178.105, Host `esp32-meter.local`.

Diese Datei beschreibt **nur** die MQTT-/HA-Anbindung des ESP32-C6-Stromzählers
(Empfänger-Rolle). Die Stromspar-/Sleep-Themen stehen in
`docs/c6-modem-sleep-safety.md`, die Messmethodik dort und in
`docs/radio-sleep-break-even.md`.

---

## 1) Zweck / Überblick

Der ESP32-C6 (C6FH4) liest den SML-Stromzähler über den IR-Lesekopf und ist
selbst der Sensor-Knoten. Zwei Wege bringen die Werte nach außen:

1. **HTTP-API** (`/api/dashboard`, `/api/power-history`, …) – Pull, für Web-UI
   und den (alten) Python-Bridge.
2. **MQTT** – Push an den lokalen Mosquitto, mit **Home-Assistant-Discovery**,
   sodass HA die Entitäten ohne manuelle YAML-Einträge selbst anlegt.

Die MQTT-Anbindung liegt im Empfänger-Image (per `#if`-Rolle; der Sender hat
kein MQTT):

- `main/mqtt_topics.{c,h}` – reine, host-getestete Topic-/Payload-Builder
  (`mqtt_topics_build`, `mqtt_topic_obis`, `mqtt_discovery_topic`,
  `mqtt_discovery_payload`).
- `main/mqtt_bridge.{c,h}` – esp-mqtt-Glue: NVS-Config, Connect-Handler,
  Discovery-/Status-Publishes, Live-Power/-Energy/-History.
- `main/mqtt_cmd.{c,h}` – host-getestetes Modul für den Rückkanal
  (Reassembly + JSON-Ergebnis).
- `main/mqtt_events.{c,h}` – Reboot-Event-Payload.
- `main/mqtt_publish.{c,h}` – reine Payload-Rechner (`mqtt_energy_to_kwh`,
  `mqtt_power_payload`, `mqtt_energy_payload`).

esp-mqtt und cJSON kommen in IDF v6.1 als **managed Components**
(`espressif/mqtt`, `espressif/cjson`, deklariert in `main/idf_component.yml`).
Ohne konfigurierte Broker-URI bleibt MQTT aus. Der Start erfolgt bei
`IP_EVENT_STA_GOT_IP` (siehe § 8, apoff-Boot-Fix).

**Broker:** Odroid 192.168.178.71:1883 (mosquitto, in HA als `core-mosquitto`),
User `meter`, Passwort `Strom-Meter-2026`. Der Broker-ACL erlaubt `meter`
Schreiben/Lesen auf `*`. Broker-URI, User, Passwort und Topic-Prefix liegen im
NVS-Namespace `mqttcfg` (nicht im Code) und sind zur Laufzeit über
`set mqtt-uri|mqtt-prefix|mqtt-user|mqtt-pass` änderbar
(`mqtt_bridge_set_config`, gefolgt von einem Bridge-Neustart).

---

## 2) Topic-Schema (State-Topics)

Basis ist der **Prefix** `esp32ir/receiver` (NVS-Wert `prefix`, in
`mqtt_topics_build` getrimmt; `mqtt-prefix` wird getrimmt persistiert).
Alle Topics entstehen in `mqtt_topics_build()` als `<base>/<suffix>`.

| Topic | Richtung | Retained | Payload | Zweck |
|---|---|---|---|---|
| `<base>/power` | ESP → Broker | ja | Bare-Number, z. B. `1234.5` | Live-Leistung in W (`state_topic` der Entität „Leistung") |
| `<base>/import` | ESP → Broker | ja | Bare-Number (kWh) | kumulativer Bezug, OBIS `1-0:1.8.0` |
| `<base>/export` | ESP → Broker | ja | Bare-Number (kWh) | kumulative Einspeisung, OBIS `1-0:2.8.0` |
| `<base>/average` | ESP → Broker | ja | Bare-Number (W) | 15-Minuten-Mittel der Leistung |
| `<base>/status` | ESP → Broker | ja | `online` / `offline` | Verfügbarkeit (LWT + expliziter `online`-Publish) |
| `<base>/state` | ESP → Broker | ja | JSON (Hub) | HA-Discovery-Hub, verknüpft das Gerät mit `<base>/status` |
| `<base>/event/reboot` | ESP → Broker | ja | JSON | Reboot-Grund (`reset reason`, FW-Version, Crash-Journal) |
| `<base>/history` | ESP → Broker | nein | JSON `{"t","w","n"}` | abgeschlossener Minutenwert (Roh-Fallback) |
| `<base>/cmd` | Broker → ESP | – | Befehlszeile | Steuerkanal (Subscribe, QoS 1) |
| `<base>/cmd/result` | ESP → Broker | ja | JSON `{"cmd","ok","out"}` | Antwort auf ein Kommando |

**Zahlenformat:** `power`/`import`/`export`/`average` gehen als **Bare-Number**
(nicht JSON) raus – die Discovery-Entitäten deklarieren das Topic ohne
`value_template`. Ein JSON-Objekt würde HA als „unknown" lesen (genau der
frühere Bug, s. § 8). `import`/`export` werden über `mqtt_energy_to_kwh()`
(Wh/1000) in kWh gerechnet; `power`/`average` in W.

`<base>/history` (JSON) ist der verbleibende Rohkanal für abgeschlossene
Minutenwerte; für die HA-Statistik sind heute die `total_increasing`-Zähler
`import`/`export` zuständig (HA rechnet Minuten-/Stunden-Historie selbst).

---

## 3) HA-Discovery

**Discovery-Prefix:** `homeassistant` (HA-Standard). Das Discovery-Topic je
Entität wird von `mqtt_discovery_topic()` gebaut:

```
<discovery_prefix>/<component>/<node_id>/<object_id>/config
```

Mit `component = sensor`, `node_id = esp32ir_recv`:

```
homeassistant/sensor/esp32ir_recv/<key>/config
```

Die Payload kommt aus `mqtt_discovery_payload(name, unique_id, state_topic,
unit, device_class, state_class, availability_topic, …)` und wird **retained**
veröffentlicht (beim Connect). Der Discovery-**Hub** `<base>/state` ist ein
retained JSON mit `availability_topic = <base>/status`, damit HA das Gerät
sieht, sobald es online ist.

### Entitäten

| `key` / `object_id` | Name | `unit` | `device_class` | `state_class` |
|---|---|---|---|---|
| `power` | Leistung | `W` | `power` | `measurement` |
| `import` | Bezug | `kWh` | `energy` | `total_increasing` |
| `export` | Einspeisung | `kWh` | `energy` | `total_increasing` |
| `average` | Leistung 15 min Mittel | `W` | `power` | `measurement` |

- **`unique_id`:** `esp32ir_recv_<key>` (z. B. `esp32ir_recv_power`).
- **`state_topic`:** jeweils `<base>/<key>`; `availability_topic` =
  `<base>/status`.

### device-Block

```json
{
  "identifiers": ["esp32ir_meter"],
  "name": "ESP32 IR Stromzaehler",
  "manufacturer": "esp32-IR",
  "model": "SML Meter"
}
```

### Resultierende HA-Entitäten

| HA-Entity-ID | Quelle |
|---|---|
| `sensor.esp32_ir_stromzaehler_leistung` | `power` |
| `sensor.esp32_ir_stromzaehler_bezug` | `import` |
| `sensor.esp32_ir_stromzaehler_einspeisung` | `export` |
| `sensor.esp32_ir_stromzaehler_leistung_15_min_mittel` | `average` |

Die drei kumulativen Zähler (`import`, `export`) schreiben dank
`total_increasing` die HA-Langzeitstatistik selbst mit; der frühere
`tools/ha_import.py`-Import bleibt nur für **Alt-Daten** nötig.

---

## 4) Steuerkanal `cmd` / `cmd/result`

Der Empfänger **abonniert** `<base>/cmd` (QoS 1) und schickt jede Zeile durch
**denselben Dispatcher** wie Web- und Seriell-Konsole (`web_console_run`). Die
Antwort geht **retained** nach `<base>/cmd/result`:

```json
{"cmd":"<Zeile>","ok":true,"out":"<Ausgabe>"}
```

Damit ist das Gerät auch ohne Web-UI bedienbar (`status`, `config`,
`set ps min`, `pull`, …).

Wichtige Eigenschaften (Details in `main/mqtt_bridge.c`, `main/mqtt_cmd.{c,h}`):

- **Eigener Worker-Task (`mqtt_cmd`, Stack 8192):** Der MQTT-Task führt **nie**
  selbst ein Kommando aus – ein langsamer Befehl (z. B. `pull`) würde den
  Keepalive blockieren und die Verbindung killen. Reassembly läuft im
  MQTT-Task, die Ausführung im Worker.
- **Queue-Tiefe 1:** Ist der Worker beschäftigt, wird ein neues Kommando
  **verworfen** (nie doppelt gestapelt) → ein Burst kann sich nicht aufstauen.
- **Reassembly:** esp-mqtt liefert große Payloads in mehreren
  `MQTT_EVENT_DATA`-Events; auf Folge-Chunks ist `topic` **NULL** (außer mit
  `CONFIG_MQTT_TOPIC_PRESENT_ALL_DATA_EVENTS`). Der Handler prüft das Topic nur,
  wenn es gesetzt ist, und setzt sonst eine laufende Reassembly fort.
  `mqtt_cmd_rx_feed()` erkennt Lücken/Überlänge und verwirft dann die Nachricht.
- **Längen:** Kommandozeile max. `MQTT_CMD_LINE_MAX` (192); Ergebnis-Payload
  max. `MQTT_CMD_PAYLOAD_MAX` (1024, unter dem esp-mqtt-TX-Buffer).
- **Sicherheit:** Kein Kommando ohne gültigen Broker-Login – der Kanal hängt an
  der authentifizierten MQTT-Session (`meter`/`Strom-Meter-2026`).
- **Subscribe zuletzt:** Erst nach Discovery-/Reboot-Publish, damit der
  optionale Kanal die HA-Publishes nicht verzögert.

---

## 5) Doppel-Publisher-Problem und Zielarchitektur

### Aktueller Zustand (Doppel-Publisher)

Es publizieren **zwei** Stellen:

1. **ESP (Discovery am falschen Prefix):** `mqtt_discovery_topic()` erzeugt
   `"<base>/homeassistant/<component>/<node_id>/<object_id>/config"` – mit
   `base = esp32ir/receiver` also
   `esp32ir/receiver/homeassistant/sensor/esp32ir_recv/.../config`.
   Das ist **nicht** der HA-Standard-Prefix `homeassistant`, daher **ignoriert
   HA diese Discovery komplett**.
2. **Odroid-Python-Bridge** `/root/esp32_mqtt_bridge.py`
   (Service `esp32-mqtt-bridge.service`): pollt die ESP-HTTP-API
   (`/api/dashboard`), bildet das 15-Min-Mittel und publiziert die Entitäten
   korrekt nach `homeassistant/sensor/esp32ir_recv/<key>/config` sowie die
   Werte nach `<base>/<key>` und `online`/`offline` nach `<base>/status`.

Momentan hängen die HA-Entitäten also faktisch an der **Python-Bridge**, nicht
am ESP. Zwei Publisher auf denselben Topics/Werten ist eine Fehlerquelle
(Flapping, doppelte Retains, uneinheitliche `unique_id`).

### Zielarchitektur (ESP als einzige Quelle)

Der ESP wird der **einzige** Publisher:

- Discovery nach `homeassistant/...` (echter HA-Prefix statt
  `<base>/homeassistant/...`).
- Werte weiter als Bare-Number auf `<base>/power|import|export|average`.
- **Explizites `online`** auf `<base>/status` beim Connect (nicht nur LWT).
- Der **Python-Bridge wird danach gestoppt** (`esp32-mqtt-bridge.service`
  disablen/stoppen) – er ist dann nur noch Alt-Daten-Werkzeug bzw. entfällt.

Damit gibt es genau einen Discovery-Autor und genau einen Wert-Autor je Topic.
Der Python-Bridge bleibt vorerst als Rückfallebene lauffähig, bis der ESP
verifiziert alle vier Entitäten selbst liefert.

---

## 6) Stromverbrauch und Messmethodik

Gemessene Werte (FNB-48 USB-Leistungsmesser, 5,19 V; Details in
`docs/c6-modem-sleep-safety.md` und `docs/radio-sleep-break-even.md` § 8.3):

| Konfiguration | Leistung |
|---|---|
| `ps min` + `radio-save off` + MQTT verbunden | **ca. 0,069 W** |
| `ps off` | **ca. 0,40 W** |
| Baseline APSTA (klassisch, Defaults) | ca. 0,433 W |

Der Hebel ist der **Modem-Sleep**: `ps min` zusammen mit `radio-save off` (bei
verbundenem WLAN) senkt den Verbrauch auf ~0,069 W, also rund **−83 %** gegenüber
`ps off` (0,40 W).

**Messartefakt (FNB48):** Der FNB48-USB-Leistungsmesser zeigt bei gepulstem
Verbrauch **stark schwankende** Werte – das ist ein **Messartefakt/Ripple**, nicht
der Mittelwert. Für belastbare Watt-Zahlen muss das Messgerät **in Reihe** in die
C6-VBUS-Leitung (in früheren Messungen lag es als Geschwister am Hub, nicht im
C6-Strompfad), und der interne Logger braucht das Stop-Kommando `0x84`, sonst
liefert er Standbilder. Die qualitativen Aussagen (Radio-Sleep ist der sichere
Hebel, Light Sleep killt die USB-Konsole) bleiben davon unberührt.

---

## 7) Der `ps_fallback`-Wächter – und warum 0,4 W „wiederkommen"

**Modul:** `main/ps_fallback.{c,h}` + `ps_fallback_worker` (in beiden Rollen),
Kconfig `ESPNOW_PS_FALLBACK` (**Default `y`**).

**Funktion:** Er zählt STA-Disconnects in einem gleitenden Fenster:

- Fenster **120000 ms**, Schwelle **3** Disconnects, Boot-Schonfrist **30000 ms**.
- Typischer Auslöser: `reason 34` = `MISSING_ACKS` (die FRITZ!Box/Mesh flappt
  auch ohne jeden Sleep-Hebel).
- Er zählt **nur** bei aktivem Modem-PS-Modus (`ps != off`) und nur auf dem
  **Empfänger** – der ESP-NOW-Wake-Window allein löst ihn nicht aus.

**Reaktion (sticky):** Bei einem „Storm" schaltet er **sticky** auf

```
cfg->wifi_ps_mode = 0        (WIFI_PS_NONE)
cfg->radio_save   = false
esp_now_set_wake_window(0); esp_wifi_set_ps(WIFI_PS_NONE);
```

und **persistiert das in NVS**. Sticky, damit es nicht flappt (PS aus → Link
heilt → PS an → wieder Disconnects). Ein **absichtlicher** Reconnect
(`set ps` / `set wifi-*` / `reset-config`) zählt nicht; `set ps` **re-armt** den
Wächter (der Nutzer darf es erneut versuchen).

**Warum 0,4 W „wiederkommen":** Weil der Wächter die Rückfall-Config
(`ps off` + `radio-save=false`) in NVS schreibt, überlebt sie den Reboot – nach
jedem Neustart steht wieder `ps off` (≈ 0,40 W) an, obwohl zuvor 0,069 W
gemessen wurden. Auch ein **USB-Ab-/Anstecken des Messgeräts** kann mehrere
Disconnects erzeugen und den Wächter auslösen. Um dauerhaft sparsam zu bleiben,
muss (a) der WLAN-Empfang/AP stabil sein und (b) nach dem Auslösen `set ps min`
(mit `radio-save`) erneut gesetzt werden.

---

## 8) Bekannte Bugs

1. **Discovery-Prefix falsch (offen):** `mqtt_discovery_topic()` baut
   `<base>/homeassistant/...` statt `homeassistant/...` → HA ignoriert die
   ESP-Discovery. Deshalb speist aktuell der Python-Bridge die HA-Entitäten
   (§ 5). Fix = echter HA-Prefix (Zielarchitektur).
2. **Fehlendes explizites `online` (offen):** Der ESP setzt bisher nur das LWT
   (`offline`); ein expliziter `online`-Publish auf `<base>/status` beim Connect
   fehlt (in der Zielarchitektur ergänzt). Ohne ihn bleibt die Verfügbarkeit
   nach einem Broker-/ESP-Neustart bis zum ersten LWT-Ereignis unklar.
3. **Serial-Console-Panic (offen):** `set mqtt-*` über die **USB-Serial-Konsole**
   führt zu

   ```
   Guru Meditation Error: Core 0 panic'ed (Stack protection fault)
   ```

   Ursache: Der Stack des Serial-Console-Tasks ist zu klein für die Locals von
   `mqtt_bridge_start()`/`mqtt_bridge_set_config()`. Workaround: MQTT-Config über
   Web-Konsole oder MQTT-`cmd` setzen, nicht über USB-Serial.
4. **History als JSON ohne `value_template` (behoben, historisch):** Die
   Live-Leistung ging früher als JSON auf `<base>/power` raus, ohne
   `value_template` → HA zeigte „unknown". Behoben durch Bare-Number-Publish
   (`mqtt_power_payload`).
5. **apoff-Boot-Panic (behoben):** Im station-only-Build
   (`receiver-c6-apoff`) gibt es kein `AP_START`-Event, daher laufen mDNS-Init,
   SNTP-Init und `mqtt_bridge_start()` im **einen** `GOT_IP`-Handler des
   Default-Event-Tasks (IDF: 2304 Byte) → `Stack protection fault`. Fix:
   `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096` in
   `sdkconfig.defaults.esp32c6`. Siehe `docs/c6-modem-sleep-safety.md`.

---

## 9) Verwandte Dokumente

- `docs/c6-modem-sleep-safety.md` – Modem-Light-Sleep, `ps_fallback`,
  apoff-Stack-Fix, gemessene 0,068 W.
- `docs/radio-sleep-break-even.md` – Radio-Sleep, Break-even und die
  FNB-48-Messmethodik (§ 8.3).
- `docs/CHAT-THREAD.md` – Verlauf/Herleitung der Messungen.
- `main/mqtt_bridge.c`, `main/mqtt_topics.c`, `main/mqtt_cmd.h`,
  `main/mqtt_events.c`, `main/mqtt_publish.c` – Implementierung.
- `/root/esp32_mqtt_bridge.py` – Python-Bridge (aktueller Publisher,
  Ziel: abgelöst).
- `TODO.md` – Abschnitte „MQTT / Home Assistant", „MQTT-Kommando-Kanal",
  „Modem-Sleep-Wächter (`ps_fallback`)".
