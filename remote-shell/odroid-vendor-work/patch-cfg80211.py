#!/usr/bin/env python3
# patch-cfg80211.py - migrate the vendor driver's cfg80211_ops callbacks to kernel 7.x.
#
# Kernel 7.x changed struct cfg80211_ops: the callbacks that used to receive a
# struct net_device * now receive a struct wireless_dev *, and a few gained an
# 'int radio_idx' / 'unsigned int link_id' argument. The driver already ships
# wdev_to_ndev(), so each affected callback just needs its parameter retyped and
# a net_device local derived from it.
import re, sys, shutil, os

P = 'os_dep/linux/ioctl_cfg80211.c'
if not os.path.exists(P + '.orig'):
    shutil.copy(P, P + '.orig')
shutil.copy(P + '.orig', P)
s = open(P).read()

# callbacks whose first arg changed net_device* -> wireless_dev*
ndev_funcs = [
    'cfg80211_rtw_add_key', 'cfg80211_rtw_get_key', 'cfg80211_rtw_del_key',
    'cfg80211_rtw_set_default_mgmt_key', 'cfg80211_rtw_get_station',
    'cfg80211_rtw_add_station', 'cfg80211_rtw_del_station',
    'cfg80211_rtw_change_station', 'cfg80211_rtw_dump_station',
]

def make_repl(fn):
    pat = re.compile(r'((?:static\s+)?int\s+' + fn + r'\(.*?)\n\{', re.S)
    def repl(m):
        head = m.group(1)
        if 'struct net_device *ndev' not in head:
            print('  WARN: no ndev in', fn)
        head = head.replace('struct net_device *ndev', 'struct wireless_dev *wdev', 1)
        return head + '\n{\n\tstruct net_device *ndev = wdev_to_ndev(wdev);'
    global s
    s, n = pat.subn(repl, s, count=1)
    print('  %-38s patched=%d' % (fn, n))

for fn in ndev_funcs:
    make_repl(fn)

# set_wiphy_params: kernel adds 'int radio_idx'
a = 'static int cfg80211_rtw_set_wiphy_params(struct wiphy *wiphy, u32 changed)'
b = 'static int cfg80211_rtw_set_wiphy_params(struct wiphy *wiphy, int radio_idx, u32 changed)'
print('  set_wiphy_params patched=%d' % s.count(a)); s = s.replace(a, b)

# set_tx_power: kernel adds 'int radio_idx' after wdev
a = ("        struct wireless_dev *wdev,\n#endif\n"
     "#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)) || defined(COMPAT_KERNEL_RELEASE)\n"
     "        enum nl80211_tx_power_setting type, int mbm)")
b = ("        struct wireless_dev *wdev,\n        int radio_idx,\n#endif\n"
     "#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)) || defined(COMPAT_KERNEL_RELEASE)\n"
     "        enum nl80211_tx_power_setting type, int mbm)")
print('  set_tx_power patched=%d' % s.count(a)); s = s.replace(a, b)

# get_tx_power: kernel adds 'int radio_idx, unsigned int link_id'
a = "        struct wireless_dev *wdev,\n#endif\n        int *dbm)"
b = "        struct wireless_dev *wdev,\n        int radio_idx, unsigned int link_id,\n#endif\n        int *dbm)"
print('  get_tx_power patched=%d' % s.count(a)); s = s.replace(a, b)

open(P, 'w').write(s)
print('written', P)
