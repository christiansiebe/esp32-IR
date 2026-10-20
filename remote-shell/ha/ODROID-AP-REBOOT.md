# Odroid N2 — Reboot-Festigkeit des Netz-Setups (T3)

Stand: 2026-10-20, erhoben über den Kommandokanal (`remote-shell/cmd_send.sh`).
Ergänzt [`ODROID-NETZ-RECON.md`](ODROID-NETZ-RECON.md) und
[`WLAN-TREIBER-ANALYSE.md`](WLAN-TREIBER-ANALYSE.md).

## Kurzantwort

**Der aktive Netz-Zustand ist reboot-fest.** Der Odroid hat zuletzt
**2026-10-20 00:16:18 CEST** neu gebootet und ist danach ohne Eingriff wieder
vollständig da (WLAN-STA verbunden, Kommandokanal aktiv, Docker/HA aktiv).

**Das AP-Setup ist dagegen bewusst AUS** (Nutzer-Entscheidung 2026-10-19:
STA-Modus dauerhaft; vgl. `remote-shell/memory/SHARED.md`). Seine Dateien und
Units sind **erhalten**, aber `disabled` — es gibt per Design keinen
„AP kommt nach dem Reboot hoch"-Test mehr. Die Reaktivierung ist ein Befehl.

## Belege am Board (Reboot 00:16:18 → geprüft 05:5x, uptime 5 h 51 min)

| Dienst / Zustand | enabled | nach Reboot aktiv | Bewertung |
|---|---|---|---|
| `esp32-channel.service` | **enabled** | active since 00:16:18 | ✅ Kanal überlebt Reboot |
| `odroid-cmd-agent.service` | **enabled** | active since 00:16:19 | ✅ WebSocket-Agent überlebt Reboot |
| `NetworkManager.service` | **enabled** | active since 00:16:18 | ✅ |
| WLAN-STA `wlxdc627949ba79` | (NM-Profil `autoconnect=yes`) | verbunden, DHCPv6-Leases erneuern | ✅ STA kommt nach Reboot zurück |
| `docker.service` | **enabled** | active since 00:16:42 | ✅ HA-Host überlebt Reboot |
| `odroid-apsta.service` | **disabled** | inactive (dead) | ⏸️ gewollt aus |
| `odroid-apsta-dnsmasq.service` | **disabled** | inactive (dead) | ⏸️ gewollt aus |
| `odroid-apsta-nat.service` | **disabled** | inactive (dead) | ⏸️ gewollt aus |
| `odroid-sta-powersave.service` | **disabled** | inactive (dead) | ⚠️ siehe „Kleine Lücke" |

## Warum der AP aus ist (und warum das T3 umdeutet)

Der Vendor-Treiber `88x2bu` ist mit `CONFIG_CONCURRENT_MODE`, aber **ohne
`CONFIG_MCC_MODE`** gebaut → nur **Single-Channel-Concurrency (SCC)**: AP- und
STA-VIF müssen auf **demselben Kanal** liegen. Zusätzlich gilt: hostapd muss den
Kanal **vor** der STA belegen, sonst `nl80211: Failed to set channel … -16
(busy)` (A/B-Beleg in `odroid-vendor-work/STATUS.md`, Branch
`feat/odroid-ap-vendor-driver`). Das ist empfindlich (Kanal-Drift der FRITZ!Box
bricht es). Der Nutzer hat deshalb am 2026-10-19 entschieden:
**STA bleibt dauerhaft, AP aus** — stabiler und ohne Kanal-Kopplung.

Damit heißt „Reboot-Festigkeit des AP-Setups" heute nicht mehr „AP muss nach
dem Reboot hochkommen", sondern:

1. **STA-Setup reboot-fest** → verifiziert (Tabelle oben).
2. **AP-Setup erhalten + ein-Befehl-reaktivierbar** → unten dokumentiert.
3. **Ein echter AP-Reboot-Test ist über den Kanal nicht fahrbar** (siehe Grenze).

## Reaktivierung des AP (falls der Nutzer es doch will)

Die Units/Skripte/Configs sind alle noch da (nur `disabled`):

