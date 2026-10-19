# Odroid an eine Cline-Cloud-Sandbox anbinden — Reparaturplan

Kurzfassung: Der 404 kommt vom **falschen Agenten** (`/opt/esp32-channel/…` pollt
`/next`, das es im heutigen Protokoll nicht gibt) **plus** einer toten
All-Hands-URL. Cline-Cloud hat keinen oeffentlichen Port, deshalb baut die
Sandbox einen ausgehenden Tunnel auf; der Odroid nutzt **denselben** Agenten und
**denselben** Token wie bisher, nur mit der Tunnel-URL.

Alle Befehle sind kopierbar. Sie geben **keinen** Token aus (nur Laenge/Status).

## 0. Am Odroid: Bestand pruefen (nichts aendern)

```bash
systemctl is-active esp32-channel.service odroid-cmd-agent.service
ls -l /opt/esp32-channel/odroid_agent.sh /etc/systemd/system/odroid-cmd-agent.service
python3 -c 'import websockets; print("websockets", websockets.__version__)'
```

## 1. Agent-Code aktualisieren (Repo ist oeffentlich)

```bash
sudo install -d -m755 /opt/esp32-channel
sudo curl -fsSL -o /opt/esp32-channel/cmd_agent.py \
  https://raw.githubusercontent.com/christiansiebe/esp32-IR/main/remote-shell/cmd_agent.py
sudo curl -fsSL -o /opt/esp32-channel/cmd_policy.py \
  https://raw.githubusercontent.com/christiansiebe/esp32-IR/main/remote-shell/cmd_policy.py
sudo curl -fsSL -o /opt/esp32-channel/odroid_allowlist.txt \
  https://raw.githubusercontent.com/christiansiebe/esp32-IR/main/remote-shell/odroid_allowlist.txt
```

> Solange der Branch noch nicht gemerged ist, `main` durch den Branch-Namen
> ersetzen (`cline/0f7jc38h`).

Testverzeichnis (Ziel der Datei-Verben):

```bash
install -d -m700 "$HOME/odroid-channel-test"
printf 'odroid channel test\n' > "$HOME/odroid-channel-test/hello.txt"
```

## 2. Token: derselbe Wert auf beiden Seiten

Der Server vergleicht nur `?token=` per `hmac.compare_digest`; **jeder** String
funktioniert, auch der vorhandene 43-Zeichen-Token. Er liegt bereits in der Unit
(`Environment=SANDBOX_TERM_TOKEN=…`) und wird **nicht** ausgegeben.

**Empfohlen (der alte Token ist offengelegt): neu erzeugen.** In der
Cline-Sandbox liegt bereits ein frischer 43-Zeichen-Token unter
`/workspace/remote-shell/token` (0600). Wert dort anzeigen und **von Hand** am
Odroid einsetzen (der Assistent braucht ihn nicht):

```bash
# Cline-Sandbox (Ausgabe ist geheim, nur auf dem eigenen Bildschirm):
cat /workspace/remote-shell/token
```

```bash
# Odroid: Wert einfuegen, nicht in die History schreiben
read -rsp 'Sandbox-Token: ' T; echo
sudo sh -c 'umask 077; printf "SANDBOX_TERM_TOKEN=%s\n" "$1" > /etc/odroid-cmd-agent.env' _ "$T"
unset T
sudo stat -c '%a %s %n' /etc/odroid-cmd-agent.env      # 600, ~54 Byte
```

Alternativ den vorhandenen Token unveraendert weiterverwenden: dann dessen Wert
in der Sandbox nach `/workspace/remote-shell/token` schreiben
(`install -m600 /dev/stdin …`) — Protokoll und Laenge sind identisch.

## 3. In der Cline-Sandbox: Server + Tunnel starten

```bash
python3 /workspace/remote-shell/supervise.py            # WS-Server auf 12001
bash    /workspace/remote-shell/tunnel.sh start         # -> https://<name>.serveousercontent.com
bash    /workspace/remote-shell/tunnel.sh status
```

