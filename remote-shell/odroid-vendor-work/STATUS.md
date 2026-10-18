# Odroid N2 - AP `Odroid` + Realtek 8822BU vendor driver port

Status snapshot. Board work happens over the shared sandbox command channel
(one agent at a time; board work stays with the parent).

## What works today (verified on the board, 2026-10-18)

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
