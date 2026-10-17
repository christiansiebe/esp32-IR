#!/usr/bin/env python3
"""Nicht-persistenter Test: laesst sich die PWM-Periode aendern?

Der CPU-Regler ist ein PWM-Regler. Das Register haelt ((period-duty)<<16)|duty,
die Spannung folgt dem TASTVERHAELTNIS:

    V = 680 mV + (duty / period) * 360 mV

Bei duty = 0 ist V = 680 mV - nach dieser Formel unabhaengig von der Periode.
Die Periode ist also KEIN Hebel fuer die Untergrenze, sondern die
Schaltfrequenz des Reglers. Der Test prueft trotzdem empirisch:

  A) ist das Periodenfeld ueberhaupt beschreibbar (bleibt der Wert stehen)?
  B) toleriert das Board eine andere PWM-Frequenz ohne Rechenfehler?

Was der Test zeigen KANN: Schreibbarkeit, Stabilitaet, PWM-Frequenz.
Was er NICHT kann: beweisen, dass die Spannung faellt (kein Spannungssensor).

Alles nur im Register -> ein Reboot stellt den Werkszustand her.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uv
import uvstress as us

LOG = '/root/pwm_period_test.log'
MAX_PWM_HZ = 5_000_000        # Schutz: keine absurden PWM-Frequenzen


def log(msg):
    line = '%s %s' % (time.strftime('%H:%M:%S'), msg)
    print(line, flush=True)
    with open(LOG, 'a') as f:
        f.write(line + '\n')
        f.flush()
        os.fsync(f.fileno())


def read_period(c):
    """Periode = (reg>>16) + duty, weil reg = ((period-duty)<<16)|duty."""
    reg = uv.read_reg(c)
    return (reg >> 16) + (reg & 0xFFFF)


def pwm_clk_hz(c):
    base = uv.CLUSTERS[c]['base']
    for suffix in ('div0', 'mux0'):
        try:
            return int(open('/sys/kernel/debug/clk/%x.pwm#%s/clk_rate'
                            % (base, suffix)).read())
        except OSError:
            continue
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cluster', default='big', choices=['big', 'little'])
    ap.add_argument('--freq', type=int, default=666666)
    ap.add_argument('--iters', type=int, default=200000)
    ap.add_argument('--workers', type=int, default=4)
    args = ap.parse_args()

    c = args.cluster
    uv.pin(c, args.freq)
    reg0 = uv.read_reg(c)
    p0 = read_period(c)
    d0 = uv.read_duty(c)
    clk = pwm_clk_hz(c)
    log('=== PWM-Perioden-Test: Cluster %s @ %d kHz ===' % (c, args.freq))
    log('  Register=0x%08x  Periode=%d  Duty=%d  frac=%.4f'
        % (reg0, p0, d0, d0 / p0))
    log('  PWM-Takt=%s Hz  ->  PWM-Frequenz %.2f kHz'
        % (clk, (clk / p0 / 1000.0 if clk else 0)))

    ref = us.load(args.iters, args.workers)
    log('  Referenz-Hash (Werksspannung): %s' % ref)

    cands = []
    for factor in (1, 2, 4, 8, 16, 0.5, 0.25):
        p = int(round(p0 * factor))
        if p < 2 or p > 0xFFFF:
            continue
        if clk and (clk / p) > MAX_PWM_HZ:
            continue
        if p not in cands:
            cands.append(p)

    log('--- A) Tastverhaeltnis (frac) erhalten, Periode aendern ---')
    for p in cands:
        d = max(0, min(p, round(d0 * p / p0)))
        uv.write_reg(c, ((p - d) << 16) | d)
        time.sleep(0.1)
        gp, gd = read_period(c), uv.read_duty(c)
        res = us.load(args.iters, args.workers)
        bad = [s for s, h in res.items() if h != ref.get(s)]
        log('  P=%-6d duty=%-6d frac=%.4f PWM=%9.1fkHz  gelesen P=%d duty=%d  %s'
            % (p, d, d / p, (clk / p / 1000.0 if clk else 0), gp, gd,
               'OK' if not bad else 'RECHENFEHLER=%d' % len(bad)))

    log('--- B) Bodenfall duty=0, Periode aendern ---')
    for p in cands:
        uv.write_reg(c, ((p - 0) << 16) | 0)
        time.sleep(0.1)
        gp, gd = read_period(c), uv.read_duty(c)
        res = us.load(args.iters, args.workers)
        bad = [s for s, h in res.items() if h != ref.get(s)]
        log('  P=%-6d duty=%d PWM=%9.1fkHz  gelesen P=%d  %s'
            % (p, gd, (clk / p / 1000.0 if clk else 0), gp,
               'OK' if not bad else 'RECHENFEHLER=%d' % len(bad)))

    uv.write_reg(c, reg0)
    log('Register wiederhergestellt: 0x%08x (P=%d duty=%d)'
        % (uv.read_reg(c), read_period(c), uv.read_duty(c)))
    uv.revert()
    log('=== fertig, Governor zurueckgesetzt ===')


if __name__ == '__main__':
    main()