Die URL ist **nicht** geheim (der Token schuetzt den Zugang), sie wechselt aber
bei jedem Tunnel-Neustart.

## 4. Am Odroid: Ziel eintragen und Agenten (neu) starten

`odroid-cmd-agent.service` bleibt der Dienst — nur die URL und die Allowlist
kommen dazu. Kein Eingriff in `esp32-channel.service`.

```bash
URL='https://<name>.serveousercontent.com'      # aus Schritt 3
sudo sh -c "printf '%s\n' '$URL' > /etc/odroid-cmd-agent.urls"
sudo install -d -m755 /etc/systemd/system/odroid-cmd-agent.service.d
sudo tee /etc/systemd/system/odroid-cmd-agent.service.d/cline.conf >/dev/null <<'EOF'
[Service]
EnvironmentFile=/etc/odroid-cmd-agent.env
Environment=SANDBOX_TERM_URLS_FILE=/etc/odroid-cmd-agent.urls
Environment=CMD_AGENT_FILE_ROOT=/home/odroid/odroid-channel-test
ExecStart=
ExecStart=/usr/bin/python3 /opt/esp32-channel/cmd_agent.py --allowlist /opt/esp32-channel/odroid_allowlist.txt
EOF
sudo systemctl daemon-reload
sudo systemctl enable --now odroid-cmd-agent.service
systemctl show odroid-cmd-agent.service -p Environment | tr ' ' '\n' | grep -c SANDBOX_TERM_TOKEN
systemctl status odroid-cmd-agent.service --no-pager | head -15
```

`CMD_AGENT_AUTO=1` nur setzen, wenn wirklich unbeaufsichtigt gearbeitet werden
soll; ohne diese Variable fragt der Agent lokal nach (`y`/`a`). Mit
`--allowlist` laeuft **nur** `hostname`, `id`, `uname -a`, `uptime`, `ls`,
`cat`, `sha256sum` (im Testverzeichnis) sowie `@read`/`@write` — alles andere
wird mit Exit-Code -1 abgelehnt.

## 5. Nachweis (in der Cline-Sandbox)

```bash
export CMD_BASE=/tmp/cmdchannel
bash /workspace/remote-shell/cmd_send.sh 'hostname'
bash /workspace/remote-shell/cmd_send.sh 'id'
bash /workspace/remote-shell/cmd_send.sh 'cat /etc/shadow'          # MUSS abgelehnt werden
bash /workspace/remote-shell/cmd_send.sh '@write proof.txt aGFsbG8K'
bash /workspace/remote-shell/cmd_send.sh '@read proof.txt'
```

Erwartet: `hostname`/`id` liefern die Odroid-Werte, `cat /etc/shadow` endet mit
`ABGELEHNT (…Policy)`, `@write` meldet `5 Byte geschrieben`, `@read` liefert
`hallo`, und am Odroid existiert `~/odroid-channel-test/proof.txt`.

## 6. Erst danach den alten Dienst abschalten

```bash
sudo systemctl disable --now esp32-channel.service
systemctl is-active esp32-channel.service            # -> inactive
```

Home Assistant bleibt unberuehrt: es wird kein anderer Dienst angefasst, kein
Port am Router geoeffnet und kein Paket installiert (nur `python3-websockets`,
falls in Schritt 0 nicht vorhanden: `sudo apt install -y python3-websockets`).

## Wenn der Tunnel nicht erreichbar ist

- `tunnel.sh status` -> HTTP-Probe; sonst `tunnel.sh start` erneut.
- `journalctl -u odroid-cmd-agent -n 30 --no-pager` zeigt
  `verbunden: wss://…/agent` (ok) oder `Name or service not known` (DNS/Odroid).
- Der Relay (serveo.net) terminiert TLS: Befehle sind dort im Klartext sichtbar.
  Fuer dauerhaften Betrieb besser einen eigenen kleinen VPS/Cloudflare-Tunnel mit
  eigenem Hostnamen verwenden — das Protokoll bleibt identisch.
