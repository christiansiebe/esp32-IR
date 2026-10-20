# WLAN-Stick: Treiber-Analyse (AP+STA gleichzeitig?) — 2026-10-18

Frage des Nutzers: **„Gibt es einen besseren Treiber?"**
Kontext: Der Odroid soll den Access Point übernehmen. Der Stick ist ein
**TP-Link 802.11ac (USB-ID `2357:0138`) = Realtek RTL8812BU/8822BU**, am Board
vom **In-Kernel-Treiber `rtw88_8822bu`** bedient.

## Kurzantwort auf „Warum geht der Konkurrenz-Modus nicht?" (T1, 2026-10-20)

Ein Funkmodul kann nur dann gleichzeitig AP **und** STA fahren, wenn es mehrere
**virtuelle Interfaces (VIF)** auf einem PHY erlaubt. Ein VIF ist eine
Software-Sicht auf denselben Funk-Chip: `wlan0` (STA) und `ap0` (AP) teilen sich
Antenne, Kanal und Zeitfenster. Der Treiber muss das koennen **und** es der
Funk-Regulierung melden — ueber die Liste `valid interface combinations`.

Der `rtw88_8822bu`-Treiber meldet diese Kombinationen **nicht**:
`iw` sagt schlicht **`interface combinations are not supported`**. Ohne diese
Meldung darf der Kernel kein zweites Interface auf demselben PHY aktivieren —
er kennt keine erlaubte Kombination. Ein zweites VIF laesst sich zwar anlegen,
aber nicht hochfahren (siehe Messung unten). Das ist **kein Konfigurations-,
sondern ein Treiber-/Chip-Limit**: die Concurrent-Unterstuetzung im `rtw88`-
Kern ist fuer **RTL8822C** implementiert, **nicht** fuer den 8822B/8812BU.

**Konsequenz fuer dieses Board:** Ein gleichzeitiger AP+STA-Betrieb ist mit dem
vorhandenen Stick **unmoeglich** — unabhaengig von Treiberwahl, Kanal oder
Konfiguration. Es braucht einen **anderen Funk-Chip** (MediaTek, siehe unten),
keinen anderen Treiber.

## Messung am Board (Beleg)

```
$ iw phy | grep -A2 'software interface modes'
        software interface modes (can always be added):
                * monitor
        interface combinations are not supported      <-- entscheidend
```
Ein zweites Interface lässt sich zwar anlegen (`iw dev wlan0 interface add ap0
type __ap` → rc=0), aber `ip link set ap0 up` scheitert:
`RTNETLINK answers: Name not unique on network`. hostapd auf `ap0` bricht mit
`Could not set interface ap0 flags (UP)` ab. Die STA-Verbindung blieb dabei
**stabil** (Gateway 2 ms) — d. h. das Board war nie in Gefahr, aber **AP+STA
parallel ist mit diesem Treiber unmöglich**.

## Entscheidungsmatrix: braucht der Odroid ueberhaupt AP+STA?

| Ziel | Reicht der vorhandene Stick? | Warum |
|------|------------------------------|-------|
| AP mit **LAN-Kabel-Uplink** (`end0`) | **Ja** | AP (WLAN) und Uplink (LAN) sind zwei **verschiedene** Geraete — kein VIF noetig. |
| AP **ohne** Kabel (AP + STA gleichzeitig) | **Nein** | genau **ein** Funkmodul, und das kann kein VIF -> Lockout-Gefahr. Braucht einen **zweiten** MediaTek-Adapter. |
| Nur STA (kein AP) | **Ja** | Standardfall, laeuft heute. |

## Treiberlage (recherchiert)

