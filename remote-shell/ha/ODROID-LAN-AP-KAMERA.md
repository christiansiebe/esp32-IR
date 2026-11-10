# Odroid N2 — LAN-Uplink + WLAN-AP + Kamera: Umsetzungsplan

Stand: 2026-10-18. Reine **Repo-Planung** — es wurde **kein** Board angefasst und
**kein** Kommando über den `remote-shell`-Kanal gesendet.

Grundlage (Messwerte, hier **nicht** wiederholt):
[`ODROID-NETZ-RECON.md`](ODROID-NETZ-RECON.md) auf dem Branch
`feat/dfs-clock-visibility`
(`git show origin/feat/dfs-clock-visibility:remote-shell/ha/ODROID-NETZ-RECON.md`).

Kurz-Ist (aus dem Recon):
`end0` = NO-CARRIER (Kabel steckt am AP), WLAN-STA `wlxdc627949ba79`
(USB `rtw88_8822bu`) ist UP auf `192.168.178.71`, Default-Route via
`192.168.178.1` (FRITZ!Box). `hostapd`/`dnsmasq` waren beim Recon noch nicht
installiert (inzwischen am Board installiert und **maskiert**, starten nicht von
selbst — siehe `WLAN-TREIBER-ANALYSE.md`).
`net.ipv4.ip_forward=1` ist **schon** gesetzt. `/dev/video0` ist **keine**
Kamera, nur der Amlogic-Decoder.

---

## 1. Zielarchitektur

```
        Heimnetz (FRITZ!Box 192.168.178.1)
                 │
      ┌──────────┴───────────┐
      │  LAN-Kabel (end0)    │   ← Uplink: Odroid ist kabelgebunden im Heimnetz
      │  Odroid N2           │
      │  wlxdc627949ba79     │   ← WLAN wird AP (SSID der alten AP-Clients)
      └──────────┬───────────┘
                 │  WLAN (hostapd) + DHCP (dnsmasq) + NAT auf end0
        alte AP-Clients (Handy, Steckdosen, …)
```

- **`end0` = Uplink ins Heimnetz.** Kabel von der FRITZ!Box (bzw. vom Switch)
  in den Odroid-Ethernet-Port. Danach ist der Odroid **unabhängig vom Funk**
  erreichbar — die Grundvoraussetzung für den Umbau.
- **`wlxdc627949ba79` = AP** für die Geräte, die vorher am alten AP hingen:
  `hostapd` (Funk) + `dnsmasq` (DHCP/DNS) + NAT/Forwarding auf `end0`.
- **Kamera über LAN** — siehe Abschnitt 6.

⚠️ **Warum die Reihenfolge zwingend ist:** Der Odroid hat genau **ein**
Funkmodul. Sobald es in den AP-Modus geht, fällt die WLAN-STA-Verbindung ins
Heimnetz weg. Der `remote-shell`-Kanal läuft aber genau über diese
STA-Verbindung. Ohne vorher verifizierten `end0`-Uplink ist der Odroid danach
**nicht mehr erreichbar** (Lockout). Deshalb: **erst end0, dann WLAN.**

> Der Recon zeigt, dass der `rtw88`-Treiber **keine** `valid interface
> combinations` meldet. Ein gleichzeitiges AP+STA auf demselben Modul ist damit
> **nicht garantiert**. Dieser Plan nimmt deshalb bewusst an: **ein Funkmodul =
> eine Rolle (AP)**. Wer AP+STA testen will, macht das erst *nach* dem
> end0-Uplink und mit Rückfallebene (Abschnitt 7).
>
> **Nachtrag (2026-10-18, am Board gemessen — siehe
> [`WLAN-TREIBER-ANALYSE.md`](WLAN-TREIBER-ANALYSE.md)):** AP+STA auf demselben
> Stick ist mit `rtw88_8822bu` **definitiv unmöglich** (`interface combinations
> are not supported`; ein zweites VIF lässt sich nicht hochfahren). Der
> Test-VIF-Versuch hat die STA-Verbindung **nicht** gekappt, das Board war also
> nie in Gefahr. Für die Zielarchitektur (**`end0`-Uplink + WLAN-AP**) reicht
> der vorhandene Stick: das sind **zwei verschiedene Geräte**, kein VIF-Problem.
> Ein AP **ohne** Kabel (WLAN-AP **und** WLAN-STA gleichzeitig) braucht dagegen
> einen **zweiten Funk-Adapter auf MediaTek-Basis** (mt7612u/mt7921au, mt76
> in-kernel vorhanden) — das ist eine Chip-, keine Treiberfrage.
> `hostapd`/`dnsmasq` sind inzwischen **installiert und maskiert** (starten
> nicht von selbst), bereit für die echte Konfiguration.

