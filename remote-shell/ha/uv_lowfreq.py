#!/usr/bin/env python3
"""Sucht die minimale stabile Spannung je Tief-Takt (Odroid N2+).

Die neuen OPP-Punkte unter 1 GHz tragen die Spannung des 1.0-GHz-Punkts
(big 730 mV, little 763 mV), obwohl der Chip bei 24-250 MHz deutlich weniger
braucht. Dieses Skript senkt die Spannung je Tief-Takt schrittweise und prueft
nach jeder Stufe die Rechenergebnisse per Hash - eine zu niedrige Spannung
erzeugt zuerst stille Bitfehler, lange bevor das System haengt.

Warum nur bei fester Frequenz: `cpufreq-dt` besitzt den Regler und setzt die
Werksspannung bei jedem Frequenzwechsel neu. Der Undervolt haelt daher nur mit
dem userspace-Governor und festgenagelter Frequenz. Fuer den schnellen
Frequenzwechsel gibt es den Stresstest am Ende.

Bewusst NICHT persistent. Der Hardware-Watchdog faengt einen Hang auf. Vor
jedem Schritt wird der Fortschritt mit fsync auf Platte geschrieben, damit nach
einem Reboot sichtbar ist, bei welcher Stufe es haengte.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv        # noqa: E402
import uvstress as us   # noqa: E402  (Hash-Last aus dem bestehenden Stresstest)

LOW_FREQS = [24000, 50000, 100000, 250000]
STEPS_MV = [0, 20, 40, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 260,
            280, 300, 320, 340, 360, 380]
MIN_UV = 680000          # Regler-Boden (V_MIN) - tiefer geht das PWM nicht
LOG = '/root/uv_lowfreq.jsonl'
LOCK = '/root/uv_lowfreq.lock'   # verhindert zwei gleichzeitige Laeufe


def take_lock():
    """Nur EIN Lauf darf Spannung/Frequenz schreiben.

    Zwei gleichzeitige Instanzen sind gefaehrlich: die eine setzt beim
    `revert()` den Governor zurueck auf schedutil (-> 2.4 GHz), waehrend die
    andere die Spannung noch abgesenkt hat. Genau so ist das Board schon
    zweimal gehangen und vom Watchdog rebootet worden.
    """
    fh = open(LOCK, 'w')
    try:
        import fcntl
        fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except (ImportError, OSError):
        raise SystemExit('Es laeuft bereits ein uv_lowfreq-Lauf (%s)' % LOCK)
    fh.write(str(os.getpid()))
    fh.flush()
    return fh


def note(rec):
    """Fortschritt sofort auf Platte (fsync) - ueberlebt einen Watchdog-Reboot."""
    with open(LOG, 'a') as fh:
        fh.write(json.dumps(rec) + '\n')
        fh.flush()
        os.fsync(fh.fileno())


def done_points():
    """Bereits fertig gemessene (cluster, freq) aus dem Log - fuer den Resume."""
    out = {}
    if not os.path.exists(LOG):
        return out
    with open(LOG) as fh:
        for line in fh:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get('event') == 'result':
                out[(r['cluster'], r['freq'])] = r
    return out


def failed_points():
    """Punkte, bei denen eine Stufe Rechenfehler zeigte (aus dem Log)."""
    out = set()
    if not os.path.exists(LOG):
        return out
    with open(LOG) as fh:
        for line in fh:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if r.get('ok') is False:
                out.add((r['cluster'], r['freq']))
    return out


def steps_for(stock_uv):
    """Stufen bis zum Regler-Boden - die hohen Takte brauchen mehr als 120 mV."""
    return [mv for mv in STEPS_MV if stock_uv - mv * 1000 >= MIN_UV] or [0]


def iters_for(c, seconds):
    """Iterationen so waehlen, dass die Last etwa `seconds` dauert.

    Der LCG in Python ist bei 24 MHz rund 80x langsamer als bei 2 GHz; eine
    feste Iterationszahl wuerde den Tief-Takt-Test unnoetig in die Laenge
    ziehen. Die Zeit ist das Budget, die Arbeit waechst mit dem Takt.
    """
    t0 = time.time()
    us.digest(0, 5000)
    dt = max(time.time() - t0, 1e-6)
    return max(60_000, int(5000 / dt * seconds))


def find_min_uv(c, freq, seconds, workers):
    """Kleinste stabile Spannung fuer (Cluster, Takt).

    Die Referenz wird fuer DIESEN Takt bei Werksspannung gemessen. Der Hash
    haengt nur von (Seed, Iterationen) ab, ist also frequenzunabhaengig - aber
    die Iterationszahl wird je Takt gewaehlt, deshalb muss die Referenz mit
    derselben Zahl entstehen. Eine globale Referenz waere hier falsch.
    """
    p = uv.policy(c)
    uv.pin(c, freq)                     # userspace, Frequenz fest
    stock_duty = uv.read_duty(c)
    stock_uv = round(uv.duty_to_uv(c, stock_duty))
    iters = iters_for(c, seconds)
    ref = us.load(iters, workers)       # Referenz bei Werksspannung, gleicher Takt
    best = None
    for step in steps_for(stock_uv):
        target = max(MIN_UV, stock_uv - step * 1000)
        # VOR dem Setzen protokollieren: haengt das Board an genau dieser Stufe,
        # steht sie schon auf Platte (der naechste Lauf weiss es dann).
        note(dict(event='try', cluster=c, freq=freq, step_mv=step,
                  target_uv=target, iters=iters))
        uv.write_duty(c, uv.uv_to_duty(c, target))
        time.sleep(0.05)
        res = us.load(iters, workers)
        bad = [s for s, h in res.items() if h != ref.get(s)]
        real_uv = round(uv.duty_to_uv(c, uv.read_duty(c)))
        note(dict(cluster=c, freq=freq, step_mv=step, target_uv=target,
                  real_uv=real_uv, iters=iters, mismatch=len(bad),
                  ok=not bad))
        print('  %s %7d kHz  %4d mV  -> %s' %
              (c, freq, real_uv // 1000, 'OK' if not bad else
               'FEHLER(%d)' % len(bad)), flush=True)
        if bad:
            break
        best = real_uv
        if target <= MIN_UV:
            break                        # Regler-Boden erreicht
    uv.write_duty(c, stock_duty)         # Cluster wieder auf Werksspannung
    return stock_uv, best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seconds', type=float, default=2.0,
                    help='Rechenlast je Stufe')
    ap.add_argument('--workers', type=int, default=4)
    ap.add_argument('--freqs', default=','.join(str(f) for f in LOW_FREQS))
    ap.add_argument('--skip-rapid', action='store_true')
    ap.add_argument('--rapid-full', action='store_true',
                    help='beim Schnellwechsel die GANZE Tabelle springen '
                         '(inkl. hoher Takte), nicht nur die Tief-Takte')
    ap.add_argument('--soak', action='store_true',
                    help='am Regler-Boden laenger und mit mehr Last nachpruefen')
    ap.add_argument('--all', action='store_true',
                    help='alle Takte beider Cluster statt nur die Tief-Takte')
    ap.add_argument('--resume', action='store_true',
                    help='bereits im Log stehende Punkte ueberspringen')
    ap.add_argument('--stop-on-fail', action='store_true',
                    help='nach dem ersten Punkt mit Rechenfehlern abbrechen')
    ap.add_argument('--summary', action='store_true',
                    help='nur das Log auswerten und die Tabelle zeigen')
    args = ap.parse_args()

    if args.summary:
        res = {}
        for (c, f), r in done_points().items():
            res['%s@%d' % (c, f)] = dict(stock_uv=r['stock_uv'],
                                         min_uv=r['min_uv'])
        print_summary(res)
        bad = failed_points()
        if bad:
            print('Punkte mit Rechenfehlern: %s'
                  % ', '.join('%s@%d' % p for p in sorted(bad)))
        return

    _lock = take_lock()                  # bis Prozessende halten

    if args.all:
        freqs = None                     # je Cluster die volle Tabelle
    else:
        freqs = [int(f) for f in args.freqs.split(',')]
    for c in uv.CLUSTERS:
        uv.pin(c, None)
    note(dict(event='start', freqs=freqs or 'all', seconds=args.seconds))

    have = done_points() if args.resume else {}
    if have:
        print('Resume: %d Punkte bereits gemessen' % len(have), flush=True)

    results = {}
    for c in uv.CLUSTERS:
        pool = us.freqs(c) if freqs is None else freqs
        for f in pool:
            if f not in us.freqs(c):
                print('%s %d nicht verfuegbar' % (c, f), flush=True)
                continue
            if (c, f) in have:
                r = have[(c, f)]
                results['%s@%d' % (c, f)] = dict(stock_uv=r['stock_uv'],
                                                 min_uv=r['min_uv'])
                print('=> %s %d kHz: uebersprungen (schon gemessen)' % (c, f),
                      flush=True)
                continue
            stock_uv, best = find_min_uv(c, f, args.seconds, args.workers)
            results['%s@%d' % (c, f)] = dict(stock_uv=stock_uv, min_uv=best)
            note(dict(event='result', cluster=c, freq=f, stock_uv=stock_uv,
                      min_uv=best))
            print('=> %s %d kHz: Werksspannung %d mV, stabil bis %s mV' %
                  (c, f, stock_uv // 1000,
                   best // 1000 if best else 'n/a'), flush=True)
            if args.stop_on_fail and best is None:
                print('Abbruch nach erstem Fehlerpunkt.', flush=True)
                break

    if not args.skip_rapid:
        # Schneller Frequenzwechsel mit dem je Takt gefundenen Undervolt.
        # WICHTIG: die Spannung ist frequenzabhaengig - eine einzige Spannung
        # fuer die ganze Tabelle hat das Board schon einmal gehaengt (2.4 GHz
        # braucht 1040 mV, 24 MHz kommt mit 680 mV aus).
        safe = {}
        for k, r in results.items():
            c, f = k.split('@')
            if r['min_uv']:
                safe[(c, int(f))] = r['min_uv']
        if safe:
            lo = min(safe.values()) // 1000
            print('--- Schnellwechsel-Test, je Takt eigene Spannung (min %d mV) ---'
                  % lo, flush=True)
            for c in uv.CLUSTERS:
                uv.write_sys(uv.policy(c) + '/scaling_governor', 'userspace')
            # Referenz bei hoechstem Takt, Werksspannung (gleiche Iterationen).
            it = 400_000
            for c in uv.CLUSTERS:
                uv.pin(c, max(us.freqs(c)))
            ref = us.load(it, args.workers)
            end = time.time() + 30
            i = 0
            while time.time() < end:
                for c in uv.CLUSTERS:
                    pool = us.freqs(c) if (args.rapid_full or freqs is None) else \
                        [f for f in freqs if f in us.freqs(c)]
                    if not pool:
                        continue
                    f = pool[i % len(pool)]
                    uv.write_sys(uv.policy(c) + '/scaling_setspeed', f)
                    time.sleep(0.02)
                    tgt = safe.get((c, f))
                    if tgt:              # nur gemessene Punkte absenken
                        uv.write_duty(c, uv.uv_to_duty(c, tgt))
                i += 1
            res = us.load(it, args.workers)
            bad = [s for s, h in res.items() if h != ref.get(s)]
            note(dict(event='rapid', switches=i, mismatch=len(bad),
                      full=args.rapid_full))
            print('Schnellwechsel%s: %d Spruenge, Rechenfehler=%d' %
                  (' (volle Tabelle)' if args.rapid_full else '',
                   i, len(bad)), flush=True)

    if args.soak:
        # Am Regler-Boden laenger und mit mehr Last nachpruefen: ein Fehler
        # durch zu wenig Spannung zeigt sich oft erst unter Dauerlast. Referenz
        # und Messung laufen bei DEMSELBEN Takt, nur die Spannung unterscheidet
        # sich - sonst waere der Vergleich wertlos.
        f = freqs[0] if freqs else us.freqs('big')[0]
        it = 400_000
        for c in uv.CLUSTERS:
            uv.pin(c, f)
            uv.write_duty(c, uv.read_duty(c))     # Werksspannung
        ref = us.load(it, args.workers)
        print('--- Soak: 60 s Volllast bei %d kHz am Boden (680 mV) ---' % f,
              flush=True)
        for c in uv.CLUSTERS:
            uv.write_duty(c, uv.uv_to_duty(c, 680000))
        t0 = time.time()
        total = 0
        while time.time() - t0 < 60:
            res = us.load(it, args.workers)
            total += sum(1 for s, h in res.items() if h != ref.get(s))
        note(dict(event='soak', freq=f, seconds=60, mismatch=total))
        print('Soak: 60 s, Rechenfehler=%d' % total, flush=True)

    uv.revert()
    print('--- Ergebnis ---')
    print(json.dumps(results, indent=2))
    note(dict(event='done', results=results))
    print_summary(results)


def print_summary(results):
    """Tabelle: je Takt Werksspannung, stabile Spannung und Absenkung."""
    print('--- Uebersicht ---')
    print('%-7s %9s %10s %10s %9s' %
          ('Cluster', 'kHz', 'Werks-mV', 'stabil-mV', 'Absenk-mV'))
    for c in uv.CLUSTERS:
        keys = [k for k in results if k.startswith(c + '@')]
        keys.sort(key=lambda k: int(k.split('@')[1]))
        for k in keys:
            r = results[k]
            f = int(k.split('@')[1])
            stock = r['stock_uv'] // 1000
            mn = r['min_uv'] // 1000 if r['min_uv'] else None
            print('%-7s %9d %10d %10s %9s' %
                  (c, f, stock, mn if mn else 'FEHLER',
                   stock - mn if mn else 'n/a'))


if __name__ == '__main__':
    main()