| Treiber | AP | STA | **AP+STA gleichzeitig** | Kernel | Bewertung |
|---------|----|-----|--------------------------|--------|-----------|
| **`rtw88_8822bu`** (in-kernel, läuft) | ja | ja | **nein** — Concurrent nur für **8822C**, nicht 8822B | beliebig | stabil, aber kein VIF |
| **morrownr `88x2bu-20210702`** (out-of-kernel, Realtek-Basis) | ja | ja | **nein** — Realtek-out-of-kernel kann **keine VIFs** | 5.12–6.14 | baut auf Kernel **7.1.8 nicht** (nur bis 6.14 bzw. ≥6.18.4 mit gcc 15.2; Board hat gcc 12.2) |
| **`lwfinger/rtw88`** (In-Kernel-Backport-Fork) | ja | ja | **experimentell** — Collaborator hat Concurrent „für alle Chips" freigeschaltet, ausdrücklich *„just for testing"*, ohne Support | bis ~6.x | nicht produktionsreif |
| **MediaTek mt76** (mt7612u / mt7921au, **in-kernel**) | ja | ja | **ja** — echte `valid interface combinations`, AP+STA auf **demselben Kanal** | mt7612u 5.19+, mt7921au 5.18+ | **der „bessere Treiber" ist ein besserer Chip** |

Quellen: `lwfinger/rtw88` Issue #263 („rtw88 in the kernel only supports
concurrent mode with RTL8822C"), `morrownr/88x2bu` Issue #175 („Realtek
out-of-kernel drivers do not support virtual interfaces"), `morrownr/USB-WiFi`
(mt7610u: `#{ managed, AP, … } <= 2, total <= 2, #channels <= 1`).

**Die mt76-Module sind auf dem Odroid schon im Kernel vorhanden:**
`mt7921u.ko`, `mt76x2u.ko`, `mt76x0u.ko` (unter
`/lib/modules/7.1.8-edge-meson64/kernel/drivers/net/wireless/mediatek/mt76/`).

## Antwort: Kommt drauf an, was der Odroid werden soll

1. **Odroid als AP MIT LAN-Kabel-Uplink (`end0`) — der empfohlene Weg:**
   Der aktuelle Stick **reicht aus**. `end0` ist das Uplink-Interface, das WLAN
   ist der AP — das sind zwei *verschiedene* Geräte, also **kein VIF-Problem**.
   `rtw88` kann AP-Modus (in `iw list` gelistet). **Kein neuer Treiber, kein
   neuer Stick nötig.**

2. **Odroid als AP OHNE Kabel (WLAN-AP *und* WLAN-STA im Heimnetz gleichzeitig):**
   Der aktuelle Stick **kann das nicht** (kein VIF). Ein anderer *Treiber* hilft
   nicht — nötig ist ein **zweiter Funk-Adapter auf MediaTek-Basis**
   (mt7612u z. B. Alfa AWUS036ACHM / Panda PAU0B, oder mt7921au z. B. EDUP
   EP-AX1672 / Brostrend AX9L). Deren In-Kernel-Treiber unterstützt echtes
   AP+STA (gleicher Kanal). **Realtek-Sticks sind dafür die falsche Wahl** — das
   ist eine Chip-Eigenschaft, nicht nur eine Treiberfrage.

## Nebenbefund

Der Stick kann **beide Bänder** (2,4 GHz Kanal 1–13 **und** 5 GHz). Für das
ESP-NOW-Sniffen (Kanal 1, 2,4 GHz) ist das relevant — mit einem reinen AP-Betrieb
ginge der Sniffer, aber der Odroid ist dann nicht mehr gleichzeitig im Heimnetz.

## Werkzeuge, die für diese Tests auf dem Board liegen (reboot-fest? NEIN — nur Session)

- `/root/netbackup/recover-wlan.sh` — stellt die STA-Verbindung wieder her
  (Test-VIFs löschen, `nmcli con up`, sonst Treiber neu laden).
- `/etc/systemd/system/wlan-deadman.{service,timer}` — feuert `OnActiveSec`
  einmalig und ruft das Recovery-Skript. **Nicht enabled** (nur für die
  Test-Session gedacht). Für Dauerbetrieb müsste man ihn `enable`n.
- `hostapd` + `dnsmasq` sind installiert und **maskiert** (starten nicht von
  selbst) — bereit für die echte AP-Konfiguration.