---

## 2. Phasenplan (mit harten Gates)

Jede Phase endet mit einem **Gate**, das bestanden sein muss, bevor die
nächste beginnt. Nichts wird „auf Vorrat“ umgestellt.

| Phase | Inhalt | Gate (muss vor der nächsten Phase gelten) |
|-------|--------|-------------------------------------------|
| **P0** | Rettungsnetz herstellen (Abschnitt 3) | physischer/serieller Zugang getestet **oder** Auto-Revert geplant |
| **P1** | `end0` als Uplink in Betrieb nehmen (Abschnitt 4) | Odroid über `end0`-IP erreichbar (ping/SSH/HA), Default-Route läuft über `end0` |
| **P2** | WLAN auf AP umstellen: `hostapd` + `dnsmasq` (Abschnitt 5.1); Ausrollen mit `deploy_ap.sh` (5.5), Dead-Man armiert (7.2) | AP-Client bekommt DHCP-Lease und erreicht `192.168.178.1` |
| **P3** | Kamera einbinden (Abschnitt 6) | HA zeigt ein Kamerabild |

**P0 und P1 dürfen nie übersprungen werden.** P2 ohne bestandenes P1-Gate =
Lockout (siehe Abschnitt 7).

---

## 3. P0 — Rettungsnetz (vor allem anderen)

Der Umbau kann die Fernverbindung kappen. Vorher **einen** Rückweg sicherstellen:

1. **Physischer Zugang** (Tastatur + Monitor am Odroid) oder **serielles
   Konsolengerät** am UART-Header, einmal testweise eingeloggt.
2. **Auto-Revert (Dead-Man-Switch):** jede Änderung wird mit einem
   zeitverzögerten Rückbau geplant. Bleibt die Bestätigung aus, fällt die
   Konfiguration automatisch zurück (Abschnitt 7.2).

Ein Board ohne P0-Rückweg umzubauen ist der einzige echte Einbahnweg in dieser
Aufgabe — er wird nicht gegangen.

---

## 4. P1 — `end0` als Uplink verifizieren

Kabel in den Odroid stecken (nicht in den AP). Dann:

```sh
# Link-Status: CARRIER da? (vorher: NO-CARRIER)
cat /sys/class/net/end0/carrier      # 1 = Kabel/Link ok
ip -br link show end0

# Falls NetworkManager end0 verwaltet: Profil „shared/unmanaged“ vermeiden,
# normale DHCP-Verbindung nutzen.
nmcli device status
nmcli con show --active
nmcli device connect end0            # holt per DHCP eine 192.168.178.x-Adresse
```

**Gate-Pfüfung** (erst danach weiter):

```sh
ip -4 addr show end0                 # end0 hat eine 192.168.178.x/24
ip route get 192.168.178.1           # geht über end0
ping -c3 192.168.178.1
```

**Wichtig bis hierher:** WLAN-STA **nicht** anfassen — sie ist noch die
Rettungsleine. Erst wenn `end0` steht (ping + Route), ist der Umbau erlaubt.
Zwei Default-Routen (WLAN und end0) sind übergangsweise okay; die Metrik
entscheidet. Nach dem Gate kann die WLAN-STA-Route entfallen, weil das WLAN
gleich AP wird.

---

## 5. P2 — WLAN wird AP (`hostapd` + `dnsmasq`)

### 5.1 Pakete und Vorbereitung

```sh
sudo apt-get update
sudo apt-get install -y hostapd dnsmasq      # bookworm main, per Recon vorhanden
```

- Die WLAN-Schnittstelle darf **nicht** gleichzeitig von NetworkManager
  verwaltet werden (sonst kämpfen NM und hostapd um das Interface):

  ```sh
  nmcli device set wlxdc627949ba79 managed no   # persistiert in NM
  # oder (falls NM nicht genutzt wird): nur sicherstellen, dass kein dhclient läuft
  ```

