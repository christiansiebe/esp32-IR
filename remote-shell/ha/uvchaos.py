#!/usr/bin/env python3
"""Undervolt-Stress mit SCHWANKENDEM Takt (Odroid N2+).

Anders als uvstress.py (Frequenz je Schritt fix) laeuft hier eine Last durch,
waehrend die Frequenz beider Cluster in kurzen, zufaelligen Abstaenden springt.
Nach jedem Sprung wird der Undervolt ABSOLUT aus der Werks-Tabelle neu gesetzt
(nicht relativ zum aktuellen Duty - sonst Aufsummierung, siehe uvstress.py).

Geprueft wird wie dort per Hash: eine zu niedrige Spannung erzeugt oft stille
Bitfehler, bevor das System haengt. Bewusst NICHT persistent.
"""
import argparse
import hashlib
import json
import multiprocessing as mp
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv            # noqa: E402
import uvstress      # noqa: E402  (stock_duties, freqs, load, digest)


def _worker(seed, chunk, rounds, q):
    """Rechnet GENAU `rounds` Runden und meldet (seed, rounds, hash).

    Feste Rundenzahl fuer Referenz- und Testlauf -> die Hashes sind direkt
    vergleichbar, der Elternprozess muss nichts nachrechnen (O(1)).
    """
    h = hashlib.sha256()
    err = ''
    try:
        for _ in range(rounds):
            h.update(uvstress.digest(seed, chunk).encode())
    except Exception as e:                # noqa: BLE001
        err = repr(e)
    try:
        q.put((seed, rounds, h.hexdigest()[:16], err or '-'))
    except Exception:                     # noqa: BLE001
        pass


def run_batch(workers, chunk, rounds, switch_fn=None, switch_until_done=True):
    """`workers` Prozesse je `rounds` Runden; optional parallel Taktwechsel.

    Bei switch_until_done laeuft der Wechsel, bis alle Worker fertig sind.
    Liefert Liste (seed, runden, hash, error).
    """
    q = mp.Queue()
    ps = [mp.Process(target=_worker, args=(s, chunk, rounds, q)) for s in range(workers)]
    t0 = time.time()
    for p in ps:
        p.start()
    if switch_fn is not None:
        while switch_until_done and any(p.is_alive() for p in ps):
            switch_fn()
        while not switch_until_done and time.time() - t0 < rounds:
            switch_fn()
    out = []
    for _ in ps:
        try:
            out.append(q.get(timeout=max(60, rounds)))
        except Exception:                 # noqa: BLE001
            out.append((None, -1, 'TIMEOUT', '-'))
    for p in ps:
        p.join(timeout=30)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--offset-mv', type=int, default=100)
    ap.add_argument('--rounds', type=int, default=600, help='Runden je Worker (Referenz und Test)')
    ap.add_argument('--workers', type=int, default=6)
    ap.add_argument('--chunk', type=int, default=300000, help='Iterationen je Runde')
    ap.add_argument('--switch-min-ms', type=int, default=10)
    ap.add_argument('--switch-max-ms', type=int, default=60)
    args = ap.parse_args()

    offset_uv = args.offset_mv * 1000
    fbig, flit = uvstress.freqs('big'), uvstress.freqs('little')

    # 1) Referenz bei Werksspannung, feste Rundenzahl.
    for c in uv.CLUSTERS:
        uv.write_sys(uv.policy(c) + '/scaling_governor', 'schedutil')
    print('Referenz bei Werksspannung laeuft ...', flush=True)
    ref_res = run_batch(args.workers, args.chunk, args.rounds)
    ref = {s: h for s, r, h, e in ref_res}
    print('Referenz: %s' % ref, flush=True)

    # 2) Werks-Tabelle messen.
    stock = uvstress.stock_duties()
    print('Werks-Tabelle gemessen.', flush=True)

    # 3) Last starten, parallel Takt schwanken lassen.
    for c in uv.CLUSTERS:
        uv.write_sys(uv.policy(c) + '/scaling_governor', 'userspace')

    rnd = random.Random(1234)
    stats = {'switches': 0, 'kept': 0}

    def switch():
        fb = rnd.choice(fbig)
        fl = rnd.choice(flit)
        uv.write_sys(uv.policy('big') + '/scaling_setspeed', fb)
        uv.write_sys(uv.policy('little') + '/scaling_setspeed', fl)
        off_b = round(offset_uv * uv.CLUSTERS['big']['period'] / (uv.V_MAX - uv.V_MIN))
        off_l = round(offset_uv * uv.CLUSTERS['little']['period'] / (uv.V_MAX - uv.V_MIN))
        tb = max(0, stock['big'][fb] - off_b)
        tl = max(0, stock['little'][fl] - off_l)
        uv.write_duty('big', tb)
        uv.write_duty('little', tl)
        if uv.read_duty('big') == tb and uv.read_duty('little') == tl:
            stats['kept'] += 1
        stats['switches'] += 1
        time.sleep(rnd.randint(args.switch_min_ms, args.switch_max_ms) / 1000.0)

    t0 = time.time()
    res = run_batch(args.workers, args.chunk, args.rounds, switch)
    dur = time.time() - t0

    bad = []
    for s, r, h, e in res:
        if e != '-' or s is None or h != ref.get(s):
            bad.append((s, r, h, e))

    uv.revert()
    print('Schaltvorgaenge: %d, Undervolt gehalten: %d' % (stats['switches'], stats['kept']), flush=True)
    print('Dauer: %.1fs' % dur, flush=True)
    print('Rechenfehler: %d %s' % (len(bad), bad), flush=True)
    print('ZUSTAND: %s' % ('INSTABIL' if bad else 'STABIL'), flush=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
