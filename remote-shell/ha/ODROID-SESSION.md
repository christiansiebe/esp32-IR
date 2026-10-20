# Übergabe: Chat „HA WP entwicklung" (Conv 7a894c13…)

Erstellt 2026-10-20 durch den Fleet-Sub-Agenten `e6c5a6cb…`, weil die Sandbox
des Chats `7a894c13…` inzwischen **MISSING** (endgültig gelöscht, nicht
resumierbar) ist. Der Transcript ist über die API noch lesbar (13 720 Events,
359 Nachrichten); diese Zusammenfassung rekonstruiert ihn. Kopierbar als
Startnachricht für einen neuen Chat.

## Rolle des Chats

Parent der Odroid-Familie, nach der Übergabe 2026-10-18 vom Odroid-Board-/
OC-Thema auf **Home-Assistant-Dashboard („Wärmepumpe")** und danach auf
**Odroid-als-Access-Point + WLAN-Treiber** umgeschwenkt. Kein Parent mehr
(`parent_conversation_id = null`). Arbeit läuft auf Branch
**`feat/dfs-clock-visibility`** (Fleet + Registry), PRs gegen
`cline/power-history-diag`.

## Was der Chat gemacht hat (Phasen)

### A. XFCE-Desktop des Odroid über den Browser (2026-10-15, früh)
- Odroid N2 (Armbian bookworm) hat XFCE + TigerVNC (`xfce-vnc.service`, :1/5901).
- noVNC-Frontend + Tunnel gebaut; Doku `remote-shell/ha/novnc-README.md`,
  PR #19 gemerged. VNC-Passwort in `/root/.vnc/PASSWORT.txt` (0600).

### B. Kommando-Kanal Sandbox → Odroid (reverse channel)
- Sandbox hat keine LAN-Route (DS-Lite). Der Odroid **pollt** die öffentliche
  Sandbox-URL und führt Befehle aus: `remote-shell/` (`supervise.py`,
  `cmd_send.sh`, `channel_server.py`), Odroid-Seite systemd `esp32-channel`.
- Nötig, um das echte Board zu bedienen. Doku `remote-shell/OTHER-CHAT.md`.

### C. Home-Assistant-Dashboard „Wärmepumpe" (2026-10-15 … 16)
- Vorbild: bosch-buderus-wp.github.io HA-SimpleDashboard.
- Karten größenänderbar/zoombar gemacht, Kontrast (schwarze Schrift auf
  schwarzem Grund) gefixt, Fenster-„kleines Quadrat"-Klick-zum-Großmachen,
  Modulation/„elektrische Anschlussleistung 3 kW" ergänzt.
- Historie bis 1 Jahr nachgeladen (HA-Recorder), HTTPS via nginx +
  lokale CA (`setup_https_nginx.sh`, `setup_lancert_tls.sh`), DuckDNS
  `odroid-ha.duckdns.org`. Tool `check_wp_zoom_chart.py`.
- Wichtig (Nutzer): HA-Themen NICHT in den esp-idf-Ordner schreiben.

### D. Odroid Strom sparen: CPU-Takt/Undervolt/OC (2026-10-16 … 18)
Ausführlich dokumentiert auf Branch `docs/uv-software-limit-report`:
`UV-README.md`, `UV-SOFTWARE-LIMIT-REPORT.md`, `LOWFREQ-README.md`,
`PLL-OC-README.md`, `RAMOOPS-README.md`, `BOOT-JOURNAL-README.md`.
- **Lowfreq-Overlay** (`gen_lowfreq_dts.py`, `lowfreq.dts`): neue Takte bis
  **15,625 MHz**; nur 2^a·5^b-Teiler real, sonst rundet der Takt still ab
  (Prüfen über `lowfreq_clk_truth.sh`). Persistent via Overlay + `lowfreq_set_floor.sh`
  (das alleinige `cpu-freq-guard.sh` reicht NICHT — armbian schreibt MIN_SPEED
  danach).
- **Undervolt** (`uv_lowfreq.py`): niedrige Takte (≤1 GHz) sind vom
  **PWM-Regler** begrenzt (680 mV = Duty 0 = DT-Untergrenze), nicht vom Silizium;
  hohe Takte haben echtes Silizium-Limit (1,908 GHz hängt bei ~700 mV).
  Runtime-UV ist NICHT persistent; dauerhaft nur übers Overlay. Nur ein UV-Lauf
  gleichzeitig (flock).
- **OC** (`pll_oc_probe.py`, `margin_oc.py`): 2472 MHz stabil (Overlay),
  2496→2472 zurückgenommen; 2,45 GHz vom PLL-Teiler nicht erreichbar.
- **cpu-hotplug** (`cpu_hotplug.py`): idle-Kerne offline (cpu0 bleibt immer).
  **Ramoops/pstore** (`boot_journal.py`) für Absturz-Diagnose.

### E. Odroid als Access Point + WLAN-Treiber (2026-10-18, AKTUELL)
Doku: `remote-shell/ha/ODROID-NETZ-RECON.md`, `remote-shell/ha/WLAN-TREIBER-ANALYSE.md`.
- Stick: **TP-Link 802.11ac `2357:0138` = RTL8812BU/8822BU**, In-Kernel
  `rtw88_8822bu`. **Kein VIF/Concurrent** (nur RTL8822C kann AP+STA) → für
  AP+STA bräuchte es einen **MediaTek-Stick** (mt7612u/mt7921au, in-kernel).
- Ziel-Architektur: **`end0` (LAN) = Uplink**, **WLAN-Stick = AP** über
  `hostapd` + `dnsmasq` + NAT. (Kein VIF-Problem, da zwei verschiedene Geräte.)
- AP „Odroid" 5 GHz Kanal 36 **80 MHz 802.11ac (wifi5)** erreicht; persistent in
  `/etc/modprobe.d/88x2bu.conf` (`rtw_vht_enable=2`), Power-Management aus
  (`rtw_power_mgnt=0`, `rtw_smart_ps=0`), STA-VIF aus NetworkManager.
  Dienste `odroid-apsta`, `-dnsmasq`, `-nat` starten gemeinsam (Wants=), alle enabled.
- **Harter Bandbreiten-Deckel:** Stick ist **USB-2.0** (`bcdUSB 2.10`, 480 Mbit).
  Real ~250–300 Mbit netto max; beobachtet ~175 Mbit brutto / ~50 Mbit netto.
  Ein anderer Port hilft NICHT (USB2-Gerät bleibt bei 480 Mbit). Einziger echter
  Hebel: USB-3.0-WLAN-Stick.

## Offene Aufgaben (Stand letzte Nachrichten, 2026-10-18 ~21:45)

1. **Warum geht der „Konkurrenz-Modus" (AP+STA gleichzeitig) nicht?**
   Nutzer will das noch erforschen. Bisheriger Befund: rtw88_8822bu kann kein
   VIF → nur mit MediaTek-Stick lösbar. Nutzer-Hinweis: „der `iw scan` hat uns
   schon viel geärgert".
2. **Bandbreite/„Mogelpackung" klären:** Stick verbindet mit 700 Mbit zur
   FritzBox, kann aber nur 480 (USB2). Nutzer fragte nach mehr USB-3-Ports —
   geklärt: Stick ist das Limit, nicht der Port.
3. **Reboot-Festigkeit des AP-Setups abschließen + dokumentieren** (letzte
   Agent-Nachricht bot (a) freierer Kanal 149/155, (b) Reboot-Festigkeit
   abschließen+dokumentieren, (c) beides an).
4. Kontext: Der Odroid hängt hinter einer **FritzBox 7272** als AP im WLAN;
   LAN-Kabel steckte zuletzt am AP (nicht am Odroid); zukünftig soll eine
   **Kamera über LAN** am Odroid hängen und in HA eingebunden werden.

## Wichtige Regeln / Konventionen
- Arbeitsbranch **`feat/dfs-clock-visibility`**, PRs gegen `cline/power-history-diag`.
- **NIEMALS auf `main` pushen.** Host-Tests grün halten (`./test/run_host_tests.sh`).
- Board-Arbeit **nur** über den geteilten remote-shell-Kanal:
  `bash remote-shell/cmd_send.sh "<befehl>" <timeout>`.
- HA-/Odroid-/Infra-Themen gehören unter `remote-shell/` (nicht in den
  esp-idf-Firmware-Baum). CI-Pfadfilter `paths-ignore: remote-shell/**`.
- Token / WLAN-Passwörter NIE committen oder veröffentlichen.
- Fleet-Regeln: `AGENTS.md` „Fleet rule", `fleet/README.md`,
  `fleet/CHAT-INSTRUCTION.md`. `TASKS.json` (open/done) führen; fertig =
  `TASKS.json` done + Registry-Eintrag in `fleet/agents.json` auf `open_tasks: 0`.

## Board-Zugang
- Odroid N2: `odroidn2.local` / `192.168.178.71` (zuletzt), root per SSH.
- noVNC: `http://192.168.178.71:6080/vnc.html` (VNC-PW in `/root/.vnc/PASSWORT.txt`).
- ESP32-Empfänger: `192.168.178.70`.
- Kanal-Bootstrap in der Sandbox: `python3 remote-shell/supervise.py`.