- `hostapd`/`dnsmasq` beim Boot über systemd starten (nicht über NM-Hotspot).

### 5.2 Konfigurationsvorlagen

> **Platzhalter, nicht raten.** `SSID`, `KANAL` und `PASSWORT` werden vom Nutzer
> festgelegt (siehe Abschnitt 8). Der Generator
> [`odroid_ap_config.py`](odroid_ap_config.py) erzeugt genau diese Dateien und
> verweigert Platzhalter-/Leerwerte.

**`/etc/hostapd/hostapd.conf`** (2,4 GHz, `hw_mode=g`):

```ini
interface=wlxdc627949ba79
driver=nl80211
ssid=<SSID>
hw_mode=g
channel=<KANAL>          # 2,4 GHz: 1..13 (DE); 1/6/11 überlappen nicht
country_code=DE
ieee80211d=1
ieee80211n=1
wmm_enabled=1
auth_algs=1
macaddr_acl=0
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=<PASSWORT>
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
```

**`/etc/hostapd/hostapd.conf`** (5 GHz, `hw_mode=a`) — nur der abweichende Teil:

```ini
hw_mode=a
channel=<KANAL>          # 5 GHz: 36/40/44/48 (nicht-DFS) oder 149..165
ieee80211ac=1
# DFS-Kanäle 52..64 / 100..144 zusätzlich:
# ieee80211h=1           # Radarerkennung, ~60 s CAC beim Start
```

Hinweis: Der alte AP funkte laut Recon auf **Kanal 36** (nicht-DFS). Ein
nicht-DFS-Kanal vermeidet die 60-Sekunden-Kanalverfügbarkeitsprüfung (CAC) beim
AP-Start und ist für einen einzelnen Funk-AP die robustere Wahl.

`/etc/default/hostapd` muss auf die Datei zeigen:

```sh
DAEMON_CONF="/etc/hostapd/hostapd.conf"
```

**`/etc/dnsmasq.d/odroid-ap.conf`**:

```ini
# DHCP/DNS nur auf dem AP-Interface
interface=wlxdc627949ba79
bind-interfaces
except-interface=lo

# Adressbereich des AP-Subnetzes (NICHT das Heimnetz 192.168.178.0/24)
dhcp-range=192.168.50.10,192.168.50.200,255.255.255.0,12h
dhcp-option=option:router,192.168.50.1
dhcp-option=option:dns-server,192.168.50.1
dhcp-authoritative

# Upstream-DNS = FRITZ!Box (aus Recon)
server=192.168.178.1
domain-needed
bogus-priv
log-dhcp
```

`/etc/default/dnsmasq`: `IGNORE_RESOLVCONF=yes` nur nötig, wenn systemd-resolved
dazwischenfunkt — sonst Standard lassen.

### 5.3 AP-Interface mit fester IP + NAT auf `end0`

Das AP-Interface bekommt die Gateway-Adresse des AP-Subnetzes:

```sh
sudo ip addr replace 192.168.50.1/24 dev wlxdc627949ba79
sudo ip link set wlxdc627949ba79 up
```

`net.ipv4.ip_forward=1` ist laut Recon **schon** gesetzt (sonst:
`sysctl -w net.ipv4.ip_forward=1` und in `/etc/sysctl.d/` persistieren).

NAT/Forwarding (iptables, bookworm nutzt das nft-Backend, Aufruf bleibt gleich):

```sh
UP=end0
AP=wlxdc627949ba79
sudo iptables -t nat -C POSTROUTING -o "$UP" -j MASQUERADE 2>/dev/null || \
  sudo iptables -t nat -A POSTROUTING -o "$UP" -j MASQUERADE
sudo iptables -C FORWARD -i "$AP" -o "$UP" -j ACCEPT 2>/dev/null || \
  sudo iptables -A FORWARD -i "$AP" -o "$UP" -j ACCEPT
sudo iptables -C FORWARD -i "$UP" -o "$AP" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
  sudo iptables -A FORWARD -i "$UP" -o "$AP" -m state --state RELATED,ESTABLISHED -j ACCEPT
```

