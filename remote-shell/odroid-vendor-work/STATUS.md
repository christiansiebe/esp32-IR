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
| 7 | **`cfg80211_ops` callbacks: kernel 7.x passes `struct wireless_dev *` (was `struct net_device *`), plus new `radio_idx`/`link_id` args** | retype the 12 callbacks + derive `ndev = wdev_to_ndev(wdev)` | **in progress (3 errors left)** |

Remaining errors after patch #7 (first pass):
* 2x `initialization of 'int (*)(struct wiphy *...` (ops entries)
* 1x `cfg_new_sta` incompatible pointer type
* 1x `cfg_del_sta` incompatible pointer type

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
