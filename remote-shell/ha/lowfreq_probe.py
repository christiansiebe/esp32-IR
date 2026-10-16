#!/usr/bin/env python3
"""Probe der unteren Taktraten des Odroid N2(+) (unter 1 GHz).

Setzt voraus, dass das Device-Tree-Overlay `lowfreq` aktiv ist, das die
OPP-Tabellen beider Cluster unter 1 GHz erweitert (siehe lowfreq.dts).

Getestet wird NICHT "laeuft noch": die Rechenergebnisse werden gegen eine
Referenz bei Werksspannung verglichen. Ein Taktwechsel ist digital, erzeugt
also keine stillen Bitfehler - aber ein falscher Mux-/Teilerwert kann den
AXI-/APB-Takt mitziehen und das System haengen. Deshalb laeuft jeder Punkt
unter Last und der Hardware-Watchdog ist das Netz bei einem Hang.

Erreichbare Raten sind parent/teiler, der Taktgeber rundet also auf wenige
kHz (fclk_div2/2 = 499999992 statt 500000000). Verglichen wird daher mit
Toleranz gegen die kHz-Werte aus scaling_available_frequencies.

Ergebnisse landen zusaetzlich in /root/lowfreq_probe_result.json, damit sie
einen Abbruch des Steuerkanals ueberleben.

Bewusst NICHT persistent: am Ende steht der Governor wieder auf schedutil und
scaling_min_freq wieder auf 1.0 GHz.
"""
import argparse
import hashlib
import json
import multiprocessing as mp
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv  # noqa: E402

ITER = 2_000_000
FLOOR = 1000000
TOL_KHZ = 3
RESULT = '/root/lowfreq_probe_result.json'


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
    except Exception as e:  # noqa: BLE001
        q.put((seed, 'ERR:' + repr(e)))


def load(iters, workers):
    q = mp.Queue()
    ps = [mp.Process(target=_worker, args=(s, iters, q)) for s in range(workers)]
    for p in ps:
        p.start()
    got = {}
    for _ in ps:
        s, h = q.get(timeout=180)
        got[s] = h
    for p in ps:
        p.join(timeout=10)
    return got


def freqs(c):
    return [int(f) for f in uv.read_sys(uv.policy(c) + '/scaling_available_frequencies').split()]


def real_khz(c):
    """Tatsaechlicher CPU-Takt aus debugfs (cpu_clk_dyn), in kHz.

    scaling_cur_freq meldet die WUNSCHrate. Der Teiler kennt aber nur 2^a*5^b;
    eine Rate mit Faktor 3 wird still abgerundet und cpufreq meldet trotzdem die
    Wunschrate. Nur cpu_clk_dyn zeigt, was der Kern wirklich faehrt.
    """
    name = 'cpu_clk_dyn' if c == 'little' else 'cpub_clk_dyn'
    try:
        return int(uv.read_sys('/sys/kernel/debug/clk/%s/clk_rate' % name)) // 1000
    except OSError:
        return None


def bench(cluster):
    """Wall-clock einer festen Arbeit -> belegt die tatsaechliche Taktung.

    Die Hash-Pruefung belegt nur Korrektheit; sie kann nicht zeigen, dass der
    Takt wirklich niedriger ist (der Prozessstart dominiert bei 50 MHz). Ein
    fester Rechenumfang bei 1.0 GHz als Referenz und der Quotient der Zeiten
    ist dagegen ein direkter Taktbeleg. Der Prozess wird auf die CPUs des
    Clusters festgenagelt, damit er auch dort laeuft.
    """
    cpus = CLUSTER_CPUS[cluster]
    os.sched_setaffinity(0, cpus)
    n = 400000
    best = None
    for _ in range(3):
        t0 = time.time()
        digest(7, n)
        dt = time.time() - t0
        best = dt if best is None else min(best, dt)
    return best


CLUSTER_CPUS = {'little': {0, 1}, 'big': {2, 3, 4, 5}}