Persistenz der Regeln (eine der beiden Varianten):

- `iptables-persistent` (`netfilter-persistent save`), **oder**
- `nft`-Regelsatz in `/etc/nftables.conf` (Äquivalent im Anhang).

> **Alternative ohne hostapd/dnsmasq:** NetworkManager kann den AP selbst
> (`nmcli con add type wifi ... mode ap` bzw. „shared“) und bringt DHCP+NAT
> mit. Weniger bewegliche Teile, aber weniger transparent. Der Auftrag nennt
> hostapd/dnsmasq — dieser Plan nimmt deshalb hostapd/dnsmasq als Hauptweg und
> NM-Shared nur als Rückfalloption.

### 5.4 Dienste starten und prüfen

```sh
sudo systemctl unmask hostapd
sudo systemctl enable --now hostapd dnsmasq
systemctl is-active hostapd dnsmasq
journalctl -u hostapd -n 20 --no-pager
iw dev wlxdc627949ba79 info        # zeigt jetzt type AP + channel
```

**Gate-P2:** Ein AP-Client verbindet sich, bekommt eine `192.168.50.x`-Lease
(`dnsmasq`-Log) und erreicht `ping 192.168.178.1` sowie `ping 8.8.8.8`.

### 5.5 Ausrollen mit `deploy_ap.sh` (empfohlener Weg)

Statt die Schritte aus 5.2–5.4 von Hand zu tippen, rendert der Generator die
Dateien und **`deploy_ap.sh`** installiert sie in einem Durchgang:

```sh
# 1) Dateien erzeugen (dry-run -> --out schreibt):
python3 remote-shell/ha/odroid_ap_config.py \
  --ssid "<SSID>" --passphrase "<PASSWORT>" --band 2g --channel 6 \
  --out /tmp/odroid-ap

# 2) Trockenlauf (Standard; aendert NICHTS, kein root noetig):
sh remote-shell/ha/deploy_ap.sh --conf-dir /tmp/odroid-ap

# 3) Ausrollen (root). Dead-Man-Switch standardmaessig 10 min:
sudo sh remote-shell/ha/deploy_ap.sh --conf-dir /tmp/odroid-ap --apply

# 4) Nach erfolgreicher Pruefung (Gate-P2) bestaetigen, sonst faellt es zurueck:
sudo sh remote-shell/ha/deploy_ap.sh --confirm
```

`deploy_ap.sh` macht genau: Dateien installieren (hostapd.conf, dnsmasq-Drop-in,
NAT-Skript), `/etc/default/hostapd` auf die Konfiguration zeigen lassen (mit
Backup), `systemctl unmask/enable --now hostapd dnsmasq`, `nmcli device set
<AP> managed no`, `ip addr replace <AP-Adresse>` + `ip link set up`, die
NAT-Regeln (idempotent, `-C`/`-A`), Zustand merken und den Dead-Man armeren.
**Kein ungefragtes Scharfschalten:** ohne `--apply` ist alles Dry-Run;
`hostapd`/`dnsmasq` bleiben bis zu einem echten Ausroll **masked**.

Optionen: `--ap-iface`, `--up-iface`, `--ap-addr CIDR`, `--deadman MIN`,
`--no-deadman`, `--root PREFIX` (Pfad-Praefix für Tests). Die Tests laufen
gegen einen `AP_ROOT`-Baum ohne Board und ohne root
(`python3 remote-shell/ha/test_ap_deploy.py`, 48 Checks).

### 5.6 Zurücknehmen mit `revert_ap.sh`

```sh
sh remote-shell/ha/revert_ap.sh            # Dry-Run
sudo sh remote-shell/ha/revert_ap.sh --apply
```

