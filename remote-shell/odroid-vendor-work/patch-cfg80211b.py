#!/usr/bin/env python3
# patch-cfg80211b.py - finish the kernel 7.x cfg80211 migration (idempotent).
# Rebuilds ioctl_cfg80211.c from the pristine .orig and applies every step.
import re, shutil, os

P = 'os_dep/linux/ioctl_cfg80211.c'
if not os.path.exists(P + '.orig'):
    shutil.copy(P, P + '.orig')
s = open(P + '.orig').read()

ndev_funcs = [
    'cfg80211_rtw_add_key', 'cfg80211_rtw_get_key', 'cfg80211_rtw_del_key',
    'cfg80211_rtw_set_default_mgmt_key', 'cfg80211_rtw_get_station',
    'cfg80211_rtw_add_station', 'cfg80211_rtw_del_station',
    'cfg80211_rtw_change_station', 'cfg80211_rtw_dump_station',
]

def patch_fn(s, fn):
    pat = re.compile(r'((?:static\s+)?int\s+' + fn + r'\(.*?)\n\{', re.S)
    def repl(m):
        head = m.group(1)
        if 'struct net_device *ndev' not in head:
            return m.group(0)  # already done / different shape
        head = head.replace('struct net_device *ndev', 'struct wireless_dev *wdev', 1)
        return head + '\n{\n\tstruct net_device *ndev = wdev_to_ndev(wdev);'
    new, n = pat.subn(repl, s, count=1)
    print('  %-38s patched=%d' % (fn, n))
    return new

for fn in ndev_funcs:
    s = patch_fn(s, fn)

# set_wiphy_params: + int radio_idx
s2 = re.sub(r'cfg80211_rtw_set_wiphy_params\(struct wiphy \*wiphy, u32 changed\)',
            'cfg80211_rtw_set_wiphy_params(struct wiphy *wiphy, int radio_idx, u32 changed)', s)
print('  set_wiphy_params patched=%d' % (s != s2)); s = s2

# set_txpower: insert 'int radio_idx,' after the wdev line (whitespace tolerant)
s2 = re.sub(r'(cfg80211_rtw_set_txpower\(struct wiphy \*wiphy,\s*\n\s*#if[^\n]*\n\s*struct wireless_dev \*wdev,\n)',
            r'\1        int radio_idx,\n', s)
print('  set_txpower patched=%d' % (s != s2)); s = s2

# get_txpower: insert 'int radio_idx, unsigned int link_id,'
s2 = re.sub(r'(cfg80211_rtw_get_txpower\(struct wiphy \*wiphy,\s*\n\s*#if[^\n]*\n\s*struct wireless_dev \*wdev,\n)',
            r'\1        int radio_idx, unsigned int link_id,\n', s)
print('  get_txpower patched=%d' % (s != s2)); s = s2

# cfg80211_new_sta / cfg80211_del_sta now take a wireless_dev*
for old, new in [('cfg80211_new_sta(ndev,', 'cfg80211_new_sta(wdev,'),
                 ('cfg80211_del_sta(ndev,', 'cfg80211_del_sta(wdev,')]:
    c = s.count(old); s = s.replace(old, new)
    print('  %-38s patched=%d' % (new, c))

open(P, 'w').write(s)
print('written', P)
