#!/usr/bin/env python3
"""Laufzeit-Undervolting fuer den Odroid N2(+) - bewusst NICHT persistent.

Die CPU-Spannung erzeugt ein PWM-Regler: VDDCPU_A (grosse Kerne, policy2) und
VDDCPU_B (kleine Kerne, policy0). Regler- und OPP-Sysfs sind read-only, deshalb
wird das PWM-Tastverhaeltnis direkt im Register gesetzt. Die Spannung folgt
linear (am Board vermessen):

    V[uV] = 680000 + duty * 360000 / period

    grosse Kerne: 0xffd1b000 + 0x00, period 999, regulator.2
    kleine Kerne: 0xff802000 + 0x04, period 333, regulator.8

Registerwert = ((period - duty) << 16) | duty.

Der cpufreq-Treiber besitzt den Regler und wuerde die Spannung beim naechsten
Frequenzwechsel ueberschreiben. Deshalb wird die Frequenz mit dem
userspace-Governor festgenagelt - echtes Undervolting ist nur bei fester
Frequenz moeglich.

Sicherheit: nichts wird gespeichert. Haengt das System, greift der
Hardware-Watchdog (RuntimeWatchdogUSec) und rebootet; danach ist alles wieder
auf Werkswert.
"""
import argparse
import json
import mmap
import os
import struct
import sys
import time

CLUSTERS = {
    'big':    dict(base=0xffd1b000, off=0x00, period=999, policy='policy2', reg='2'),
    'little': dict(base=0xff802000, off=0x04, period=333, policy='policy0', reg='8'),
}
V_MIN, V_MAX = 680000, 1040000
SAFE_MIN_UV = 700000          # nie unter 700 mV - Schutz vor sofortigem Hang
STATE = '/run/uv-state.json'
CPUFREQ = '/sys/devices/system/cpu/cpufreq'

_maps = {}


def _regs(c):
    if c not in _maps:
        f = open('/dev/mem', 'rb+', buffering=0)
        _maps[c] = mmap.mmap(f.fileno(), 0x1000, offset=CLUSTERS[c]['base'])
        _maps[c + '!f'] = f        # keep fd alive
    return _maps[c]


def read_reg(c):
    off = CLUSTERS[c]['off']
    return struct.unpack('<I', _regs(c)[off:off + 4])[0]


def write_reg(c, val):
    off = CLUSTERS[c]['off']
    _regs(c)[off:off + 4] = struct.pack('<I', val)


def read_duty(c):
    return read_reg(c) & 0xFFFF


def write_duty(c, duty):
    period = CLUSTERS[c]['period']
    duty = max(0, min(period, int(duty)))
    write_reg(c, ((period - duty) << 16) | duty)


def uv_to_duty(c, uv):
    period = CLUSTERS[c]['period']
    return round((uv - V_MIN) * period / (V_MAX - V_MIN))


def duty_to_uv(c, duty):
    period = CLUSTERS[c]['period']
    return V_MIN + duty * (V_MAX - V_MIN) / period


def reg_uv(c):
    return int(open('/sys/class/regulator/regulator.%s/microvolts' % CLUSTERS[c]['reg']).read())


def read_sys(path):
    return open(path).read().strip()


def write_sys(path, val):
    open(path, 'w').write(str(val))


def policy(c):
    return os.path.join(CPUFREQ, CLUSTERS[c]['policy'])


def pin(c, freq):
    """Frequenz festnageln (userspace-Governor) - Voraussetzung fuers Undervolten."""
    p = policy(c)
    if read_sys(p + '/scaling_governor') != 'userspace':
        write_sys(p + '/scaling_governor', 'userspace')
    if freq:
        write_sys(p + '/scaling_setspeed', freq)
    time.sleep(0.2)
    return int(read_sys(p + '/scaling_cur_freq'))


def status():
    out = []
    for c in CLUSTERS:
        p = policy(c)
        out.append(dict(cluster=c, freq=int(read_sys(p + '/scaling_cur_freq')),
                        governor=read_sys(p + '/scaling_governor'),
                        duty=read_duty(c), uv=reg_uv(c),
                        duty_uv=round(duty_to_uv(c, read_duty(c)))))
    return out


def apply(big_freq, little_freq, offset_uv, explicit):
    orig = {}
    for c in CLUSTERS:
        p = policy(c)
        orig[c] = dict(governor=read_sys(p + '/scaling_governor'),
                       max_freq=read_sys(p + '/scaling_max_freq'))
    # Frequenz zuerst festnageln, dann Spannung setzen.
    pin('big', big_freq)
    pin('little', little_freq)
    result = {}
    for c in CLUSTERS:
        stock = reg_uv(c)
        target = explicit[c] if explicit.get(c) else stock - offset_uv
        target = max(SAFE_MIN_UV, min(V_MAX, target))
        write_duty(c, uv_to_duty(c, target))
        time.sleep(0.2)
        result[c] = dict(stock_uv=stock, target_uv=target, actual_uv=reg_uv(c))
    with open(STATE, 'w') as f:
        json.dump(orig, f)
    return result


def revert():
    if not os.path.exists(STATE):
        return {'note': 'kein Zustand gespeichert - setze nur Governor zurueck'}
    orig = json.load(open(STATE))
    for c in CLUSTERS:
        p = policy(c)
        write_sys(p + '/scaling_governor', 'schedutil')
        write_sys(p + '/scaling_max_freq', orig[c]['max_freq'])
        write_sys(p + '/scaling_min_freq', '1000000')
    time.sleep(0.3)
    os.remove(STATE)
    return status()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('status')
    a = sub.add_parser('apply')
    a.add_argument('--big-freq', type=int, default=1800000)
    a.add_argument('--little-freq', type=int, default=1800000)
    a.add_argument('--offset-mv', type=int, default=40, help='Absenkung in mV')
    a.add_argument('--big-mv', type=int)
    a.add_argument('--little-mv', type=int)
    sub.add_parser('revert')
    args = ap.parse_args()

    if args.cmd == 'status':
        print(json.dumps(status(), indent=2))
    elif args.cmd == 'apply':
        explicit = {}
        if args.big_mv:
            explicit['big'] = args.big_mv * 1000
        if args.little_mv:
            explicit['little'] = args.little_mv * 1000
        print(json.dumps(apply(args.big_freq, args.little_freq,
                               args.offset_mv * 1000, explicit), indent=2))
    elif args.cmd == 'revert':
        print(json.dumps(revert(), indent=2))


if __name__ == '__main__':
    main()
