#!/usr/bin/env python3
"""Stresstest fuer Undervolting UNTER Frequenzwechsel (Odroid N2+).

Faehrt die komplette OPP-Tabelle beider Cluster ab, setzt bei jeder Frequenz
den Undervolt neu (cpufreq setzt beim Sprung die Werksspannung, danach wird das
PWM-Tastverhaeltnis wieder abgesenkt) und laesst Last laufen.

Wichtig: Es wird nicht nur geprueft "lebt noch", sondern die Rechenergebnisse
werden mit einem Referenzwert verglichen. Eine zu niedrige Spannung erzeugt oft
stille Bitfehler, bevor das System haengt.

Bewusst NICHT persistent; der Hardware-Watchdog ist das Netz bei einem Hang.
"""
import argparse
import hashlib
import json
import multiprocessing as mp
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv  # noqa: E402  (unser Undervolt-Werkzeug)

ITER = 4_000_000


def lcg(seed, iters):
    x = seed
    for _ in range(iters):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
    return x


def digest(seed, iters):
    return hashlib.sha256(str(lcg(seed, iters)).encode()).hexdigest()[:16]


def _worker(seed, iters, q):
    try:
        q.put((seed, digest(seed, iters)))
    except Exception as e:               # noqa: BLE001
        q.put((seed, 'ERR:' + repr(e)))


def load(iters, workers):
    """Jeden Worker mit eigenem Seed starten; Ergebnis per Seed zuordnen.

    Die Queue liefert in Fertigstellungs-Reihenfolge, deshalb muss ueber den
    Seed zugeordnet werden - nicht ueber die Position.
    """
    seeds = list(range(workers))
    q = mp.Queue()
    ps = [mp.Process(target=_worker, args=(s, iters, q)) for s in seeds]
    for p in ps:
        p.start()
    got = {}
    for _ in ps:
        s, h = q.get(timeout=120)
        got[s] = h
    for p in ps:
        p.join(timeout=10)
    return got


def freqs(c):
    return [int(f) for f in uv.read_sys(uv.policy(c) + '/scaling_available_frequencies').split()]


def stock_duties():
    """Werks-Tastverhaeltnis je Frequenz VOR jeder Absenkung messen.

    Noetig, weil der Regler-Framework-Cache nur bei geaenderter Zielspannung
    neu programmiert: bei zwei Frequenzen mit gleicher Werksspannung bleibt ein
    zuvor gesetzter Undervolt stehen. Wuerde man die Absenkung vom aktuellen
    Duty ableiten, summierte sie sich bis zur Sicherheitsgrenze auf.
    """
    tbl = {}
    for c in uv.CLUSTERS:
        p = uv.policy(c)
        gov = uv.read_sys(p + '/scaling_governor')
        uv.write_sys(p + '/scaling_governor', 'userspace')
        tbl[c] = {}
        for f in freqs(c):
            uv.write_sys(p + '/scaling_setspeed', f)
            time.sleep(0.05)
            tbl[c][f] = uv.read_duty(c)
        uv.write_sys(p + '/scaling_governor', gov)
    return tbl