Idempotent: stoppt/deaktiviert/**maskiert** `hostapd`+`dnsmasq` wieder, entfernt
die NAT-Regeln, gibt das Funkmodul an NetworkManager zurück (`managed yes`),
nimmt die AP-Adresse weg, entfernt das dnsmasq-Drop-in, stellt
`/etc/default/hostapd` aus dem Backup her und entschärft den Dead-Man.

---

## 6. Kamera über LAN

### 6.1 Anschluss-Architektur (wichtig!)

Der Odroid N2 hat **einen** Ethernet-Port. Wenn `end0` der Uplink zur
FRITZ!Box ist, kann **kein** zweites LAN-Gerät direkt in den Odroid gesteckt
werden. Zwei saubere Wege:

1. **Kamera am selben LAN wie `end0`** (empfohlen): Kamera per Kabel in den
   Switch/die FRITZ!Box. Sie bekommt ihre eigene `192.168.178.x`-Adresse per
   DHCP. Der Odroid muss nur ins LAN routen — was er mit dem Uplink ohnehin tut.
   Kein zweiter NIC, keine Bridge nötig.
2. **Kamera direkt am Odroid**: braucht einen **zweiten** Ethernet-Port
   (USB-Gigabit-Adapter) und eine **Bridge** (`end0` + `usb-eth`), damit beide
   im Heimnetz liegen. Aufwendiger, nur nötig, wenn kein Switch verfügbar ist.

PoE-Kameras brauchen einen PoE-fähigen Switch oder einen PoE-Injektor.

### 6.2 Weg A — IP-Kamera (RTSP/ONVIF) über LAN

Einbindung in Home Assistant (läuft als Container auf dem Odroid):

- **ONVIF-Integration** (HA-Kern): findet die Kamera per WS-Discovery,
  liefert Videostream **plus** Motion-/Tamper-Events (wenn die Kamera sie
  anbietet). Einzutragen: IP, ONVIF-Port (meist 80/8000), Zugangsdaten.
- **Generic Camera** (HA-Kern): manuelle RTSP-URL, wenn ONVIF nicht passt.
  Typische Muster (herstellerspezifisch!):
  `rtsp://user:pass@<IP>:554/Streaming/Channels/101`,
  `rtsp://user:pass@<IP>:554/stream1`, `rtsp://user:pass@<IP>:554/h264Preview_01_main`.
- **Erweitert (optional):** `go2rtc`/Frigate für Aufzeichnung, Objekterkennung,
  WebRTC. Nicht nötig für „nur ansehen“.

Der HA-Container muss die Kamera-IP erreichen (normales Docker-Netz reicht für
ausgehende Verbindungen ins LAN; bei `network_mode: host` ist es trivial).
`ffmpeg` ist laut Recon vorhanden.

### 6.3 Weg B — USB-Kamera (UVC/V4L2) am Odroid

- Heute **nicht möglich**: `/dev/video0` ist der Amlogic-Decoder, **keine**
  Kamera (Recon). Es existiert kein V4L2-Kameragerät.
- Nach dem Anstecken einer **UVC-konformen** USB-Kamera erscheint ein
  `/dev/videoN` (N≥1). Prüfen: `v4l2-ctl --list-devices`, `ls /dev/video*`.
- HA hat **keine** eigene UVC-Integration. Übliche Wege:
  - **Generic Camera** mit ffmpeg-Input `-f v4l2 -i /dev/videoN` (Snapshot/Stream),
  - oder ein kleiner MJPEG-Brückendienst (z. B. `mjpg-streamer`) und diesen als
    Generic-Camera/MJPEG-Quelle einbinden.
- Container-Zugriff: das Gerät muss in den HA-Container durchgereicht werden
  (`--device /dev/videoN`, bzw. `devices:` in der Compose-Datei).

### 6.4 Entscheidungskriterien IP- vs. USB-Kamera

| Kriterium | IP-Kamera (LAN) | USB-Kamera (UVC) |
|-----------|-----------------|------------------|
| Kabelweg | Cat.5e/6 bis Switch, ggf. PoE | USB bis Odroid (kurz) |
| Position | frei im Haus (Netz genügt) | max. USB-Reichweite am Odroid |
| HA-Integration | ONVIF/Generic, sehr etabliert | nur Generic/ffmpeg, gebastelt |
| Bildqualität/Auflösung | hoch (4K, günstige Modelle) | begrenzt durch USB-Bandbreite |
| Nacht/IR, PTZ | üblich (IR-Cut, PTZ, Audio) | selten/variabel |
| Mehrere Kameras | einfach (jede im LAN) | begrenzt durch USB-Ports |
| Latenz/Aufzeichnung | RTSP, gut mit Frigate/go2rtc | V4L2, für Daueraufzeichnung schwächer |
| Preis | ab ~30 € | ab ~10 € |

**Empfehlung (vorläufig):** IP-Kamera (Weg A), weil sie in HA nativ
(ONVIF/Generic) läuft, unabhängig von Odroid-USB-Ports ist und die Aufstellung
frei bleibt. USB-UVC nur, wenn die Kamera zwingend am Odroid hängen soll und
Anspruch/Aufwand klein sind.

---

## 7. Rollback / Notfall

### 7.1 Wenn der Kanal nach dem AP-Umbau stirbt

1. **Physischer Zugang** (P0) nutzen: Tastatur/Monitor oder seriell.
2. AP-Dienste stoppen: `sudo systemctl stop hostapd dnsmasq`.
3. WLAN zurück auf STA/managed stellen:
   ```sh
   sudo ip addr flush dev wlxdc627949ba79
   sudo nmcli device set wlxdc627949ba79 managed yes
   sudo nmcli device connect wlxdc627949ba79     # STA ins Heimnetz
   ```
   Falls NM kein Profil mehr hat: neues `nmcli con add type wifi ifname
   wlxdc627949ba79 con-name wlan-home ssid "<Heim-SSID>"` + Passwort.
4. NAT-Regeln entfernen:
   ```sh
   sudo iptables -t nat -D POSTROUTING -o end0 -j MASQUERADE
   sudo iptables -D FORWARD -i wlxdc627949ba79 -o end0 -j ACCEPT
   sudo iptables -D FORWARD -i end0 -o wlxdc627949ba79 -m state --state RELATED,ESTABLISHED -j ACCEPT
   ```
5. AP-IP entfernen: `sudo ip addr del 192.168.50.1/24 dev wlxdc627949ba79`.
6. `hostapd`/`dnsmasq` deaktivieren: `sudo systemctl disable hostapd dnsmasq`.

### 7.2 Dead-Man-Switch (Auto-Revert)

**Implementiert** in `deploy_ap.sh` (Marker + `systemd`-Timer) und
`odroid_ap_deadman.sh` (Wrapper). `--apply` armiert den Timer standardmäßig auf
10 Minuten (`--deadman MIN`, `--no-deadman` schaltet ihn ab):

```sh
# Ausrollen MIT Dead-Man (Default 10 min):
sudo sh remote-shell/ha/deploy_ap.sh --conf-dir /tmp/odroid-ap --apply
#   -> deploy_ap.sh laesst /etc/odroid-ap.deadman liegen und startet
#      odroid-ap-deadman.timer (OnActiveSec=10min).

# Nach erfolgreicher Pruefung (Gate-P2) bestaetigen -> Timer aus, AP bleibt:
sudo sh remote-shell/ha/deploy_ap.sh --confirm
```

Bleibt die Bestätigung aus (z. B. weil der Kanal nach dem Umbau tot ist),
feuert der Timer und ruft `odroid_ap_deadman.sh`:

- Marker vorhanden → `revert_ap.sh --apply` (hostapd/dnsmasq gehen wieder
  **masked**, NAT weg, NM bekommt das Modul zurück).
- Marker weg (bestätigt) → nichts.

Damit ist die Absicherung aus Abschnitt 7.1 automatisiert: ein nicht
bestätigter AP baut sich **selbst** zurück. Host-getestet in
`test_ap_deploy.py` (Auto-Revert ohne Confirm, Confirm verhindert Revert).

> Die manuelle `systemd-run`-Vorlage ist damit nicht mehr nötig; wer sie
> dennoch nutzen will, kann sie zusätzlich verwenden.

### 7.3 Datei-Backups vor dem Umbau

```sh
sudo cp -a /etc/hostapd /etc/hostapd.bak.$(date +%F)
sudo cp -a /etc/dnsmasq.d /etc/dnsmasq.d.bak.$(date +%F)
sudo iptables-save > ~/iptables.before-ap.rules
nmcli con show > ~/nmcli.before-ap.txt
```

---

## 8. Offene Nutzer-Entscheidungen

1. **Welcher AP ist das genau (Modell), und soll der Odroid ihn ersetzen oder
   nur ergänzen?**
   - *Ersetzen* → gleiche **SSID + Passwort** wie der alte AP wählen, damit die
     Clients automatisch reconnecten. Gleicher/nahegelegener Kanal (laut Recon
     Kanal 36) hilft.
   - *Ergänzen* → eigene SSID/Kanal; Kanalabstand zum alten AP einplanen.
2. **IP-Kamera oder USB-Kamera?** (Abschnitt 6.4)
   - Bei IP: Hersteller/Modell, damit ONVIF/RTSP-URL und HA-Integration passen.
   - Bei USB: UVC-Kompatibilität prüfen; Aufstellung am Odroid.
3. **Soll der Odroid reiner AP werden oder Router mit eigenem Subnetz + NAT?**
   - *AP-only (bridge)* → ein einziges Heimnetz, Clients bekommen FRITZ!Box-IPs.
     Einfacher, aber der Odroid ist kein DHCP/NAT-Gateway.
   - *Router (eigenes Subnetz + NAT)* → Trennung/Autonomie, aber doppeltes NAT
     und der Odroid wird zum Single Point of Failure. Dieser Plan nimmt
     **Router/NAT** (`192.168.50.0/24`) als Hauptvariante, weil der Recon
     `ip_forward=1` und NAT-Regeln als Ziel nennt.
4. **SSID / Kanal / Passwort** konkret festlegen (nicht raten).
5. **hostapd/dnsmasq oder NetworkManager-Shared?** (Abschnitt 5.3)
6. **Kamera direkt am Odroid (zweiter NIC + Bridge) oder am selben Switch?**
   (Abschnitt 6.1)

---

## 9. Anhang

### 9.1 Generator benutzen

```sh
# Nur anzeigen (Standard = Dry-Run, schreibt nichts):
python3 remote-shell/ha/odroid_ap_config.py \
  --ssid "Wohnzimmer 5 Ghz" --passphrase "<PASSWORT>" --band 5g --channel 36

# In Dateien schreiben:
python3 remote-shell/ha/odroid_ap_config.py \
  --ssid "<SSID>" --passphrase "<PASSWORT>" --band 2g --channel 6 \
  --out /tmp/odroid-ap
```

Der Generator validiert SSID-/Passwortlänge, Band↔Kanal und dass das
AP-Subnetz **nicht** mit dem Heimnetz `192.168.178.0/24` überlappt, und
verweigert Platzhalterwerte. Tests:
`python3 remote-shell/ha/test_odroid_ap_config.py`.

Ausrollen/Rücknehmen: `remote-shell/ha/deploy_ap.sh` (Abschnitt 5.5),
`remote-shell/ha/revert_ap.sh` (5.6), Dead-Man `remote-shell/ha/odroid_ap_deadman.sh`
(7.2). Tests: `python3 remote-shell/ha/test_ap_deploy.py`.

### 9.2 nft-Äquivalent zu Abschnitt 5.3

```nft
table ip odroid_ap {
  chain postrouting {
    type nat hook postrouting priority srcnat; policy accept;
    oifname "end0" masquerade
  }
  chain forward {
    type filter hook forward priority filter; policy accept;
    iifname "wlxdc627949ba79" oifname "end0" accept
    iifname "end0" oifname "wlxdc627949ba79" ct state established,related accept
  }
}
```

### 9.3 Referenzen

- Recon: `remote-shell/ha/ODROID-NETZ-RECON.md` (Branch
  `feat/dfs-clock-visibility`).
- WLAN-Treiber/AP+STA: `remote-shell/ha/WLAN-TREIBER-ANALYSE.md` (Branch
  `feat/dfs-clock-visibility`) — belegt: `rtw88_8822bu` kann kein AP+STA; für
  AP ohne Kabel braucht es einen MediaTek-Adapter (mt7612u/mt7921au).
- Werkzeuge: `odroid_ap_config.py` (Generator), `deploy_ap.sh` (Ausrollen),
  `revert_ap.sh` (Rücknahme), `odroid_ap_deadman.sh` (Auto-Revert);
  Tests `test_odroid_ap_config.py`, `test_ap_deploy.py`.
- HA-Integrationen: ONVIF, Generic Camera (HA-Dokumentation).
- hostapd: `hw_mode`, `channel`, `wpa_passphrase`, `ieee80211h` (DFS).
- dnsmasq: `dhcp-range`, `dhcp-option`, `interface`, `bind-interfaces`.
