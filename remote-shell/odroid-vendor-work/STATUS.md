# Odroid N2 - AP `Odroid` + Realtek 8822BU vendor driver port

Status snapshot. Board work happens over the shared sandbox command channel
(one agent at a time; board work stays with the parent).

## AP + STA CONCURRENT WORKS (verified after reboot, 2026-10-18)

**The goal is met.** The vendor `88x2bu` driver runs the WLAN station (uplink)
and the `Odroid` access point **at the same time**, reboot-proof:

* STA VIF `wlxdc627949ba79` -> "Wohnzimmer 5 Ghz", `192.168.178.71`, ch 36.
* AP  VIF `wlxde627949ba79` -> SSID `Odroid`, `10.42.0.1/24`, ch 36, AP-ENABLED.
* dnsmasq serves DHCP `10.42.0.10-200` on the AP VIF only (port 53 off).

### Why concurrent mode needs the SAME channel (root cause, verified 2026-10-19)

The driver is built with `CONFIG_CONCURRENT_MODE` but **without
`CONFIG_MCC_MODE`**. `CONFIG_MCC_MODE` is the multi-channel-concurrency feature
(AP and STA on *different* channels, time-sliced); it is not enabled in the
Makefile, so `rtw_en_mcc` never appears under
`/sys/module/88x2bu/parameters/`. Without it the driver supports only
**single-channel concurrency (SCC)**: both VIFs must sit on one channel.

A/B proof (STA = FB7272 uplink "Wohnzimmer 5 Ghz" on 5180 MHz / channel 36):

| AP channel | result |
|-----------|--------|
| 36 (same as STA) | `AP-ENABLED`, both VIFs up at once |
| 44 (different)   | `nl80211: Failed to set channel (freq=5220): -16 (Device or resource busy)`, hostapd exits |

So "concurrent mode does not work" really means: it works **only** while the
Odroid AP and the upstream AP are on the same channel. The FB7272 currently
sits on channel 36, which is why AP+STA works on the bench.

Two consequences:

1. **Ordering matters.** hostapd must claim the channel *before* the STA owns
   the radio; otherwise the channel set returns `-16 (busy)`. Hence start the AP
   first, then bring the STA up (`odroid-apsta.sh` v2).
2. **Channel drift breaks it.** If the FB7272 changes channel (auto-channel),
   the AP (hard-coded channel 36) diverges and hostapd fails. The robust fix is
   to read the STA's channel and set the AP to the same value (v2 script does
   this); true independence needs an MCC build.

**True dual-channel concurrency** (AP and STA on different channels) would
require rebuilding the module with `CONFIG_MCC_MODE=y`
(`make ... CONFIG_MCC_MODE=y`, adds `-DCONFIG_MCC_MODE`), then `rtw_en_mcc=1`.
Not done - the current single-channel setup already meets the goal.

### What made it work (three changes, in order)

1. **Build with `CONFIG_CONCURRENT_MODE`.** The stock Makefile only sets it for
   Android/NV_TK1 platforms, so `CONFIG_IFACE_NUMBER` was 1 and a second VIF
   could not be created (`RTW: WARN adapter pool empty!`, `-ENODEV`). Fix: add
   `EXTRA_CFLAGS += -DCONFIG_CONCURRENT_MODE` to the **feature block** near the
   top of the Makefile (line ~55), *not* the platform blocks further down.
2. **Disable Wi-Fi P2P in NetworkManager** (`/etc/NetworkManager/conf.d/99-no-p2p.conf`:
   `[device]` / `wifi.p2p=0`). Without it NM asks the driver for a third
   (P2P) interface, the pool is empty again, and the STA gets torn down every
   few seconds (`device-managed ... args=false` in the NM journal).
3. **Keep the AP VIF out of NetworkManager** and let hostapd own it; the STA
   VIF stays with NM (`autoconnect=yes`, bound via `connection.interface-name`).
   AP channel must equal the STA channel (concurrent mode, MCC off).

### Reboot-proof layout

* Module installed at `/lib/modules/$(uname -r)/extra/88x2bu.ko`, loaded via
  `/etc/modules-load.d/88x2bu.conf`.
* `/etc/modprobe.d/88x2bu.conf`: `blacklist rtw88_8822bu` + power options
  (`rtw_power_mgnt=2 rtw_ips_mode=0 rtw_smart_ps=2 rtw_enusbss=0`).
* `/usr/local/sbin/odroid-apsta.sh` - picks STA/AP VIFs, matches the AP channel
  to the STA, writes hostapd + dnsmasq conf, runs hostapd in the foreground.