def set_freq_uv(c, freq, offset_uv, stock):
    """Frequenz setzen (cpufreq stellt Werksspannung her), dann gezielt absenken.

    Zielwert kommt aus der Werks-Tabelle, nicht vom aktuellen Duty -> keine
    Aufsummierung.
    """
    p = uv.policy(c)
    uv.write_sys(p + '/scaling_setspeed', freq)
    time.sleep(0.05)
    period = uv.CLUSTERS[c]['period']
    off = round(offset_uv * period / (uv.V_MAX - uv.V_MIN))
    sb = stock[c][freq]
    target = max(0, sb - off)
    uv.write_duty(c, target)
    time.sleep(0.02)
    return sb, target


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--offset-mv', type=int, default=100)
    ap.add_argument('--cycles', type=int, default=2)
    ap.add_argument('--iters', type=int, default=ITER)
    ap.add_argument('--workers', type=int, default=6)
    ap.add_argument('--rapid-ms', type=int, default=0,
                    help='zusaetzlich so viele ms schnell zwischen Frequenzen springen')
    args = ap.parse_args()

    offset_uv = args.offset_mv * 1000
    for c in uv.CLUSTERS:
        uv.pin(c, None)                       # userspace, Frequenz unveraendert
    # Referenz bei Werksspannung, hoechste Frequenz
    for c in uv.CLUSTERS:
        uv.pin(c, max(freqs(c)))
    uv.revert()                               # Governor zurueck -> volle Spannung
    ref = load(args.iters, args.workers)
    print('Referenz-Hashes bei Werksspannung berechnet: %s' % ref)

    failures = []
    t0 = time.time()

    # Werks-Tastverhaeltnisse EINMAL messen - Grundlage jeder Absenkung.
    stock = stock_duties()

    def check(label, pairs):
        """pairs: Liste (cluster, freq). Undervolt anwenden, Last, Hash pruefen."""
        kept = {}
        for c, f in pairs:
            sb, tb = set_freq_uv(c, f, offset_uv, stock)
            kept[c] = tb
        res = load(args.iters, args.workers)
        bad = [s for s, h in res.items() if h != ref.get(s)]
        # Wurde der Undervolt uebernommen (nicht von cpufreq ueberschrieben)?
        ok_kept = all(uv.read_duty(c) == kept[c] for c, _ in pairs)
        mark = 'OK' if not bad and ok_kept else 'FEHLER'
        if bad or not ok_kept:
            failures.append(dict(where=label, mismatch=len(bad), kept=ok_kept))
        print('%-28s -> %s%s' % (label, mark,
              '' if not bad else ' rechenfehler=%d' % len(bad)), flush=True)

    for cyc in range(1, args.cycles + 1):
        for c in uv.CLUSTERS:
            uv.write_sys(uv.policy(c) + '/scaling_governor', 'userspace')
        # Referenzfrequenz fuer den jeweils anderen Cluster festhalten.
        base_big = max(freqs('big'))
        base_lit = max(freqs('little'))
        print('=== Zyklus %d: Undervolt je Frequenz (anderer Cluster auf max) ===' % cyc)
        for f in freqs('big'):
            check('c%d big=%d' % (cyc, f), [('big', f), ('little', base_lit)])
        for f in freqs('little'):
            check('c%d little=%d' % (cyc, f), [('big', base_big), ('little', f)])
        # Kritische Kombinationen: beide gleichzeitig.
        print('=== Zyklus %d: beide Cluster gleichzeitig ===' % cyc)
        for fb in (freqs('big')[0], freqs('big')[len(freqs('big')) // 2], base_big):
            for fl in (freqs('little')[0], base_lit):
                check('c%d big=%d+little=%d' % (cyc, fb, fl),
                      [('big', fb), ('little', fl)])

    if args.rapid_ms:
        print('--- Schnelles Umschalten %d ms ---' % args.rapid_ms, flush=True)
        for c in uv.CLUSTERS:
            uv.write_sys(uv.policy(c) + '/scaling_governor', 'userspace')
        end = time.time() + args.rapid_ms / 1000.0
        fb, fl = freqs('big'), freqs('little')
        i = 0
        while time.time() < end:
            set_freq_uv('big', fb[i % len(fb)], offset_uv, stock)
            set_freq_uv('little', fl[i % len(fl)], offset_uv, stock)
            i += 1
        res = load(args.iters, args.workers)
        bad = [s for s, h in res.items() if h != ref.get(s)]
        print('schnelle Wechsel: %d, rechenfehler=%d' % (i, len(bad)), flush=True)
        if bad:
            failures.append(dict(rapid=True, mismatch=len(bad)))

    uv.revert()
    print('--- Ergebnis ---')
    print('Dauer: %.1fs, Fehler: %d' % (time.time() - t0, len(failures)))
    print(json.dumps(failures, indent=2))
    print('ZUSTAND: %s' % ('INSTABIL' if failures else 'STABIL'))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