```
/etc/systemd/system/odroid-apsta{,-dnsmasq,-nat}.service
/usr/local/sbin/odroid-apsta.sh
/etc/hostapd-odroid.conf
/etc/dnsmasq.d/odroid-ap.conf
/etc/NetworkManager/conf.d/99-odroid-ap-unmanaged.conf   (umbenannt, Backup)
```

Rückweg laut Backup (Quelle: `remote-shell/memory/SHARED.md`;
Backup `/root/netbackup/sta-persist-20261019-212419/RESTORE.txt`):

```sh
systemctl enable --now odroid-apsta odroid-apsta-nat odroid-apsta-dnsmasq
# unmanaged-conf zurueckbenennen, NM-Profil autoconnect=no
```

**Wichtig — SCC-Kopplung:** Wenn AP und STA gleichzeitig laufen sollen, muss der
AP-Kanal **gleich** dem STA-Kanal sein (aktuell Kanal 36). Der `odroid-apsta.sh`
liest den STA-Kanal und setzt den AP darauf (v2-Skript). Ein fest verdrahteter
AP-Kanal bricht, sobald die FRITZ!Box den Kanal wechselt.

## Kanal 149/155 — nur ohne STA sinnvoll

Die Übergabe notierte „ggf. freierer Kanal 149/155". Dazu:

- 149/155 sind im **EU/DE-SRD** erlaubt und **nicht DFS** (kein 60-s-CAC beim
  AP-Start) — für einen **eigenständigen** AP also eine gute Wahl.
- Für **AP+STA (SCC)** sind sie aber **schädlich**: der AP muss auf dem
  STA-Kanal liegen (36). Ein AP auf 149 würde bei laufender STA mit
  `-16 (busy)` scheitern.
- Fazit: 149/155 erst relevant, wenn der AP **allein** (ohne STA) betrieben
  wird. Solange die Nutzer-Entscheidung „STA-only" gilt, ist der Kanal **moot**.

## Grenze: was der Kommandokanal NICHT kann

Der Odroid-Agent läuft mit einer **engen Allowlist**
(`/opt/esp32-channel/odroid_allowlist.txt`). Erlaubt sind nur:
`hostname`, `id`, `uptime`, `uname -a`, `ls -la`-Muster,
`systemctl status <unit> --no-pager`, `journalctl -u <unit> -n N --no-pager`,
`df -h /`, `free -m`, lesende `git`-Operationen im Projekt.

**Nicht** erlaubt (und damit über den Kanal nicht fahrbar):
`systemctl start/stop/enable/disable`, `reboot`, `ip`, `iw`, `nmcli`, `cat`
außerhalb des Test-Roots. Datei-Verben (`@read`/`@write`) sind auf
`~/odroid-channel-test` eingesperrt.

→ Ein **Live-AP-Reboot-Test** (AP aktivieren, neu booten, prüfen ob der AP
wieder hochkommt) braucht **entweder** eine erweiterte Allowlist **oder** eine
Person/ein Skript direkt am Board. Über den heutigen Kanal ist er nicht möglich.

## Kleine Lücke: `odroid-sta-powersave`

`odroid-sta-powersave.service` (STA-VIF auf Powersave **an**) ist `disabled`
und lief nach dem letzten Reboot nicht. Folge: die STA fährt mit der
Armbian-Default-Powersave-Einstellung, nicht mit der bewusst gesetzten. Kein
Funktionsverlust (nur ein Stromspar-Nicety), aber wenn Powersave auf der STA
gewünscht ist, gehört der Dienst auf `enabled`. Empfehlung: prüfen und, falls
gewünscht, `systemctl enable odroid-sta-powersave` (Board-Zugang nötig).

## Offen / nächster Schritt (T3)

- [x] Aktiven (STA-)Netz-Zustand als reboot-fest belegen.
- [x] AP-Erhalt + Reaktivierungsbefehl dokumentieren.
- [x] Kanal-149/155-Frage einordnen (SCC-Kopplung).
- [ ] **Offen (braucht Board-Zugang außerhalb der Allowlist):** ein echter
      AP-Reboot-Test — AP aktivieren, rebooten, `AP-ENABLED` + AP-Client-Lease
      prüfen. Erst danach ist „AP-Setup reboot-fest" **gemessen** statt nur
      „Dateien erhalten". Bis dahin gilt: **STA-Setup reboot-fest (gemessen)**,
      **AP-Setup erhalten (nicht gemessen)**.