* `odroid-apsta.service` (enabled, Restart=always) + `odroid-apsta-dnsmasq.service`.
* **Safety:** the script never touches the STA VIF, so the LAN (`end0`) and the
  WLAN station (`192.168.178.71`) stay reachable regardless of AP state.

### Gotcha that cost an outage

`odroid-ap.service` (the old *standalone* AP service) was left **enabled** from
the in-kernel phase. It called `/usr/bin/iw` (the binary is at `/usr/sbin/iw`),
failed, and restart-looped (restart counter 261). Each restart ran
`nmcli dev set <sta> managed no`, tearing the STA down every 5 s and dropping
`192.168.178.71`. **Disable the old service** when switching to the vendor
driver: `systemctl disable --now odroid-ap.service`.

### Power saving (coherent, reboot-proof)

The STA is the uplink (may sleep), the AP must stay awake for its clients, so:

* STA VIF -> powersave **on**; AP VIF -> powersave **off**.
* Problem: rc.local runs *before* NetworkManager connects (no STA yet), and
  Armbian's global `zz-20-override-wifi-powersave-disable.conf`
  (`wifi.powersave=2`) turns powersave off again. Fixed with two hooks:
  `odroid-sta-powersave.service` (oneshot after NM, retries until the STA is up)
  and a NetworkManager dispatcher `90-odroid-sta-powersave` (on every STA `up`).
* `/usr/local/sbin/odroid-apsta.sh` forces powersave **off** on the AP VIF.
* Module options `/etc/modprobe.d/88x2bu.conf`: `rtw_power_mgnt=2`
  (max PS on the STA), `rtw_ips_mode=0` (IPS off so an idle AP never powers the
  radio down), `rtw_smart_ps=2`, `rtw_enusbss=0`.
* CPU governor `schedutil` + `rate_limit_us=10` via rc.local. `cpuidle` driver
  is `none` (kernel/DT limit, not changed). Idle temps ~35/37 C.

Measured: STA -> gateway 0 % loss, ~2.1 ms avg with powersave on.

## Earlier phase: standalone AP (in-kernel rtw88, no concurrency)

## What worked then (verified on the board, 2026-10-18)

* **Standalone AP `Odroid` is up and visible** on the phone.
  - in-kernel `rtw88_8822bu` driver, 5 GHz channel 36 (`hw_mode=a`).
  - Method: kill stray hostapd, `iw dev ap0 del`, NetworkManager unmanage the
    WLAN iface, `iw dev <sta> interface add ap0 type __ap`, give ap0 its **own
    MAC** (`dc:62:79:49:ba:7a`), `ip link set ap0 up`, then start hostapd.
  - The AP VIF must be created + up before hostapd starts; letting hostapd
    bring it up gives "Could not configure driver mode".
  - Leftover hostapd processes from earlier tests were the cause of the
    intermittent "Failed to set beacon parameters" failures.
  - `odroid-ap.service` (systemd, enabled, Restart=always) makes it reboot-proof.
    It refuses to start unless `end0` (Ethernet) has carrier, so a reboot
    without the LAN cable can never lock the board out.
  - `/etc/hostapd-odroid.conf`, `/usr/local/sbin/odroid-ap.sh`.

* **Not concurrent.** in-kernel rtw88 cannot run AP+STA at the same time: as
  soon as the STA connects, `ap0` goes NO-CARRIER and hostapd tears the AP
  down. So while the AP runs the WLAN station is down and the board's own
  uplink must be Ethernet (`end0`).

## The goal that is NOT met: AP + STA concurrent

Only the vendor driver has `CONFIG_CONCURRENT_MODE`. lwfinger's `rtw_*` driver
declares concurrent limits but its AP path is broken on this kernel
(`Register spurious class3 failed: ret=-22`). The vendor driver is the right
base but does not build on kernel 7.1.8 without a port.

### Vendor driver port (source: RinCat/RTL88x2BU-Linux-Driver, Feb 2025)

Built at `/usr/src/rtl88x2bu-git`. Progress against kernel `7.1.8-edge-meson64`:

| # | Breakage | Fix | State |
|---|----------|-----|-------|
| 1 | all `-I` paths land in `EXTRA_CFLAGS`, which kbuild >= 6.x ignores | one `ccflags-y += $(EXTRA_CFLAGS)` at the **END** of the KERNELRELEASE branch (after the `.mk` includes) | done |
| 2 | `-fmin-function-alignment=8` unknown to gcc 12 (kernel built with gcc 14) | `make CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT=` (empty override -> `-falign-functions`) | done |
| 3 | `from_timer`/`del_timer`/`del_timer_sync` renamed | compat shim at top of `include/osdep_service_linux.h`: `from_timer`->`timer_container_of`, `del_timer`->`timer_delete`, `del_timer_sync`->`timer_delete_sync` | done |
| 4 | `halrf/halrf_psd.h` not found | fixed by #1 | done |
| 5 | `struct pppoe_tag/hdr` member changes in `core/rtw_br_ext.c` | `CONFIG_BR_EXT = n` (NAT25/Internet-sharing only; irrelevant for AP+STA) | done |
| 6 | driver `hmac_sha256` conflicts with kernel `crypto/sha2.h` | rename to `rtw_hmac_sha256` (word-boundary sed) | done |
| 7 | **`cfg80211_ops` callbacks: kernel 7.x passes `struct wireless_dev *` (was `struct net_device *`), plus new `radio_idx`/`link_id` args** | retype the 12 callbacks + derive `ndev = wdev_to_ndev(wdev)`; `set_wiphy_params`/`set_tx_power`/`get_tx_power` gain `radio_idx` (+`link_id`); `cfg80211_new_sta`/`del_sta` now take `wdev` | **done** |

### BUILD + LOAD SUCCEED (2026-10-18)

**The module now builds AND loads on kernel 7.1.8.**

Recipe (the two things that matter beyond the source patches above):
1. **Build with the kernel's own compiler, GCC 14.2.0-19**, not the host GCC 12.
   The kernel is built with `aarch64-linux-gnu-gcc (Debian 14.2.0-19) 14.2.0` +
   binutils 2.44. A GCC-12 module trips ftrace validation at load time
   (`ftrace_call_adjust` warning + `kernel BUG at arch/arm64/kernel/patching.c`).
   Armbian bookworm has no gcc-14, but **Docker does**: build inside
   `debian:trixie-slim` (ships exactly GCC 14.2.0-19 + binutils 2.44).
2. **`strip --strip-debug` the .ko** before insmod (the unstripped module is
   77 MB of debug info; the stripped one is 6.5 MB and loads clean).

Build command (on the board):
```
docker run --rm \
  -v /usr/src/rtl88x2bu-git:/src \
  -v /usr/src/linux-headers-$(uname -r):/lib/modules/$(uname -r)/build \
  -w /src debian:trixie-slim sh -c '
    apt-get update -qq && apt-get install -y -qq gcc make binutils bc \
      libssl-dev libelf-dev flex bison kmod
    make clean; make -j4 ARCH=arm64 KSRC=/lib/modules/$(uname -r)/build
    strip --strip-debug 88x2bu.ko'
```
Note: with GCC 14 do **not** pass `CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT=`
(that override was only needed for GCC 12, which does not know
`-fmin-function-alignment`).

Result: `insmod` succeeds, `dmesg` shows
`RTW: rtl88x2bu v5.13.1-30-g37e60b26a ... module init ret=0`.
Working module backed up on the board:
`/root/netbackup/88x2bu-gcc14-stripped-20261018-2008.ko`.

Build log `/root/netbackup/vendor-build14.log` (GCC 12, builds but does not
load); the loading build is the trixie/gcc-14 one.

### Next: device takeover

The USB dongle is `2357:0138` (TP-Link 802.11ac NIC, an RTL88x2BU). Our module
lists that alias, but the **in-kernel `rtw88_8822bu` currently binds the
device** (`/sys/bus/usb/drivers/rtw88_8822bu/2-1.1:1.0`). To hand it to the
vendor driver: unbind from `rtw88_8822bu` (and unload it / blacklist it), then
let `88x2bu` probe. Then test AP + STA concurrent.

Patch scripts (in `remote-shell/odroid-vendor-work/`):
* `patch-vendor.sh` / `patch-vendor2.sh` - #1-#4 (Makefile + timer shim)
* `patch-vendor3.sh` - #6 (hmac_sha256 rename)
* `patch-cfg80211.py` - #7 (cfg80211 callback migration)

Build log on board: `/root/netbackup/vendor-build12.log` (latest).
Patched source backup: `/root/netbackup/rtl88x2bu-patched-20261018-1919.tar.gz`.

### Open question for the user

Is a gcc-14 update wanted? **Not required** - the flag issue is already worked
around, and gcc-14 is not in the Armbian bookworm repos (would need backports).
gcc-14 would only match the kernel's build compiler exactly. The remaining
errors are driver source porting, which gcc-14 does not solve.