def restore():
    for c in uv.CLUSTERS:
        p = uv.policy(c)
        uv.write_sys(p + '/scaling_min_freq', FLOOR)
        uv.write_sys(p + '/scaling_max_freq', uv.read_sys(p + '/cpuinfo_max_freq'))
        uv.write_sys(p + '/scaling_governor', 'schedutil')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iters', type=int, default=ITER)
    ap.add_argument('--workers', type=int, default=6)
    ap.add_argument('--dwell', type=float, default=0.8, help='Sekunden Last je Frequenz')
    ap.add_argument('--cluster', choices=['big', 'little', 'both'], default='both')
    args = ap.parse_args()

    low = {c: sorted(f for f in freqs(c) if f < FLOOR) for c in uv.CLUSTERS}
    if not any(low.values()):
        print('FEHLER: keine OPPs unter 1 GHz - Overlay nicht aktiv?')
        print('vorhanden:', {c: freqs(c) for c in uv.CLUSTERS})
        return 2
    for c in uv.CLUSTERS:
        print('%-7s < 1GHz (kHz): %s' % (c, low[c]), flush=True)

    clusters = ['big', 'little'] if args.cluster == 'both' else [args.cluster]

    # Referenz bei Werksspannung und maximaler Frequenz.
    for c in uv.CLUSTERS:
        uv.pin(c, max(freqs(c)))
    uv.revert()
    ref = load(args.iters, args.workers)
    print('Referenz-Hashes: %s' % ref, flush=True)

    # Takt-Referenz je Cluster bei 1.0 GHz (Governor auf userspace, gleiche Rate).
    base_ms = {}
    for c in uv.CLUSTERS:
        p = uv.policy(c)
        uv.write_sys(p + '/scaling_governor', 'userspace')
        uv.write_sys(p + '/scaling_setspeed', '1000000')
        time.sleep(0.3)
        base_ms[c] = bench(c)
        print('Takt-Referenz %-6s @1000000 kHz: %.3f s' % (c, base_ms[c]), flush=True)
    uv.revert()

    results = []
    t0 = time.time()
    aborted = False
    try:
        for c in clusters:
            p = uv.policy(c)
            other = 'little' if c == 'big' else 'big'
            # Anderer Cluster bleibt auf Maximum: es wird nur einer getestet.
            uv.pin(other, max(freqs(other)))
            # userspace zuerst -> die aktuelle Frequenz bleibt stehen, schedutil
            # kann nicht schon auf den neuen niedrigen Punkt springen.
            uv.write_sys(p + '/scaling_governor', 'userspace')
            uv.write_sys(p + '/scaling_min_freq', str(min(low[c])))
            uv.write_sys(p + '/scaling_max_freq', str(max(freqs(c))))
            for f in sorted(low[c], reverse=True):
                uv.write_sys(p + '/scaling_setspeed', f)
                time.sleep(0.3)
                cur = int(uv.read_sys(p + '/scaling_cur_freq'))
                real = real_khz(c)
                time.sleep(args.dwell)
                res = load(args.iters, args.workers)
                bad = [s for s, h in res.items() if h != ref.get(s)]
                cur2 = int(uv.read_sys(p + '/scaling_cur_freq'))
                real2 = real_khz(c)
                t = bench(c)
                # Erwarteter Faktor: 1.0 GHz / Zielrate.
                exp = 1000000.0 / f
                ratio = t / base_ms[c] if base_ms[c] else 0
                # Massgeblich ist der ECHTE Takt: meldet der Mux eine andere Rate
                # als angefordert, ist der OPP-Punkt unbrauchbar (Spannung passt
                # nicht zur Rate) und wird als Fehler gewertet.
                real_ok = (real is not None and abs(real - f) <= TOL_KHZ
                           and abs(real2 - f) <= TOL_KHZ)
                ok = (not bad and abs(cur - f) <= TOL_KHZ and abs(cur2 - f) <= TOL_KHZ
                      and real_ok)
                results.append(dict(cluster=c, want_khz=f, got_khz=cur, got2_khz=cur2,
                                    real_khz=real, real2_khz=real2, errors=len(bad),
                                    bench_s=round(t, 3), ratio=round(ratio, 2), ok=ok))
                print('%-6s soll=%7d  meldet=%d/%d  real=%s/%s  fehler=%d  %.3fs '
                      '(x%.1f, erwartet x%.1f)  %s'
                      % (c, f, cur, cur2, real, real2, len(bad), t, ratio, exp,
                         'OK' if ok else 'FEHLER'), flush=True)
                with open(RESULT, 'w') as fh:
                    json.dump(results, fh, indent=2)
                if not ok:
                    print('Abbruch: erster instabiler Punkt, Rest wird nicht getestet.',
                          flush=True)
                    aborted = True
                    break
            if aborted:
                break
    finally:
        restore()

    bad = [r for r in results if not r['ok']]
    print('--- Ergebnis ---')
    print('Dauer %.1fs, geprueft %d Punkte, Fehler %d' % (
        time.time() - t0, len(results), len(bad)))
    print(json.dumps(results, indent=2))
    print('ZUSTAND: %s' % ('INSTABIL' if bad else 'STABIL'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
